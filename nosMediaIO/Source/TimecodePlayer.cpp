// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

#include "ANC_generated.h"
#include "Timing.hpp"

namespace nos::mediaio
{

namespace
{
// HH:MM:SS:FF to a frame number, mirroring TimecodeToFrameNumber.
uint32_t TCToFrameNumber(uint8_t hours, uint8_t minutes, uint8_t seconds, uint8_t frames,
	float fps, bool dropFrame)
{
	const int fpsRound = std::max(1, int(std::lround(fps)));
	const uint32_t totalSec = (uint32_t(hours) * 60u + minutes) * 60u + seconds;
	if (!dropFrame)
		return totalSec * uint32_t(fpsRound) + frames;

	const int dropPerMin = int(std::lround(fps * 0.066666f));
	const int framesPerMin = fpsRound * 60 - dropPerMin;
	const int framesPer10Min = framesPerMin * 10 + dropPerMin;
	const int totalMin = int(hours) * 60 + minutes;
	return uint32_t(framesPer10Min) * uint32_t(totalMin / 10)
		+ uint32_t(framesPerMin) * uint32_t(totalMin % 10)
		+ uint32_t(seconds) * uint32_t(fpsRound)
		+ frames;
}

// Frame number to HH:MM:SS:FF, mirroring TimecodeFromFrameNumber.
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

// Matches nos.mediaio.TimecodePlayerMode in TimecodePlayer.fbs.
enum class TimecodePlayerMode : uint32_t
{
	Ranged = 0,
	Continuous = 1,
};

// Plays a running timecode, advancing one frame per run while playing. Start/Stop
// play/pause, Reset returns to Start Time; Ranged mode runs from Start to End then stops.
struct TimecodePlayerNode : NodeContext
{
	uint32_t FrameCounter = 0;
	bool Playing = false;
	TimecodePlayerMode Mode = TimecodePlayerMode::Ranged;
	// Function (Start/Stop/Reset) node ids, keyed by class name, for orphaning controls.
	std::unordered_map<nos::Name, uuid> FunctionIds;

	TimecodePlayerNode(nosFbNodePtr node) : NodeContext(node)
	{
		if (node->pins())
			for (auto* pin : *node->pins())
				if (pin->data() && pin->data()->size() &&
					nos::Name(pin->name()->c_str()) == NOS_NAME_STATIC("Mode"))
				{
					const void* data = pin->data()->data();
					Mode = static_cast<TimecodePlayerMode>(*static_cast<const uint32_t*>(data));
				}
		if (node->functions())
			for (auto* fn : *node->functions())
				if (fn->class_name() && fn->id())
					FunctionIds[nos::Name(fn->class_name()->c_str())] = *fn->id();
		SyncEndTimecodeState();
		SyncTransportControls();
	}

	void OnPinValueChanged(nos::Name pinName, uuid const&, nosBuffer value) override
	{
		if (pinName == NOS_NAME_STATIC("Mode"))
		{
			Mode = static_cast<TimecodePlayerMode>(*static_cast<const uint32_t*>(value.Data));
			SyncEndTimecodeState();
		}
	}

	// End Timecode only applies in Ranged mode.
	void SyncEndTimecodeState()
	{
		SetPinOrphanState(NOS_NAME_STATIC("EndTimecode"),
			Mode == TimecodePlayerMode::Continuous ? fb::PinOrphanStateType::PASSIVE
												   : fb::PinOrphanStateType::ACTIVE);
	}

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		nos::NodeExecuteParams execParams(params);
		const Timecode* start = execParams.GetPinData<Timecode>(NOS_NAME_STATIC("StartTime"));
		if (!start)
			return NOS_RESULT_FAILED;

		// The player advances one frame per run, so its rate is the rate it is scheduled
		// at: take it from the path's fixed-step timing (60 fallback if not fixed-step).
		const float frameRate = FrameRateFromTiming(params, 60.0f);

