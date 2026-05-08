// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "ANC_generated.h"

namespace nos::mediaio
{

namespace
{
struct EncodedTC
{
	uint8_t Hours;
	uint8_t Minutes;
	uint8_t Seconds;
	uint8_t Frames;
	bool DropFrame;
};

// Frame-number to HH:MM:SS:FF, with SMPTE drop-frame correction when requested.
// Standard Andrew Duncan formulation: project N onto a non-DF timeline by adding
// back the dropped frames, then do plain modular arithmetic with fpsRound.
EncodedTC FrameNumberToTC(uint32_t n, float fps, bool dropFrame)
{
	const int fpsRound = std::max(1, int(std::lround(fps)));
	uint32_t projected = n;
	if (dropFrame)
	{
		const int dropPerMin = int(std::lround(fps * 0.066666f));
		const int framesPerMin = fpsRound * 60 - dropPerMin;
		const int framesPer10Min = framesPerMin * 10 + dropPerMin;
		const uint32_t d = n / uint32_t(framesPer10Min);
		const uint32_t m = n % uint32_t(framesPer10Min);
		uint32_t addend = uint32_t(dropPerMin) * 9u * d;
		if (m > uint32_t(dropPerMin))
			addend += uint32_t(dropPerMin) * ((m - uint32_t(dropPerMin)) / uint32_t(framesPerMin));
		projected = n + addend;
	}
	EncodedTC tc{};
	tc.DropFrame = dropFrame;
	tc.Frames    = uint8_t(projected % uint32_t(fpsRound));
	const uint32_t totalSec = projected / uint32_t(fpsRound);
	tc.Seconds   = uint8_t(totalSec % 60u);
	const uint32_t totalMin = totalSec / 60u;
	tc.Minutes   = uint8_t(totalMin % 60u);
	tc.Hours     = uint8_t((totalMin / 60u) % 24u);
	return tc;
}

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

// Inverse of DecodeATCPayload (see ExtractTimecode.cpp). 16-byte SMPTE ST 12-2
// payload: TC nibbles in HIGH nibble of even bytes, BG nibbles in HIGH nibble
// of odd bytes (we leave BG zero), DBB1 packed across bytes 0-7 / DBB2 across
// 8-15 — bit 3 of each UDW, LSB first.
void EncodeATCPayload(const EncodedTC& tc, uint8_t dbb1Type, uint8_t out[16])
{
	std::memset(out, 0, 16);
	const uint8_t frameUnits = uint8_t(tc.Frames % 10);
	const uint8_t frameTens  = uint8_t((tc.Frames / 10) & 0x3) | (tc.DropFrame ? 0x4 : 0x0);
	const uint8_t secUnits   = uint8_t(tc.Seconds % 10);
	const uint8_t secTens    = uint8_t((tc.Seconds / 10) & 0x7);
	const uint8_t minUnits   = uint8_t(tc.Minutes % 10);
	const uint8_t minTens    = uint8_t((tc.Minutes / 10) & 0x7);
	const uint8_t hourUnits  = uint8_t(tc.Hours % 10);
	const uint8_t hourTens   = uint8_t((tc.Hours / 10) & 0x3);

	out[0]  = uint8_t(frameUnits << 4);
	out[2]  = uint8_t(frameTens  << 4);
	out[4]  = uint8_t(secUnits   << 4);
	out[6]  = uint8_t(secTens    << 4);
	out[8]  = uint8_t(minUnits   << 4);
	out[10] = uint8_t(minTens    << 4);
	out[12] = uint8_t(hourUnits  << 4);
	out[14] = uint8_t(hourTens   << 4);

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
	InjectTimecodeNode(nosFbNodePtr node) : NodeContext(node) {}

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		nos::NodeExecuteParams execParams(params);
		const ANCFrame* in = execParams.GetPinData<ANCFrame>(NOS_NAME_STATIC("ANCFrame"));
		const uint32_t frameNumber = *execParams.GetPinData<uint32_t>(NOS_NAME_STATIC("FrameNumber"));
		const bool dropFrame = *execParams.GetPinData<bool>(NOS_NAME_STATIC("DropFrame"));
		const auto source = *execParams.GetPinData<ATCSource>(NOS_NAME_STATIC("Source"));

		float frameRate = *execParams.GetPinData<float>(NOS_NAME_STATIC("FrameRateOverride"));
		if (frameRate <= 0.0f)
		{
			frameRate = 60.0f;
			if (params->TimingInfo.TimingMode == NOS_EXECUTION_TIMING_MODE_FIXED_STEP)
			{
				const auto& ds = params->TimingInfo.FixedStepTiming.DeltaSeconds;
				if (ds.x != 0 && ds.y != 0)
					frameRate = float(double(ds.y) / double(ds.x));
			}
		}

		const EncodedTC tc = FrameNumberToTC(frameNumber, frameRate, dropFrame);
		const uint8_t dbb1Type = SourceToDBB1Type(source);

		uint8_t payload[16];
		EncodeATCPayload(tc, dbb1Type, payload);

		flatbuffers::FlatBufferBuilder fbb;
		std::vector<flatbuffers::Offset<ANCPacket>> packets;

		// Forward incoming packets, dropping any existing ATC of the same flavor
		// so we don't double-emit. (DID=0x60/SDID=0x60 is the ATC ANC type.)
		if (in && in->packets())
		{
			const auto* incoming = in->packets();
			packets.reserve(incoming->size() + 1);
			for (uint32_t i = 0; i < incoming->size(); ++i)
			{
				const auto* src = incoming->Get(i);
				if (!src)
					continue;
				if (src->did() == 0x60 && src->sdid() == 0x60)
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
				const auto* p = src->payload();
				const uint8_t* pdata = p ? p->data() : nullptr;
				const size_t psize = p ? p->size() : 0;
				auto payloadOffset = fbb.CreateVector(pdata, psize);
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

		// Line 9 + Y (luma) + HANC + horiz_offset=0x0FFE matches AJA's reference
		// ATC packet (AJAAncillaryData_Timecode_ATC::GeneratePayloadData and
		// SetDBB1PayloadType). 0x0FFE is AJAAncDataHorizOffset_AnyHanc — telling
		// the inserter to place the packet anywhere legal in HANC. With offset=0
		// (Unknown) the inserter mis-places the bytes and the receiver decodes
		// shifted nibbles (looks like a free-running random timecode).
		auto atcPayload = fbb.CreateVector(payload, 16);
		ANCPacketBuilder pb(fbb);
		pb.add_did(0x60);
		pb.add_sdid(0x60);
		pb.add_line_number(9);
		pb.add_horiz_offset(0x0FFE);
		pb.add_space(ANCDataSpace::HANC);
		pb.add_channel(ANCDataChannel::Y);
		pb.add_link(ANCDataLink::A);
		pb.add_payload(atcPayload);
		packets.push_back(pb.Finish());

		auto packetsVec = fbb.CreateVector(packets);
		ANCFrameBuilder frameBuilder(fbb);
		frameBuilder.add_packets(packetsVec);
		fbb.Finish(frameBuilder.Finish());

		SetPinValue(NOS_NAME_STATIC("Out"), nos::Buffer(fbb.Release()));

		char buf[16];
		std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u%c%02u",
			unsigned(tc.Hours), unsigned(tc.Minutes), unsigned(tc.Seconds),
			tc.DropFrame ? ';' : ':', unsigned(tc.Frames));
		SetPinValue(NOS_NAME_STATIC("Timecode"), nos::Buffer(buf, std::strlen(buf) + 1));

		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterInjectTimecode(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("InjectTimecode"), InjectTimecodeNode, fn)
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::mediaio
