// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "ANC_generated.h"

namespace nos::mediaio
{

namespace
{
// Frame-number to HH:MM:SS:FF, with SMPTE drop-frame correction when requested.
// Standard Andrew Duncan formulation: project N onto a non-DF timeline by adding
// back the dropped frames, then do plain modular arithmetic with fpsRound.
void FrameNumberToTC(uint32_t n, float fps, bool dropFrame,
	uint8_t& outH, uint8_t& outM, uint8_t& outS, uint8_t& outF)
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
	outF = uint8_t(projected % uint32_t(fpsRound));
	const uint32_t totalSec = projected / uint32_t(fpsRound);
	outS = uint8_t(totalSec % 60u);
	const uint32_t totalMin = totalSec / 60u;
	outM = uint8_t(totalMin % 60u);
	outH = uint8_t((totalMin / 60u) % 24u);
}
} // namespace

struct TimecodeFromFrameNumberNode : NodeContext
{
	TimecodeFromFrameNumberNode(nosFbNodePtr node) : NodeContext(node) {}

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		nos::NodeExecuteParams execParams(params);
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

		// Drop-frame is only defined for the NTSC fractional rates (29.97 /
		// 59.94). Gate on the actual fractional rate (not the rounded value)
		// so a user at exactly 30.0 or 60.0 fps with DropFrame=true does not
		// get NTSC drop-math silently applied to an integer-rate timeline.
		const bool isNtscFamily =
			std::abs(frameRate - 29.97f) < 0.05f ||
			std::abs(frameRate - 59.94f) < 0.05f;
		const bool effectiveDropFrame = dropFrame && isNtscFamily;

		uint8_t h = 0, m = 0, s = 0, f = 0;
		FrameNumberToTC(frameNumber, frameRate, effectiveDropFrame, h, m, s, f);

		Timecode tc(h, m, s, f, effectiveDropFrame,
			source == ATCSource::Auto ? ATCSource::ATC_LTC : source);
		SetPinValue(NOS_NAME_STATIC("Timecode"), nos::Buffer::From(tc));
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterTimecodeFromFrameNumber(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("TimecodeFromFrameNumber"), TimecodeFromFrameNumberNode, fn)
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::mediaio
