// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>

#include "ANC_generated.h"
#include "Timing.hpp"

namespace nos::mediaio
{

namespace
{
struct DecodedTC
{
	uint8_t Hours;
	uint8_t Minutes;
	uint8_t Seconds;
	uint8_t Frames;
	bool DropFrame;
	bool ColorFrame;
	uint8_t DBB1Type; // bits 2-0 of DBB1: 0=LTC, 1=VITC1, 2=VITC2, ...
	uint8_t FieldID;  // HFR pair-encoding field flag (0/1); always 0 below 40 fps.
};

// SMPTE ST 12-2 ATC payload layout (16 user data words, one byte per UDW).
// TC nibbles live in the HIGH nibble of even-indexed bytes; BG nibbles live
// in the HIGH nibble of odd-indexed bytes; DBB1/DBB2 are reconstructed from
// bit 3 of each UDW (LSB first), DBB1 from bytes 0-7, DBB2 from bytes 8-15.
//   [0]  frame units            [1]  BG1
//   [2]  frame tens + DF + CF   [3]  BG2
//   [4]  second units           [5]  BG3
//   [6]  second tens + BG flag  [7]  BG4
//   [8]  minute units           [9]  BG5
//   [10] minute tens + BG flag  [11] BG6
//   [12] hour units             [13] BG7
//   [14] hour tens + flags      [15] BG8
//
// HFR FieldID (frame rates >= 40 fps, ST 12-1:2014 Section 12.1): the wire
// frame field carries half the actual frame count (0..30) and a 1-bit
// FieldID picks even/odd of the pair. Symmetric with InjectTimecode's
// EncodeATCPayload — bit position depends on family and DBB1Type:
//   PAL family (25/50) + VITC carriage (DBB1=1/2): bit 7 of UDW2  (out[2])
//   PAL family (25/50) + LTC carriage  (DBB1=0):   bit 7 of UDW14 (out[14])
//   NTSC family / 48:                              bit 7 of UDW6  (out[6])
bool DecodeATCPayload(const uint8_t* p, size_t n, int fpsRound, DecodedTC& out)
{
	if (!p || n < 16)
		return false;
	auto hi = [&](size_t i) -> uint8_t { return uint8_t((p[i] >> 4) & 0x0F); };
	const uint8_t frameTens = hi(2);
	const uint8_t secTens   = hi(6);
	const uint8_t minTens   = hi(10);
	const uint8_t hourTens  = hi(14);
	uint8_t frames = uint8_t(hi(0) + (frameTens & 0x3) * 10);
	out.DropFrame  = (frameTens & 0x4) != 0;
	out.ColorFrame = (frameTens & 0x8) != 0;
	out.Seconds    = uint8_t(hi(4)  + (secTens   & 0x7) * 10);
	out.Minutes    = uint8_t(hi(8)  + (minTens   & 0x7) * 10);
	out.Hours      = uint8_t(hi(12) + (hourTens  & 0x3) * 10);

	// DBB1 bits 2-0 identify the ATC flavor (0=LTC, 1=VITC1, 2=VITC2). Each DBB
	// bit is bit 3 of one UDW, packed LSB-first across UDW1..UDW8.
	uint8_t dbb1 = 0;
	for (int i = 0; i < 8; ++i)
	{
		dbb1 = uint8_t(dbb1 >> 1);
		dbb1 = uint8_t(dbb1 | ((p[i] << 4) & 0x80));
	}
	out.DBB1Type = uint8_t(dbb1 & 0x07);

	if (fpsRound >= 40)
	{
		const bool isPalFamily = (fpsRound == 25 || fpsRound == 50);
		const bool isVITC = (out.DBB1Type == 1 || out.DBB1Type == 2);
		uint8_t fieldID = 0;
		if (isPalFamily && isVITC)
			fieldID = (p[2]  & 0x80) ? 1u : 0u;
		else if (isPalFamily)
			fieldID = (p[14] & 0x80) ? 1u : 0u;
		else
			fieldID = (p[6]  & 0x80) ? 1u : 0u;
		out.FieldID = fieldID;
		out.Frames  = uint8_t(frames * 2 + fieldID);
	}
	else
	{
		out.FieldID = 0;
		out.Frames  = frames;
	}

	return out.Hours < 24 && out.Minutes < 60 && out.Seconds < 60 && out.Frames < uint8_t(fpsRound);
}

bool MatchesSource(uint8_t dbb1Type, ATCSource source)
{
	switch (source)
	{
	case ATCSource::ATC_LTC:   return dbb1Type == 0;
	case ATCSource::ATC_VITC1: return dbb1Type == 1;
	case ATCSource::ATC_VITC2: return dbb1Type == 2;
	case ATCSource::Auto:
	default:                   return true;
	}
}

int AutoPriority(uint8_t dbb1Type)
{
	// Prefer LTC, then VITC1, then VITC2, then anything else.
	switch (dbb1Type)
	{
	case 0: return 0;
	case 1: return 1;
	case 2: return 2;
	default: return 3;
	}
}

ATCSource DBB1TypeToSource(uint8_t dbb1Type)
{
	switch (dbb1Type)
	{
	case 1:  return ATCSource::ATC_VITC1;
	case 2:  return ATCSource::ATC_VITC2;
	default: return ATCSource::ATC_LTC;
	}
}
} // namespace

struct ExtractTimecodeNode : NodeContext
{
	ExtractTimecodeNode(nosFbNodePtr node) : NodeContext(node) {}

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		nos::NodeExecuteParams execParams(params);
		const ANCFrame* frame = execParams.GetPinData<ANCFrame>(NOS_NAME_STATIC("ANCFrame"));
		const auto source = *execParams.GetPinData<ATCSource>(NOS_NAME_STATIC("Source"));

