// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#pragma once

#include <cstdio>
#include <string>

#include <Nodos/PluginHelpers.hpp>

// Shared frame-rate helpers for nodes that derive a rate from the executing path's
// timing (Record/Playback Clip and the timecode nodes).
namespace nos::mediaio
{

// Frame rate (FPS) inferred from a path's fixed-step timing. The scheduler's
// DeltaSeconds is the seconds-per-frame rational {x = numerator, y = denominator}
// (e.g. {1001, 60000} == 59.94 FPS), so FPS = y/x. Variable-step paths, and fixed-step
// paths with no nominal rate yet ({0, 0}), have no fixed rate; `fallback` is returned for
// those - pass 0.0f for "unset" or a nominal default (e.g. 60.0f) to substitute.
inline float FrameRateFromTiming(const nosNodeExecuteParams* params, float fallback = 0.0f)
{
	if (params->TimingInfo.TimingMode == NOS_EXECUTION_TIMING_MODE_FIXED_STEP)
	{
		const auto& ds = params->TimingInfo.FixedStepTiming.DeltaSeconds;
		if (ds.x != 0 && ds.y != 0)
			return float(double(ds.y) / double(ds.x));
	}
	return fallback;
}

// A frame rate formatted for display with 4 significant digits ("59.94", "60", "23.98") -
// enough to tell the fractional NTSC rates from their integer neighbours.
inline std::string FrameRateToString(float fps)
{
	char buf[16];
	std::snprintf(buf, sizeof(buf), "%.4g", fps);
	return std::string(buf);
}

} // namespace nos::mediaio
