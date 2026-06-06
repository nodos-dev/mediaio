// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>
#include <nosVulkanSubsystem/Helpers.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <nosUtil/Stopwatch.hpp>

#include "ANC_generated.h"
#include "Conversion_generated.h"
#include "Dpx.h"
#include "Timing.hpp"

namespace nos::mediaio
{

// Records the input texture to disk as an uncompressed DPX sequence, one file per
// timecode, while the Record pin is true. The GammaCurve pin chooses the output transfer
// curve: IDENTITY writes the graph's linear pixels verbatim; SRGB encodes them to sRGB
// (a free GPU format-blit) so the files look correct in ordinary viewers. The GPU is
// flushed (submit and wait) before the readback so the captured frame is complete - see
// RepeatingJunction / WriteImage / RingBuffer for the same pattern. Readback and the disk
// write run synchronously on the execution path; drive the cadence from the node graph.
// The input Texture is passed straight through to the Output pin. Progress and errors are
// surfaced on the node status.
struct RecordClipNode : NodeContext
{
	std::string StatusText;
	int StatusType = -1;
	bool WasRecording = false;
	uint64_t RecordedFrames = 0;

	// Reused across frames; reallocated only when the frame size/resolution changes.
	std::optional<vkss::Resource> Readback;
	uint64_t ReadbackSize = 0;
	std::optional<vkss::Resource> SrgbTex;
	uint32_t SrgbWidth = 0, SrgbHeight = 0;

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

	// Records one Copy into the host-visible readback buffer, waiting for it to finish.
	void CopyToReadback(const nosResourceShareInfo& src, const nosResourceShareInfo& dst)
	{
		nosGPUEvent event{};
		nosCmd cmd = vkss::BeginCmd(NOS_NAME("RecordClip Copy"), NodeId);
		nosVulkan->Copy(cmd, &src, &dst, nullptr);
		nosCmdEndParams endParams{ .ForceSubmit = true, .OutGPUEventHandle = &event };
		nosVulkan->End(cmd, &endParams);
		nosVulkan->WaitGpuEvent(&event, UINT64_MAX);
	}

	// Reused readback buffer; reallocated only when the required size changes.
	vkss::Resource* EnsureReadback(uint64_t size)
	{
		if (!Readback || ReadbackSize != size)
		{
			nosBufferInfo bufInfo = {};
			bufInfo.Size = uint32_t(size);
			bufInfo.Usage = nosBufferUsage(NOS_BUFFER_USAGE_TRANSFER_SRC | NOS_BUFFER_USAGE_TRANSFER_DST);
			bufInfo.MemoryFlags = nosMemoryFlags(NOS_MEMORY_FLAGS_HOST_VISIBLE | NOS_MEMORY_FLAGS_DOWNLOAD);
			Readback = vkss::Resource::Create(bufInfo, "RecordClip Readback");
			ReadbackSize = Readback ? size : 0;
		}
		return Readback ? &*Readback : nullptr;
	}

	// Reused sRGB encode texture; reallocated only when the resolution changes.
	vkss::Resource* EnsureSrgbTex(uint32_t width, uint32_t height)
	{
		if (!SrgbTex || SrgbWidth != width || SrgbHeight != height)
		{
			nosResourceShareInfo info = {};
			info.Info.Type = NOS_RESOURCE_TYPE_TEXTURE;
			info.Info.Texture.Width = width;
			info.Info.Texture.Height = height;
			info.Info.Texture.Format = NOS_FORMAT_R8G8B8A8_SRGB;
			info.Info.Texture.Usage = nosImageUsage(NOS_IMAGE_USAGE_TRANSFER_SRC | NOS_IMAGE_USAGE_TRANSFER_DST);
			SrgbTex = vkss::Resource::Create(info, "RecordClip sRGB Encode");
			SrgbWidth = SrgbTex ? width : 0;
			SrgbHeight = SrgbTex ? height : 0;
		}
		return SrgbTex ? &*SrgbTex : nullptr;
	}

