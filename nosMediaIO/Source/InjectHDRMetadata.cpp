// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "ANC_generated.h"
#include "Conversion_generated.h"

namespace nos::mediaio
{

namespace
{
// SMPTE ST 2108-1 HDR/WCG metadata ANC packet identifiers.
constexpr uint8_t S2108_DID  = 0x41;
constexpr uint8_t S2108_SDID = 0x0C;

// SMPTE ST 2108-1 frame types (Sec 5.3.1). Each frame carries one HEVC SEI
// message: mastering-display colour volume (payloadType 137) and content
// light level info (payloadType 144).
constexpr uint8_t HDR_STATIC1 = 0x00; // Mastering Display Colour Volume
constexpr uint8_t HDR_STATIC2 = 0x01; // Content Light Level Info
constexpr uint8_t SEI_MASTERING_DISPLAY = 137;
constexpr uint8_t SEI_CONTENT_LIGHT      = 144;

void PutU16(std::vector<uint8_t>& v, uint16_t x)
{
	v.push_back(uint8_t(x >> 8));
	v.push_back(uint8_t(x & 0xFF));
}

void PutU32(std::vector<uint8_t>& v, uint32_t x)
{
	v.push_back(uint8_t(x >> 24));
	v.push_back(uint8_t(x >> 16));
	v.push_back(uint8_t(x >> 8));
	v.push_back(uint8_t(x & 0xFF));
}

// ST 2086 chromaticity: increments of 0.00002, stored as uint16.
uint16_t EncodeChroma(float c)
{
	long q = std::lround(double(c) / 0.00002);
	return uint16_t(std::clamp<long>(q, 0, 0xFFFF));
}

// ST 2086 luminance: increments of 0.0001 cd/m^2, stored as uint32.
uint32_t EncodeLuminance(float nits)
{
	long long q = std::llroundf(nits / 0.0001f);
	return uint32_t(std::clamp<long long>(q, 0, 0xFFFFFFFFll));
}

uint16_t EncodeNits(uint32_t nits)
{
	return uint16_t(std::min<uint32_t>(nits, 0xFFFF));
}

// Build the ST 2108-1 user-data-word payload: a Mastering Display frame
// followed by a Content Light Level frame. All multi-byte fields big-endian.
std::vector<uint8_t> BuildPayload(const HDRMetadata& m)
{
	std::vector<uint8_t> udw;
	udw.reserve(36);

	// Frame 1 — Static Metadata Type 1 (HEVC mastering_display SEI, 24 bytes).
	udw.push_back(HDR_STATIC1);
	udw.push_back(26);                  // frame_length = 2 (SEI hdr) + 24 (payload)
	udw.push_back(SEI_MASTERING_DISPLAY);
	udw.push_back(24);                  // SEI payload size
	// HEVC orders the primaries Green, Blue, Red.
	PutU16(udw, EncodeChroma(m.green_x())); PutU16(udw, EncodeChroma(m.green_y()));
	PutU16(udw, EncodeChroma(m.blue_x()));  PutU16(udw, EncodeChroma(m.blue_y()));
	PutU16(udw, EncodeChroma(m.red_x()));   PutU16(udw, EncodeChroma(m.red_y()));
	PutU16(udw, EncodeChroma(m.white_x())); PutU16(udw, EncodeChroma(m.white_y()));
	PutU32(udw, EncodeLuminance(m.max_luminance()));
	PutU32(udw, EncodeLuminance(m.min_luminance()));

	// Frame 2 — Static Metadata Type 2 (HEVC content_light_level SEI, 4 bytes).
	udw.push_back(HDR_STATIC2);
	udw.push_back(6);                   // frame_length = 2 (SEI hdr) + 4 (payload)
	udw.push_back(SEI_CONTENT_LIGHT);
	udw.push_back(4);                   // SEI payload size
	PutU16(udw, EncodeNits(m.max_cll()));
	PutU16(udw, EncodeNits(m.max_fall()));

	return udw;
}
} // namespace

struct InjectHDRMetadataNode : NodeContext
{
	InjectHDRMetadataNode(nosFbNodePtr node) : NodeContext(node) {}

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		nos::NodeExecuteParams execParams(params);
		const ANCFrame* in = execParams.GetPinData<ANCFrame>(NOS_NAME_STATIC("ANCFrame"));
		const HDRMetadata* meta = execParams.GetPinData<HDRMetadata>(NOS_NAME_STATIC("Metadata"));
		if (!meta)
			return NOS_RESULT_FAILED;

		uint16_t lineNumber = 9;
		if (auto* p = execParams.GetPinData<uint32_t>(NOS_NAME_STATIC("LineNumber")))
			lineNumber = uint16_t(*p);

		const std::vector<uint8_t> udw = BuildPayload(*meta);

		flatbuffers::FlatBufferBuilder fbb;
		std::vector<flatbuffers::Offset<ANCPacket>> packets;

		// Forward incoming packets, dropping any existing ST 2108-1 packet
		// (DID=0x41/SDID=0x0C) so we don't emit stale HDR metadata alongside
		// the fresh one. Everything else passes through verbatim.
		if (in && in->packets())
		{
			const auto* incoming = in->packets();
			packets.reserve(incoming->size() + 1);
			for (uint32_t i = 0; i < incoming->size(); ++i)
			{
				const auto* src = incoming->Get(i);
				if (!src)
					continue;
				if (src->did() == S2108_DID && src->sdid() == S2108_SDID)
					continue;
				const auto* p = src->payload();
				auto payloadOffset = fbb.CreateVector(p ? p->data() : nullptr, p ? p->size() : 0);
				ANCPacketBuilder pb(fbb);
				pb.add_did(src->did());
				pb.add_sdid(src->sdid());
				pb.add_line_number(src->line_number());
				pb.add_horiz_offset(src->horiz_offset());
				pb.add_space(src->space());
				pb.add_channel(src->channel());
				pb.add_link(src->link());
				pb.add_stream(src->stream());
				pb.add_is_field2(src->is_field2());
				pb.add_payload(payloadOffset);
				packets.push_back(pb.Finish());
			}
		}
		else
		{
			packets.reserve(1);
		}

		// HDR/WCG static metadata is frame-level: VANC, luma, link A, field 1.
		auto hdrPayload = fbb.CreateVector(udw.data(), udw.size());
		ANCPacketBuilder pb(fbb);
		pb.add_did(S2108_DID);
		pb.add_sdid(S2108_SDID);
		pb.add_line_number(lineNumber);
		pb.add_horiz_offset(0);
		pb.add_space(ANCDataSpace::VANC);
		pb.add_channel(ANCDataChannel::Y);
		pb.add_link(ANCDataLink::A);
		pb.add_is_field2(false);
		pb.add_payload(hdrPayload);
		packets.push_back(pb.Finish());

		auto packetsVec = fbb.CreateVector(packets);
		ANCFrameBuilder frameBuilder(fbb);
		frameBuilder.add_packets(packetsVec);
		fbb.Finish(frameBuilder.Finish());

		SetPinValue(NOS_NAME_STATIC("Out"), nos::Buffer(fbb.Release()));
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterInjectHDRMetadata(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("InjectHDRMetadata"), InjectHDRMetadataNode, fn)
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::mediaio
