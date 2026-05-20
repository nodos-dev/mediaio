// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "ANC_generated.h"

// Minimal DPX (SMPTE ST 268M) reader/writer for uncompressed RGB/RGBA frames.
// Files are written little-endian so header fields and pixel data go to disk with no
// byte-swapping, and the pixel data is the texture readback buffer verbatim.
namespace nos::mediaio::dpx
{

// The DPX header is a fixed 2048 bytes; image data follows immediately after.
constexpr uint32_t HEADER_SIZE = 2048;

// Pixel layout of the DPX image element that Record/Playback Clip handle.
struct ImageDesc
{
	uint32_t Width = 0;
	uint32_t Height = 0;
	uint8_t Channels = 0; // 3 = RGB, 4 = RGBA
	uint8_t BitDepth = 0; // 8 or 16
};

inline uint64_t ImageDataSize(const ImageDesc& d)
{
	return uint64_t(d.Width) * d.Height * d.Channels * (d.BitDepth / 8);
}

// "HH-MM-SS-FF.dpx" file name for a timecode.
inline std::string TimecodeToFileName(const Timecode& tc)
{
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%02u-%02u-%02u-%02u.dpx",
		unsigned(tc.hours()), unsigned(tc.minutes()),
		unsigned(tc.seconds()), unsigned(tc.frames()));
	return std::string(buf);
}

namespace detail
{
inline void PutU32(uint8_t* p, uint32_t v)
{
	p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
inline void PutU16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void PutF32(uint8_t* p, float f) { uint32_t v; std::memcpy(&v, &f, 4); PutU32(p, v); }
inline uint32_t GetU32(const uint8_t* p, bool be)
{
	return be ? (uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3])
			  : (uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]);
}
inline uint16_t GetU16(const uint8_t* p, bool be)
{
	return be ? uint16_t(uint16_t(p[0]) << 8 | p[1]) : uint16_t(uint16_t(p[1]) << 8 | p[0]);
}
} // namespace detail

// Builds a little-endian DPX header into `out` (HEADER_SIZE bytes), embedding `tc` as the
// SMPTE timecode in the television header.
inline void WriteHeader(uint8_t* out, const ImageDesc& d, const Timecode& tc)
{
	using namespace detail;
	std::memset(out, 0, HEADER_SIZE);

	// File information header.
	std::memcpy(out, "XPDS", 4);                  // magic - little-endian DPX
	PutU32(out + 4, HEADER_SIZE);                 // offset to image data
	std::memcpy(out + 8, "V2.0", 4);              // version
	PutU32(out + 16, uint32_t(HEADER_SIZE + ImageDataSize(d))); // total file size
	PutU32(out + 20, 1);                          // ditto key
	PutU32(out + 24, 1664);                       // generic header length
	PutU32(out + 28, 384);                        // industry header length
	PutU32(out + 32, 0);                          // user header length
	std::memcpy(out + 160, "Nodos Record Clip", 17); // creator
	PutU32(out + 660, 0xFFFFFFFFu);               // encryption key - unencrypted

	// Image information header.
	PutU16(out + 768, 0);                         // orientation - top-left origin
	PutU16(out + 770, 1);                         // number of image elements
	PutU32(out + 772, d.Width);
	PutU32(out + 776, d.Height);
	uint8_t* e = out + 780;                       // image element 1
	PutU32(e + 0, 0);                             // data sign - unsigned
	PutU32(e + 4, 0);                             // reference low data
	PutF32(e + 8, 0.0f);                          // reference low quantity
	PutU32(e + 12, d.BitDepth == 16 ? 65535u : 255u); // reference high data
	PutF32(e + 16, 1.0f);                         // reference high quantity
	e[20] = d.Channels == 4 ? 51 : 50;            // descriptor - RGBA / RGB
	e[21] = 2;                                    // transfer characteristic - linear
	e[22] = 2;                                    // colorimetric - linear
	e[23] = d.BitDepth;                           // bit depth
	PutU16(e + 24, 0);                            // packing - packed
	PutU16(e + 26, 0);                            // encoding - uncompressed
	PutU32(e + 28, HEADER_SIZE);                  // offset to data
	PutU32(e + 32, 0);                            // end-of-line padding
	PutU32(e + 36, 0);                            // end-of-image padding

	// Television information header - SMPTE timecode, BCD-packed as 0xHHMMSSFF.
	auto bcd = [](unsigned v) { return uint32_t(((v / 10) << 4) | (v % 10)); };
	PutU32(out + 1920, (bcd(tc.hours()) << 24) | (bcd(tc.minutes()) << 16)
					   | (bcd(tc.seconds()) << 8) | bcd(tc.frames()));
}

// Parses a DPX header (`hdr` must hold at least HEADER_SIZE bytes). Returns false for a
// non-DPX, RLE-encoded, or otherwise unsupported file.
inline bool ReadHeader(const uint8_t* hdr, ImageDesc& d, uint32_t& dataOffset, bool& bigEndian)
{
	using namespace detail;
	if (std::memcmp(hdr, "SDPX", 4) == 0) bigEndian = true;
	else if (std::memcmp(hdr, "XPDS", 4) == 0) bigEndian = false;
	else return false;

	dataOffset = GetU32(hdr + 4, bigEndian);
	if (dataOffset < 1664)
		return false;
	d.Width = GetU32(hdr + 772, bigEndian);
	d.Height = GetU32(hdr + 776, bigEndian);

	const uint8_t* e = hdr + 780;
	if (GetU16(e + 26, bigEndian) != 0)           // encoding - reject RLE
		return false;
	uint8_t descriptor = e[20];
	if (descriptor == 51) d.Channels = 4;
	else if (descriptor == 50) d.Channels = 3;
	else return false;
	uint8_t bitDepth = e[23];
	if (bitDepth != 8 && bitDepth != 16)
		return false;
	d.BitDepth = bitDepth;
	return d.Width != 0 && d.Height != 0;
}

} // namespace nos::mediaio::dpx
