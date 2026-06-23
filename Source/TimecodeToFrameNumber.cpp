// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "nosMediaio/ANC_generated.h"
#include "Timing.hpp"

namespace nos::mediaio
{

namespace
{
uint32_t TCToFrameNumber(uint8_t hours, uint8_t minutes, uint8_t seconds, uint8_t frames,
	float fps, bool dropFrame)
{
	const int fpsRound = std::max(1, int(std::lround(fps)));
	const uint32_t totalSec = (uint32_t(hours) * 60u + minutes) * 60u + seconds;
	if (!dropFrame)
		return totalSec * uint32_t(fpsRound) + frames;

	// SMPTE drop-frame: drop 2 frames at the start of every minute except every
	// 10th minute (29.97). Scales linearly with rate (4 dropped per minute at 59.94).
	const int dropPerMin = int(std::lround(fps * 0.066666f));
	const int framesPerMin = fpsRound * 60 - dropPerMin;
	const int framesPer10Min = framesPerMin * 10 + dropPerMin;
	const int totalMin = int(hours) * 60 + minutes;
	return uint32_t(framesPer10Min) * uint32_t(totalMin / 10)
		+ uint32_t(framesPerMin) * uint32_t(totalMin % 10)
		+ uint32_t(seconds) * uint32_t(fpsRound)
		+ frames;
}
} // namespace

struct TimecodeToFrameNumberNode : NodeContext
{

	nosResult ExecuteNode(NodeExecuteParams const& params) override
	{
		const Timecode* tc = params.GetPinValue<Timecode>(NOS_NAME_STATIC("Timecode"));
		if (!tc)
			return NOS_RESULT_FAILED;

		float frameRate = *params.GetPinValue<float>(NOS_NAME_STATIC("FrameRateOverride"));
		if (frameRate <= 0.0f)
			frameRate = FrameRateFromTiming(params.RawParams, 60.0f);

		const bool isNtscFamily =
			std::abs(frameRate - 29.97f) < 0.05f ||
			std::abs(frameRate - 59.94f) < 0.05f;
		const bool effectiveDropFrame = tc->drop_frame() && isNtscFamily;

		const uint32_t frameNumber = TCToFrameNumber(
			tc->hours(), tc->minutes(), tc->seconds(), tc->frames(),
			frameRate, effectiveDropFrame);
		SetPinValue(NOS_NAME_STATIC("FrameNumber"), nos::Buffer::From(frameNumber));
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterTimecodeToFrameNumber(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("TimecodeToFrameNumber"), TimecodeToFrameNumberNode, fn)
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::mediaio
