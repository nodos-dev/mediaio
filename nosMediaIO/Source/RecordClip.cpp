// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>
#include <nosVulkanSubsystem/Helpers.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "ANC_generated.h"
#include "Dpx.h"

namespace nos::mediaio
{

// Records the input texture to disk as an uncompressed DPX sequence, one file per
// timecode, while the Record pin is true. The GPU is flushed (submit and wait) before
// the readback so the captured frame is complete - see RepeatingJunction / WriteImage /
// RingBuffer for the same pattern. Writing a frame is a fixed header plus the readback
// buffer verbatim - no compression, no per-pixel work. The readback and disk write run
// synchronously on the execution path; drive the cadence from the node graph.
// The input Texture is also passed straight through to the Output pin. Progress and
// errors are surfaced on the node status.
struct RecordClipNode : NodeContext
{
	std::string StatusText;
	int StatusType = -1;
	bool WasRecording = false;
	uint64_t RecordedFrames = 0;

	RecordClipNode(nosFbNodePtr node) : NodeContext(node) {}

	// Sets the node status, skipping the update when nothing changed (avoids per-frame churn).
	void ShowStatus(const std::string& text, fb::NodeStatusMessageType type)
	{
		if (int(type) == StatusType && text == StatusText)
			return;
		StatusText = text;
		StatusType = int(type);
		SetNodeStatusMessage(text, type);
	}

	// Flushes pending GPU work so the texture we read back is a finished, tear-free frame.
	void SubmitAndWait()
	{
		nosGPUEvent event{};
		nosCmd cmd = vkss::BeginCmd(NOS_NAME("RecordClip Flush"), NodeId);
		nosCmdEndParams endParams{ .ForceSubmit = true, .OutGPUEventHandle = &event };
		nosVulkan->End(cmd, &endParams);
		nosVulkan->WaitGpuEvent(&event, UINT64_MAX);
	}

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		nos::NodeExecuteParams execParams(params);

		// Pass the input texture straight through, whether or not we are recording.
		auto& texPin = execParams[NOS_NAME_STATIC("Texture")];
		if (texPin.Data)
			SetPinValue(NOS_NAME_STATIC("Output"), *texPin.Data);

		const bool* record = execParams.GetPinData<bool>(NOS_NAME_STATIC("Record"));
		bool recording = record && *record;
		if (recording && !WasRecording)
			RecordedFrames = 0; // rising edge: restart the frame count
		WasRecording = recording;
		if (!recording)
		{
			ShowStatus("Idle", fb::NodeStatusMessageType::INFO);
			return NOS_RESULT_SUCCESS;
		}

		const Timecode* tc = execParams.GetPinData<Timecode>(NOS_NAME_STATIC("Timecode"));
		if (!tc)
		{
			ShowStatus("Missing Timecode input", fb::NodeStatusMessageType::FAILURE);
			return NOS_RESULT_FAILED;
		}

		const char* pathC = "";
		auto& pathPin = execParams[NOS_NAME_STATIC("Path")];
		if (pathPin.Data && pathPin.Data->Data && pathPin.Data->Size)
			pathC = static_cast<const char*>(pathPin.Data->Data);
		if (!*pathC)
		{
			ShowStatus("Set output directory", fb::NodeStatusMessageType::WARNING);
			return NOS_RESULT_SUCCESS;
		}

		if (!texPin.Data)
			return NOS_RESULT_SUCCESS;
		auto inputTex = vkss::DeserializeTextureInfo(texPin.Data->Data);
		if (!inputTex.Memory.Handle)
		{
			ShowStatus("Waiting for input texture", fb::NodeStatusMessageType::WARNING);
			return NOS_RESULT_SUCCESS;
		}

