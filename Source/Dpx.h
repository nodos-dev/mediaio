// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "nosMediaio/ANC_generated.h"

// Minimal DPX (SMPTE ST 268M) reader/writer for uncompressed RGB/RGBA frames.
// Files are written little-endian so header fields and pixel data go to disk with no
// byte-swapping, and the pixel data is the texture readback buffer verbatim.
// Header field offsets follow the DPX V2.0 spec (SMPTE ST 268M); see the field/offset table in
// FADGI "Guidelines for Embedded Metadata within DPX Files":
// https://www.digitizationguidelines.gov/audio-visual/documents/DPX_Embed_Guideline_20180507.pdf
namespace nos::mediaio::dpx
{

// The DPX header is a fixed 2048 bytes; image data follows immediately after.
constexpr uint32_t HEADER_SIZE = 2048;

// DPX "Transfer Characteristic" / "Colorimetric Specification" codes (SMPTE ST 268M). Both
// header fields share this one enumeration. Values 2, 3, 11, 12 describe amplitude transfer
// functions only and are NOT valid colorimetric specifications - the colorimetric field takes
// user-defined (0), printing density (1), unspecified video (4), or a standard primaries code
// (5..10). Codes match OpenImageIO's libdpx (DPXHeader.h "Characteristic").
constexpr uint8_t CHARACTERISTIC_USER_DEFINED      = 0;
constexpr uint8_t CHARACTERISTIC_PRINTING_DENSITY  = 1;
constexpr uint8_t CHARACTERISTIC_LINEAR            = 2;    // transfer only
constexpr uint8_t CHARACTERISTIC_LOGARITHMIC       = 3;    // transfer only
constexpr uint8_t CHARACTERISTIC_UNSPECIFIED_VIDEO = 4;
constexpr uint8_t CHARACTERISTIC_SMPTE274M         = 5;
constexpr uint8_t CHARACTERISTIC_ITUR709           = 6;    // Rec.709 (CCIR 709-1)
constexpr uint8_t CHARACTERISTIC_ITUR601_625       = 7;    // Rec.601 system B/G
constexpr uint8_t CHARACTERISTIC_ITUR601_525       = 8;    // Rec.601 system M
constexpr uint8_t CHARACTERISTIC_NTSC              = 9;
constexpr uint8_t CHARACTERISTIC_PAL               = 10;
constexpr uint8_t CHARACTERISTIC_UNDEFINED         = 0xFF; // field not specified

// Aliases for the transfer field, kept for existing call sites.
constexpr uint8_t TRANSFER_USER_DEFINED = CHARACTERISTIC_USER_DEFINED; // e.g. sRGB-encoded pixels
constexpr uint8_t TRANSFER_LINEAR       = CHARACTERISTIC_LINEAR;

// Frame-rate field offsets. DPX has two standard R32 (float) frame-rate fields and no integer
// rational: the motion-picture film header's (the canonical rate most DPX tools read) and the
// television header's (companion to the timecode, which also lives in the TV header). Fractional
// rates (59.94, 29.97) round to the nearest float, so playback compares them with a small tolerance.
constexpr uint32_t FILM_FRAME_RATE_OFFSET = 1724; // film header R32 frame rate (FPS), canonical
constexpr uint32_t TV_FRAME_RATE_OFFSET = 1940;   // television header R32 frame rate (FPS)

// Pixel layout of the DPX image element that Record/Playback Clip handle.
struct ImageDesc
{
	uint32_t Width = 0;
	uint32_t Height = 0;
	uint8_t Channels = 0;                 // 3 = RGB, 4 = RGBA
	uint8_t BitDepth = 0;                 // 8, 10 or 16
	uint8_t Transfer = TRANSFER_LINEAR;   // transfer characteristic (CHARACTERISTIC_*)
	uint8_t Colorimetric = CHARACTERISTIC_UNDEFINED; // colorimetric specification (CHARACTERISTIC_*)
	float FrameRate = 0.0f;               // frames per second; 0 when the file records no rate
};

inline uint64_t ImageDataSize(const ImageDesc& d)
{
	// 10-bit RGB is stored DPX Method A: one 32-bit word per pixel (3x10 bits + 2 pad).
	if (d.BitDepth == 10)
		return uint64_t(d.Width) * d.Height * 4;
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
inline float GetF32(const uint8_t* p, bool be) { uint32_t v = GetU32(p, be); float f; std::memcpy(&f, &v, 4); return f; }
} // namespace detail

// Builds a little-endian DPX header into `out` (HEADER_SIZE bytes), embedding `tc` as the
// SMPTE timecode in the television header. `frameRate` (FPS) is written to both standard
// frame-rate fields so the HH:MM:SS:FF timecode is unambiguous; pass <= 0 to leave it unset.
inline void WriteHeader(uint8_t* out, const ImageDesc& d, const Timecode& tc, float frameRate)
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
	std::memcpy(out + 160, "Nodos WriteDPX", 14); // creator
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
	uint32_t refHigh = d.BitDepth == 16 ? 65535u : (d.BitDepth == 10 ? 1023u : 255u);
	PutU32(e + 12, refHigh);                      // reference high data
	PutF32(e + 16, 1.0f);                         // reference high quantity
	e[20] = d.Channels == 4 ? 51 : 50;            // descriptor - RGBA / RGB
	e[21] = d.Transfer;                           // transfer characteristic
	e[22] = d.Colorimetric;                       // colorimetric specification
	e[23] = d.BitDepth;                           // bit depth
	// Packing: 10-bit RGB is filled to 32-bit words (Method A); 8/16-bit are tightly packed.
	PutU16(e + 24, d.BitDepth == 10 ? 1 : 0);     // packing
	PutU16(e + 26, 0);                            // encoding - uncompressed
	PutU32(e + 28, HEADER_SIZE);                  // offset to data
	PutU32(e + 32, 0);                            // end-of-line padding
	PutU32(e + 36, 0);                            // end-of-image padding

	// Television information header - SMPTE timecode, BCD-packed as 0xHHMMSSFF.
	auto bcd = [](unsigned v) { return uint32_t(((v / 10) << 4) | (v % 10)); };
	PutU32(out + 1920, (bcd(tc.hours()) << 24) | (bcd(tc.minutes()) << 16)
					   | (bcd(tc.seconds()) << 8) | bcd(tc.frames()));
	// Frame rate the FF field counts against, written to both standard R32 fields (the film
	// header's canonical field and the television header's). 0 means unknown (variable-step).
	if (frameRate > 0.0f)
	{
		PutF32(out + FILM_FRAME_RATE_OFFSET, frameRate);
		PutF32(out + TV_FRAME_RATE_OFFSET, frameRate);
	}
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
	d.Transfer = e[21];
	d.Colorimetric = e[22];
	d.FrameRate = GetF32(hdr + FILM_FRAME_RATE_OFFSET, bigEndian); // canonical frame rate (0 when unset)
	return d.Width != 0 && d.Height != 0;
}

} // namespace nos::mediaio::dpx
