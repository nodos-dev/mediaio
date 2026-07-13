// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "nosMediaio/ANC_generated.h"
#include "ANCUtils.hpp"
#include "Timing.hpp"

namespace nos::mediaio
{

namespace
{
uint8_t SourceToDBB1Type(ATCSource source)
{
	switch (source)
	{
	case ATCSource::ATC_VITC1: return 1;
	case ATCSource::ATC_VITC2: return 2;
	case ATCSource::ATC_LTC:
	case ATCSource::Auto:
	default:                   return 0;
	}
}

// 16-byte SMPTE ST 12-2 payload: TC nibbles in HIGH nibble of even bytes, BG
// nibbles in HIGH nibble of odd bytes (we leave BG zero), DBB1 packed across
// bytes 0-7 / DBB2 across 8-15 — bit 3 of each UDW, LSB first.
//
// For frame rates >= 40 fps (SMPTE ST 12-1:2014 Section 12.1, HFR pair
// encoding), the on-wire frame field can only represent 0..30, so each TC
// value is shared by two consecutive video frames and a Field Identification
// bit toggles between them. The wire frame number is `frames / 2`, and the
// FieldID (= `frames % 2`) goes at a bit position that depends on both frame
// family and carriage (LTC vs VITC):
//
//   PAL family (25/50 fps):
//     - LTC carriage    (DBB1Type=0): LTC  bit 59 → UDW14 bit 7  [Table 3]
//     - VITC carriage   (DBB1Type=1/2): VITC bit 15 → UDW2  bit 7  [Table 7]
//   NTSC family (30/60 fps) and 48 fps:
//     - LTC and VITC both land at UDW6 bit 7 (LTC bit 27 / VITC bit 35).
//
// AJA's AJAAncillaryData_Timecode::SetFieldIdFlag uses the LTC bit position
// regardless of DBB1Type, which ST 12-1 Section 12.2 (Informative) calls out
// as one of "various implementations" that exist. We follow strict ST 12-1
// here so spec-conformant receivers will decode VITC field flag.
void EncodeATCPayload(uint8_t hours, uint8_t minutes, uint8_t seconds, uint8_t frames,
	bool dropFrame, uint8_t dbb1Type, int fpsRound, bool isPalFamily, uint8_t out[16])
{
	std::memset(out, 0, 16);

	const bool isHFR = fpsRound >= 40;
	uint8_t wireFrames = frames;
	uint8_t fieldID = 0;
	if (isHFR)
	{
		fieldID = uint8_t(frames & 0x1);
		wireFrames = uint8_t(frames / 2);
	}

	const uint8_t frameUnits = uint8_t(wireFrames % 10);
	const uint8_t frameTens  = uint8_t((wireFrames / 10) & 0x3) | (dropFrame ? 0x4 : 0x0);
	const uint8_t secUnits   = uint8_t(seconds % 10);
	const uint8_t secTens    = uint8_t((seconds / 10) & 0x7);
	const uint8_t minUnits   = uint8_t(minutes % 10);
	const uint8_t minTens    = uint8_t((minutes / 10) & 0x7);
	const uint8_t hourUnits  = uint8_t(hours % 10);
	const uint8_t hourTens   = uint8_t((hours / 10) & 0x3);

	out[0]  = uint8_t(frameUnits << 4);
	out[2]  = uint8_t(frameTens  << 4);
	out[4]  = uint8_t(secUnits   << 4);
	out[6]  = uint8_t(secTens    << 4);
	out[8]  = uint8_t(minUnits   << 4);
	out[10] = uint8_t(minTens    << 4);
	out[12] = uint8_t(hourUnits  << 4);
	out[14] = uint8_t(hourTens   << 4);

	if (isHFR && fieldID)
	{
		const bool isVITC = (dbb1Type == 1 || dbb1Type == 2);
		if (isPalFamily && isVITC)
			out[2]  = uint8_t(out[2]  | 0x80);  // VITC bit 15 (25-frame)
		else if (isPalFamily)
			out[14] = uint8_t(out[14] | 0x80);  // LTC bit 59 (25-frame)
		else
			out[6]  = uint8_t(out[6]  | 0x80);  // LTC bit 27 / VITC bit 35 (30-frame)
	}

	const uint8_t dbb1 = uint8_t(dbb1Type & 0x07);
	for (int i = 0; i < 8; ++i)
	{
		if ((dbb1 >> i) & 0x01)
			out[i] = uint8_t(out[i] | 0x08);
	}
}
} // namespace

struct InjectTimecodeNode : NodeContext
{