	// Frees the reused GPU resources when idle.
	void ReleaseResources()
	{
		Readback.reset();
		SrgbTex.reset();
		ReadbackSize = 0;
		SrgbWidth = SrgbHeight = 0;
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

		// Pass the input texture straight through, whether or not we are recording.
		auto& texPin = execParams[NOS_NAME_STATIC("Texture")];
		if (texPin.Data)
			SetPinValue(NOS_NAME_STATIC("Output"), *texPin.Data);

		const bool* record = execParams.GetPinData<bool>(NOS_NAME_STATIC("Record"));
		bool recording = record && *record;
		if (recording && !WasRecording)
			RecordedFrames = 0; // rising edge: restart the frame count
		if (!recording && WasRecording)
			ReleaseResources(); // falling edge: free the reused buffers while idle
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

		nosFormat inFormat = inputTex.Info.Texture.Format;
		bool input8 = inFormat == NOS_FORMAT_R8G8B8A8_UNORM || inFormat == NOS_FORMAT_R8G8B8A8_SRGB;
		bool input16 = inFormat == NOS_FORMAT_R16G16B16A16_UNORM;
		if (!input8 && !input16)
		{
			ShowStatus("Unsupported texture format - need R8G8B8A8 or R16G16B16A16_UNORM",
				fb::NodeStatusMessageType::FAILURE);
			return NOS_RESULT_FAILED;
		}

		// GammaCurve picks the transfer curve the file is encoded with.
		GammaCurve curve = GammaCurve::SRGB;
		if (auto* c = execParams.GetPinData<GammaCurve>(NOS_NAME_STATIC("GammaCurve")))
			curve = *c;
		bool encodeSrgb;
		if (curve == GammaCurve::SRGB)
			encodeSrgb = true;
		else if (curve == GammaCurve::IDENTITY)
			encodeSrgb = false;
		else
		{
			ShowStatus("Gamma curve not supported - use IDENTITY or SRGB (convert others "
					   "with a gamma node upstream)", fb::NodeStatusMessageType::FAILURE);
			return NOS_RESULT_FAILED;
		}
		if (encodeSrgb && !input8)
		{
			ShowStatus("SRGB output needs an 8-bit input texture - record 16-bit as IDENTITY",
				fb::NodeStatusMessageType::FAILURE);
			return NOS_RESULT_FAILED;
		}

		dpx::ImageDesc desc{};
		desc.Width = inputTex.Info.Texture.Width;
		desc.Height = inputTex.Info.Texture.Height;
		desc.Channels = 4;
		desc.BitDepth = (encodeSrgb || input8) ? 8 : 16;
		desc.Transfer = encodeSrgb ? dpx::TRANSFER_USER_DEFINED : dpx::TRANSFER_LINEAR;
		uint64_t dataSize = dpx::ImageDataSize(desc);
		lap("Setup");

		// Flush the GPU so the input texture holds a complete frame before we read it.
		SubmitAndWait();
		lap("SubmitAndWait");

		vkss::Resource* readback = EnsureReadback(dataSize);
		if (!readback)
		{
			nosEngine.LogE("RecordClip: failed to allocate readback buffer");
			ShowStatus("Failed to allocate readback buffer", fb::NodeStatusMessageType::FAILURE);
			return NOS_RESULT_FAILED;
		}
		lap("EnsureReadback");

		if (encodeSrgb)
		{
			// Blit the linear input into an sRGB-format texture; the hardware store
			// applies the linear->sRGB encode for free, then read that back.
			vkss::Resource* srgbTex = EnsureSrgbTex(desc.Width, desc.Height);
			if (!srgbTex)
			{
				nosEngine.LogE("RecordClip: failed to allocate sRGB encode texture");
				ShowStatus("Failed to allocate sRGB encode texture",
					fb::NodeStatusMessageType::FAILURE);
				return NOS_RESULT_FAILED;
			}
			CopyToReadback(inputTex, *srgbTex);  // linear -> sRGB (blit encodes)
			CopyToReadback(*srgbTex, *readback); // sRGB texture -> host buffer (raw)
		}
		else
		{
			CopyToReadback(inputTex, *readback);
		}
		lap("CopyToReadback");

		uint8_t* pixels = nosVulkan->Map(readback);
		if (!pixels)
		{
			nosEngine.LogE("RecordClip: failed to map readback buffer");
			ShowStatus("Failed to map readback buffer", fb::NodeStatusMessageType::FAILURE);
			return NOS_RESULT_FAILED;
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
			nosEngine.LogE("RecordClip: %s: %s", nos::PathToUtf8(dir).c_str(), e.what());
			ShowStatus("Cannot create output directory", fb::NodeStatusMessageType::FAILURE);
			return NOS_RESULT_FAILED;
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
			nosEngine.LogE("RecordClip: failed to write %s", nos::PathToUtf8(filePath).c_str());
			ShowStatus("Failed to write " + fileName, fb::NodeStatusMessageType::FAILURE);
			return NOS_RESULT_FAILED;
		}
		lap("DiskWrite");
		nosEngine.WatchLog((NodeName.AsString() + " | Total").c_str(), total.ElapsedString().c_str());

		++RecordedFrames;
		// "Recording 1920x1080 @ 59.94 FPS - 123 frames" (FPS omitted on variable-step paths).
		std::string status = "Recording " + std::to_string(desc.Width) + "x" + std::to_string(desc.Height);
		if (frameRate > 0.0f)
			status += " @ " + FrameRateToString(frameRate) + " FPS";
		status += " - " + std::to_string(RecordedFrames) + " frames";
		ShowStatus(status, fb::NodeStatusMessageType::INFO);
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterRecordClip(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("RecordClip"), RecordClipNode, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::mediaio
