// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>
#include <nosVulkanSubsystem/Helpers.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <nosUtil/Stopwatch.hpp>

#include "ANC_generated.h"
#include "Conversion_generated.h"
#include "Dpx.h"
#include "Timing.hpp"

namespace nos::mediaio
{

// Maps a packed RGBPixelFormat to the DPX image element's channel count and bit depth.
// The pixel bytes themselves are produced upstream by the Texture To Buffer node.
static void DpxLayoutFromFormat(RGBPixelFormat fmt, dpx::ImageDesc& d)
{
	switch (fmt)
	{
	case RGBPixelFormat::RGBA16: d.Channels = 4; d.BitDepth = 16; break;
	case RGBPixelFormat::RGB10:  d.Channels = 3; d.BitDepth = 10; break;
	case RGBPixelFormat::RGB8:   d.Channels = 3; d.BitDepth = 8;  break;
	case RGBPixelFormat::RGBA8:
	default:                        d.Channels = 4; d.BitDepth = 8;  break;
	}
}

// DPX transfer characteristic code for the curve the upstream node encoded with. Curves DPX
// has no dedicated code for (sRGB, HLG, ST2084, S-Log3) are tagged USER_DEFINED.
static uint8_t DpxTransferFromCurve(GammaCurve curve)
{
	switch (curve)
	{
	case GammaCurve::IDENTITY: return dpx::CHARACTERISTIC_LINEAR;
	case GammaCurve::REC709:   return dpx::CHARACTERISTIC_ITUR709;
	default:                   return dpx::CHARACTERISTIC_USER_DEFINED;
	}
}

// DPX colorimetric specification (colour primaries) implied by the encode curve. sRGB and
// Rec.709 share Rec.709 primaries; the linear, HDR and log curves carry no primaries by
// themselves (and DPX has no Rec.2020 / S-Gamut code), so they are left undefined rather than
// mislabelled. Note "Linear" is a transfer code only - it is not a valid colorimetric value.
static uint8_t DpxColorimetricFromCurve(GammaCurve curve)
{
	switch (curve)
	{
	case GammaCurve::SRGB:
	case GammaCurve::REC709: return dpx::CHARACTERISTIC_ITUR709;
	default:                 return dpx::CHARACTERISTIC_UNDEFINED;
	}
}

// Writes the input Buffer to disk as an uncompressed DPX sequence, one file per timecode,
// while the Write pin is true. The pixels are produced upstream (Texture To Buffer +
// download ring), so this node does no GPU work and no conversion: it maps the host-visible
// buffer and writes the bytes after the DPX header. Resolution, Output Format and Transfer
// pins describe the buffer for the header - they must match the upstream Texture To Buffer
// node. Execution is driven through the In/Out exe pins. Progress and errors are surfaced on
// the node status.
struct WriteDPXNode : NodeContext
{
	// Status is split across a few short lines instead of one long string. Each slot is
	// updated independently; the set is pushed to the engine only when it actually changes
	// (see FlushStatus), so a steadily-writing node does not touch the engine every frame.
	enum class StatusSlot : int { State = 0, Format = 1, Path = 2, Frames = 3 };
	std::map<StatusSlot, fb::TNodeStatusMessage> StatusMessages;
	bool StatusDirty = false;

	bool WasWriting = false;
	uint64_t WrittenFrames = 0;

	// Refresh the live frame count every this many frames rather than every frame.
	static constexpr uint64_t FrameStatusInterval = 30;

	WriteDPXNode(nosFbNodePtr node) : NodeContext(node) {}

	void SetStatus(StatusSlot slot, fb::NodeStatusMessageType type, std::string text)
	{
		auto it = StatusMessages.find(slot);
		if (it != StatusMessages.end() && it->second.type == type && it->second.text == text)
			return; // unchanged - leave the dirty flag alone
		StatusMessages[slot] = fb::TNodeStatusMessage{ {}, std::move(text), type };
		StatusDirty = true;
	}

	void ClearStatus(StatusSlot slot)
	{
		if (StatusMessages.erase(slot))
			StatusDirty = true;
	}

	// Pushes the message set to the engine only when it changed (map order = display order).
	void FlushStatus()
	{
		if (!StatusDirty)
			return;
		std::vector<fb::TNodeStatusMessage> messages;
		messages.reserve(StatusMessages.size());
		for (auto& [slot, msg] : StatusMessages)
			messages.push_back(msg);
		SetNodeStatusMessages(messages);
		StatusDirty = false;
	}

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		nos::NodeExecuteParams execParams(params);

		// Per-section timing surfaced on the watch panel as "<node> <section>".
		nos::util::Stopwatch total, section;
		auto lap = [&](const char* name) {
			nosEngine.WatchLog((NodeName.AsString() + " " + name).c_str(),
				section.ElapsedStringAndReset().c_str());
		};

		auto& bufPin = execParams[NOS_NAME_STATIC("Buffer")];

		// Destination directory - shown on the status whether or not we are writing.
		const char* pathC = "";
		auto& pathPin = execParams[NOS_NAME_STATIC("Path")];
		if (pathPin.Data && pathPin.Data->Data && pathPin.Data->Size)
			pathC = static_cast<const char*>(pathPin.Data->Data);
		if (!*pathC)
		{
			ClearStatus(StatusSlot::State);
			ClearStatus(StatusSlot::Format);
			ClearStatus(StatusSlot::Frames);
			SetStatus(StatusSlot::Path, fb::NodeStatusMessageType::WARNING, "Destination path not set");
			FlushStatus();
			return NOS_RESULT_SUCCESS;
		}
		SetStatus(StatusSlot::Path, fb::NodeStatusMessageType::INFO, pathC);