	nosResult ExecuteNode(NodeExecuteParams const& params) override
	{
		const ANCFrame* in = params.GetPinValue<ANCFrame>(NOS_NAME_STATIC("ANCFrame"));
		const Timecode* tc = params.GetPinValue<Timecode>(NOS_NAME_STATIC("Timecode"));
		if (!tc)
			return NOS_RESULT_FAILED;

		float frameRate = *params.GetPinValue<float>(NOS_NAME_STATIC("FrameRateOverride"));
		if (frameRate <= 0.0f)
			frameRate = FrameRateFromTiming(params.RawParams, 60.0f);

		const uint8_t dbb1Type = SourceToDBB1Type(tc->source());
		const int fpsRound = std::max(1, int(std::lround(frameRate)));
		// PAL family per CRP188::FormatIsPAL (25/50 fps) puts the HFR FieldID
		// at LTC bit 59; everything else (including 48 and 60) puts it at LTC
		// bit 27. Only matters when fpsRound >= 40.
		const bool isPalFamily = (fpsRound == 25 || fpsRound == 50);

		uint8_t payload[16];
		EncodeATCPayload(tc->hours(), tc->minutes(), tc->seconds(), tc->frames(),
			tc->drop_frame(), dbb1Type, fpsRound, isPalFamily, payload);

		// SMPTE ST 12-2 / RP-188 places ATC on a single field per packet:
		//   LTC   (DBB1=0): F1 — describes the whole frame.
		//   VITC1 (DBB1=1): F1.
		//   VITC2 (DBB1=2): F2 (the F2-line VITC).
		// On progressive output the F2 distinction is degenerate; downstream
		// WriteAnc collapses is_field2=true to F1 when not interlaced.
		const bool emitField2 = (dbb1Type == 2);

		flatbuffers::FlatBufferBuilder fbb;
		std::vector<flatbuffers::Offset<ANCPacket>> packets;

		// Forward incoming packets, dropping any existing ATC of the same flavor
		// (DID=0x60/SDID=0x60, same DBB1Type) ON THE SAME FIELD so we don't
		// double-emit. ATC on the other field is forwarded untouched.
		if (in && in->packets())
		{
			const auto* incoming = in->packets();
			packets.reserve(incoming->size() + 1);
			for (uint32_t i = 0; i < incoming->size(); ++i)
			{
				const auto* src = incoming->Get(i);
				if (!src)
					continue;
				if (src->did() == 0x60 && src->sdid() == 0x60 && src->is_field2() == emitField2)
				{
					const auto* p = src->payload();
					if (p && p->size() >= 16)
					{
						uint8_t existingDbb1 = 0;
						for (int b = 0; b < 8; ++b)
						{
							existingDbb1 = uint8_t(existingDbb1 >> 1);
							existingDbb1 = uint8_t(existingDbb1 | ((p->Get(b) << 4) & 0x80));
						}
						if ((existingDbb1 & 0x07) == dbb1Type)
							continue;
					}
				}
				packets.push_back(CloneANCPacket(fbb, src));
			}
		}
		else
		{
			packets.reserve(1);
		}

		// VANC line 10, Y (luma), Link A, horiz_offset 0 — matches AJA's
		// ntv2llburn reference for transmitted ATC packets (see ntv2llburn.cpp
		// F1AncDataLoc) and SMPTE ST 12M-2, which specifies ATC-LTC in VANC.
		// The HANC/AnyHanc placement that lived here previously matched AJA's
		// AJAAncillaryData_Timecode_ATC::GeneratePayloadData internal default,
		// but the AJA hardware ANC inserter doesn't surface those packets to
		// downstream SDI monitors — extractors (incl. ours) find camera ATC
		// in VANC, so the inserter has to write there too for round-trip
		// compatibility.
		auto atcPayload = fbb.CreateVector(payload, 16);
		ANCPacketBuilder pb(fbb);
		pb.add_did(0x60);
		pb.add_sdid(0x60);
		pb.add_line_number(10);
		pb.add_horiz_offset(0);
		pb.add_space(ANCDataSpace::VANC);
		pb.add_channel(ANCDataChannel::Y);
		pb.add_link(ANCDataLink::A);
		pb.add_is_field2(emitField2);
		pb.add_payload(atcPayload);
		packets.push_back(pb.Finish());

		auto packetsVec = fbb.CreateVector(packets);
		ANCFrameBuilder frameBuilder(fbb);
		frameBuilder.add_packets(packetsVec);
		fbb.Finish(frameBuilder.Finish());

		SetPinValue(NOS_NAME_STATIC("Out"), nos::Buffer(fbb.Release()));
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterInjectTimecode(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("InjectTimecode"), InjectTimecodeNode, fn)
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::mediaio