		// Drop-frame is only defined for the NTSC fractional rates (29.97 / 59.94).
		const bool isNtscFamily =
			std::abs(frameRate - 29.97f) < 0.05f ||
			std::abs(frameRate - 59.94f) < 0.05f;
		const bool effectiveDropFrame = start->drop_frame() && isNtscFamily;

		const uint32_t startFrame = TCToFrameNumber(
			start->hours(), start->minutes(), start->seconds(), start->frames(),
			frameRate, effectiveDropFrame);

		uint32_t frame = startFrame + FrameCounter;
		// In Ranged mode the player runs from Start Time to End Timecode and then stops,
		// holding the end frame. Continuous mode counts up without bound.
		bool reachedEnd = false;
		if (Mode == TimecodePlayerMode::Ranged)
			if (const Timecode* end = execParams.GetPinData<Timecode>(NOS_NAME_STATIC("EndTimecode")))
			{
				const uint32_t endFrame = TCToFrameNumber(
					end->hours(), end->minutes(), end->seconds(), end->frames(),
					frameRate, effectiveDropFrame);
				if (endFrame > startFrame && frame >= endFrame)
				{
					frame = endFrame;
					reachedEnd = true;
				}
			}

		uint8_t h = 0, m = 0, s = 0, f = 0;
		FrameNumberToTC(frame, frameRate, effectiveDropFrame, h, m, s, f);

		Timecode tc(h, m, s, f, effectiveDropFrame,
			start->source() == ATCSource::Auto ? ATCSource::ATC_LTC : start->source());
		SetPinValue(NOS_NAME_STATIC("Timecode"), nos::Buffer::From(tc));

		if (reachedEnd)
		{
			if (Playing)
				SetPlaying(false);
		}
		else if (Playing)
			++FrameCounter;
		return NOS_RESULT_SUCCESS;
	}

	void SetPlaying(bool playing)
	{
		Playing = playing;
		SetPinValue(NOS_NAME_STATIC("Playing"), nos::Buffer::From(playing));
		SyncTransportControls();
	}

	// Orphan the control that does not apply to the current state: Start while playing,
	// Stop while stopped. Node orphan state has no PASSIVE, so ORPHAN is used.
	void SyncTransportControls()
	{
		SetFunctionOrphanState(NOS_NAME_STATIC("Start"),
			Playing ? fb::NodeOrphanStateType::ORPHAN : fb::NodeOrphanStateType::ACTIVE);
		SetFunctionOrphanState(NOS_NAME_STATIC("Stop"),
			Playing ? fb::NodeOrphanStateType::ACTIVE : fb::NodeOrphanStateType::ORPHAN);
	}

	void SetFunctionOrphanState(nos::Name className, fb::NodeOrphanStateType type)
	{
		auto it = FunctionIds.find(className);
		if (it != FunctionIds.end())
			SetNodeOrphanState(it->second, type);
	}

	nosResult Reset(nosFunctionExecuteParams*)
	{
		FrameCounter = 0;
		return NOS_RESULT_SUCCESS;
	}

	nosResult Start(nosFunctionExecuteParams*)
	{
		// Ranged mode plays once from Start to End, so each Start restarts from the beginning.
		if (Mode == TimecodePlayerMode::Ranged)
			FrameCounter = 0;
		SetPlaying(true);
		return NOS_RESULT_SUCCESS;
	}

	nosResult Stop(nosFunctionExecuteParams*)
	{
		SetPlaying(false);
		return NOS_RESULT_SUCCESS;
	}

	NOS_DECLARE_FUNCTIONS(
		NOS_ADD_FUNCTION(NOS_NAME_STATIC("Reset"), Reset),
		NOS_ADD_FUNCTION(NOS_NAME_STATIC("Start"), Start),
		NOS_ADD_FUNCTION(NOS_NAME_STATIC("Stop"), Stop),
	)
};

nosResult RegisterTimecodePlayer(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("TimecodePlayer"), TimecodePlayerNode, fn)
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::mediaio