		const bool* write = execParams.GetPinData<bool>(NOS_NAME_STATIC("Write"));
		bool writing = write && *write;
		if (writing && !WasWriting)
			WrittenFrames = 0; // rising edge: restart the frame count
		WasWriting = writing;
		if (!writing)
		{
			ClearStatus(StatusSlot::Format);
			ClearStatus(StatusSlot::Frames);
			SetStatus(StatusSlot::State, fb::NodeStatusMessageType::INFO, "Idle");
			FlushStatus();
			return NOS_RESULT_SUCCESS;
		}

		// Surfaces a failure on the State line and stops.
		auto fail = [&](std::string text) {
			ClearStatus(StatusSlot::Format);
			ClearStatus(StatusSlot::Frames);
			SetStatus(StatusSlot::State, fb::NodeStatusMessageType::FAILURE, std::move(text));
			FlushStatus();
			return NOS_RESULT_FAILED;
		};

		const Timecode* tc = execParams.GetPinData<Timecode>(NOS_NAME_STATIC("Timecode"));
		if (!tc)
			return fail("Missing Timecode input");

		const nosVec2u* resolution = execParams.GetPinData<nosVec2u>(NOS_NAME_STATIC("Resolution"));
		if (!resolution || !resolution->x || !resolution->y)
			return fail("Set Resolution to the buffer's frame size");

		auto fmt = RGBPixelFormat::RGBA8;
		if (auto* f = execParams.GetPinData<RGBPixelFormat>(NOS_NAME_STATIC("OutputFormat")))
			fmt = *f;
		GammaCurve curve = GammaCurve::SRGB;
		if (auto* c = execParams.GetPinData<GammaCurve>(NOS_NAME_STATIC("Transfer")))
			curve = *c;

		dpx::ImageDesc desc{};
		desc.Width = resolution->x;
		desc.Height = resolution->y;
		DpxLayoutFromFormat(fmt, desc);
		desc.Transfer = DpxTransferFromCurve(curve);
		desc.Colorimetric = DpxColorimetricFromCurve(curve);
		uint64_t dataSize = dpx::ImageDataSize(desc);
		lap("Setup");

		// The pixels are already on the host (download ring upstream); just map and read them.
		if (!bufPin.Data)
		{
			FlushStatus();
			return NOS_RESULT_SUCCESS;
		}
		auto buffer = vkss::ConvertToResourceInfo(*InterpretPinValue<sys::vulkan::Buffer>(bufPin.Data->Data));
		if (!buffer.Memory.Handle)
		{
			ClearStatus(StatusSlot::Format);
			ClearStatus(StatusSlot::Frames);
			SetStatus(StatusSlot::State, fb::NodeStatusMessageType::WARNING, "Waiting for input buffer");
			FlushStatus();
			return NOS_RESULT_SUCCESS;
		}
		if (buffer.Memory.Size < dataSize)
		{
			nosEngine.LogE("WriteDPX: buffer (%llu bytes) smaller than the frame needs (%llu bytes) "
				"- check Resolution and Output Format", (unsigned long long)buffer.Memory.Size,
				(unsigned long long)dataSize);
			return fail("Buffer smaller than frame - check Resolution / Output Format");
		}

		uint8_t* pixels = nosVulkan->Map(&buffer);
		if (!pixels)
		{
			nosEngine.LogE("WriteDPX: failed to map input buffer");
			return fail("Failed to map input buffer");
		}
		lap("Map");

		std::filesystem::path dir = nos::Utf8ToPath(std::string(pathC));
		std::string fileName = dpx::TimecodeToFileName(*tc);
		std::filesystem::path filePath = dir / fileName;
		try
		{
			std::filesystem::create_directories(dir);
		}
		catch (const std::filesystem::filesystem_error& e)
		{
			nosEngine.LogE("WriteDPX: %s: %s", nos::PathToUtf8(dir).c_str(), e.what());
			return fail("Cannot create output directory");
		}

		// Frame rate for the DPX header: infer from the path's fixed-step timing; variable-step
		// paths have no nominal rate, so leave it unset (0).
		float frameRate = FrameRateFromTiming(params);

		uint8_t header[dpx::HEADER_SIZE];
		dpx::WriteHeader(header, desc, *tc, frameRate);

		std::ofstream file(filePath, std::ios::binary | std::ios::trunc);
		if (!file
			|| !file.write(reinterpret_cast<const char*>(header), dpx::HEADER_SIZE)
			|| !file.write(reinterpret_cast<const char*>(pixels), std::streamsize(dataSize)))
		{
			nosEngine.LogE("WriteDPX: failed to write %s", nos::PathToUtf8(filePath).c_str());
			return fail("Failed to write " + fileName);
		}
		lap("DiskWrite");
		nosEngine.WatchLog((NodeName.AsString() + " | Total").c_str(), total.ElapsedString().c_str());

		++WrittenFrames;
		SetStatus(StatusSlot::State, fb::NodeStatusMessageType::INFO, "Writing");
		// "1920x1080 @59.94 FPS" (FPS dropped on variable-step paths) - stable while writing.
		std::string format = std::to_string(desc.Width) + "x" + std::to_string(desc.Height);
		if (frameRate > 0.0f)
			format += " @" + FrameRateToString(frameRate) + " FPS";
		SetStatus(StatusSlot::Format, fb::NodeStatusMessageType::INFO, std::move(format));
		// Only refresh the count periodically so we are not flushing status every frame.
		if (WrittenFrames == 1 || WrittenFrames % FrameStatusInterval == 0)
			SetStatus(StatusSlot::Frames, fb::NodeStatusMessageType::INFO,
				std::to_string(WrittenFrames) + " frames");
		FlushStatus();
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterWriteDPX(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("WriteDPX"), WriteDPXNode, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::mediaio