		// DPX stores RGB/RGBA in native channel order, so these formats need no swizzle.
		dpx::ImageDesc desc{};
		desc.Width = inputTex.Info.Texture.Width;
		desc.Height = inputTex.Info.Texture.Height;
		switch (inputTex.Info.Texture.Format)
		{
		case NOS_FORMAT_R8G8B8A8_UNORM:
		case NOS_FORMAT_R8G8B8A8_SRGB:
			desc.Channels = 4; desc.BitDepth = 8; break;
		case NOS_FORMAT_R16G16B16A16_UNORM:
			desc.Channels = 4; desc.BitDepth = 16; break;
		default:
			ShowStatus("Unsupported texture format - need R8G8B8A8 or R16G16B16A16_UNORM",
				fb::NodeStatusMessageType::FAILURE);
			return NOS_RESULT_FAILED;
		}
		uint64_t dataSize = dpx::ImageDataSize(desc);

		// Flush the GPU so the input texture holds a complete frame before we read it.
		SubmitAndWait();

		// Copy the input texture into a host-visible buffer and read it back.
		nosBufferInfo bufInfo = {};
		bufInfo.Size = uint32_t(dataSize);
		bufInfo.Usage = nosBufferUsage(NOS_BUFFER_USAGE_TRANSFER_SRC | NOS_BUFFER_USAGE_TRANSFER_DST);
		bufInfo.MemoryFlags = nosMemoryFlags(NOS_MEMORY_FLAGS_HOST_VISIBLE | NOS_MEMORY_FLAGS_DOWNLOAD);

		auto readback = vkss::Resource::Create(bufInfo, "RecordClip Readback");
		if (!readback)
		{
			nosEngine.LogE("RecordClip: failed to allocate readback buffer");
			ShowStatus("Failed to allocate readback buffer", fb::NodeStatusMessageType::FAILURE);
			return NOS_RESULT_FAILED;
		}

		nosGPUEvent event{};
		nosCmd cmd = vkss::BeginCmd(NOS_NAME("RecordClip Readback Copy"), NodeId);
		nosVulkan->Copy(cmd, &inputTex, &*readback, nullptr);
		nosCmdEndParams endParams{ .ForceSubmit = true, .OutGPUEventHandle = &event };
		nosVulkan->End(cmd, &endParams);
		nosVulkan->WaitGpuEvent(&event, UINT64_MAX);

		uint8_t* pixels = nosVulkan->Map(&*readback);
		if (!pixels)
		{
			nosEngine.LogE("RecordClip: failed to map readback buffer");
			ShowStatus("Failed to map readback buffer", fb::NodeStatusMessageType::FAILURE);
			return NOS_RESULT_FAILED;
		}

		std::filesystem::path dir = nos::Utf8ToPath(std::string(pathC));
		std::string fileName = dpx::TimecodeToFileName(*tc);
		std::filesystem::path filePath = dir / fileName;
		try
		{
			std::filesystem::create_directories(dir);
		}
		catch (const std::filesystem::filesystem_error& e)
		{
			nosEngine.LogE("RecordClip: %s: %s", nos::PathToUtf8(dir).c_str(), e.what());
			ShowStatus("Cannot create output directory", fb::NodeStatusMessageType::FAILURE);
			return NOS_RESULT_FAILED;
		}

		uint8_t header[dpx::HEADER_SIZE];
		dpx::WriteHeader(header, desc, *tc);

		std::ofstream file(filePath, std::ios::binary | std::ios::trunc);
		if (!file
			|| !file.write(reinterpret_cast<const char*>(header), dpx::HEADER_SIZE)
			|| !file.write(reinterpret_cast<const char*>(pixels), std::streamsize(dataSize)))
		{
			nosEngine.LogE("RecordClip: failed to write %s", nos::PathToUtf8(filePath).c_str());
			ShowStatus("Failed to write " + fileName, fb::NodeStatusMessageType::FAILURE);
			return NOS_RESULT_FAILED;
		}

		++RecordedFrames;
		ShowStatus("Recording (" + std::to_string(RecordedFrames) + " frames)",
			fb::NodeStatusMessageType::INFO);
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterRecordClip(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("RecordClip"), RecordClipNode, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::mediaio