		// Frame rate: prefer the explicit override pin (>0); otherwise infer from the
		// path's fixed-step timing, falling back to 60 on variable-step paths.
		float frameRate = *execParams.GetPinData<float>(NOS_NAME_STATIC("FrameRateOverride"));
		if (frameRate <= 0.0f)
			frameRate = FrameRateFromTiming(params, 60.0f);

		const int fpsRound = std::max(1, int(std::lround(frameRate)));
		// Drop-frame is only defined for the NTSC fractional rates (29.97 /
		// 59.94). If a foreign device misencodes DF=1 on an integer-rate
		// timeline, ignore the flag in the emitted Timecode so consumers don't
		// apply NTSC drop-math to a non-NTSC stream.
		const bool isNtscFamily =
			std::abs(frameRate - 29.97f) < 0.05f ||
			std::abs(frameRate - 59.94f) < 0.05f;

		DecodedTC best{};
		int bestPriority = INT_MAX;
		bool found = false;

		if (frame && frame->packets())
		{
			for (const auto* pkt : *frame->packets())
			{
				// SMPTE ST 12-2 ATC: DID=0x60, SDID=0x60.
				if (!pkt || pkt->did() != 0x60 || pkt->sdid() != 0x60)
					continue;
				const auto* payload = pkt->payload();
				if (!payload)
					continue;
				DecodedTC tc{};
				if (!DecodeATCPayload(payload->data(), payload->size(), fpsRound, tc))
					continue;
				if (!MatchesSource(tc.DBB1Type, source))
					continue;
				const int prio = (source == ATCSource::Auto) ? AutoPriority(tc.DBB1Type) : 0;
				if (!found || prio < bestPriority)
				{
					best = tc;
					bestPriority = prio;
					found = true;
					if (source != ATCSource::Auto)
						break; // exact match, no need to keep scanning
				}
			}
		}

		if (found)
		{
			const bool effectiveDropFrame = best.DropFrame && isNtscFamily;
			Timecode out(best.Hours, best.Minutes, best.Seconds, best.Frames,
				effectiveDropFrame, DBB1TypeToSource(best.DBB1Type));
			SetPinValue(NOS_NAME_STATIC("Timecode"), nos::Buffer::From(out));
			SetPinValue(NOS_NAME_STATIC("Valid"), nos::Buffer::From(true));
		}
		else
		{
			SetPinValue(NOS_NAME_STATIC("Valid"), nos::Buffer::From(false));
		}
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterExtractTimecode(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("ExtractTimecode"), ExtractTimecodeNode, fn)
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::mediaio
