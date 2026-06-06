// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>
#include <nosVulkanSubsystem/Helpers.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "ANC_generated.h"
#include "Dpx.h"
#include "Timing.hpp"

namespace nos::mediaio
{

// Plays back a clip recorded by RecordClip: for the incoming timecode it loads the
// matching 'HH-MM-SS-FF.dpx' frame from Path and emits it on the Texture output. The
// output texture format follows the DPX transfer tag - an sRGB-encoded file becomes an
// _SRGB texture so the graph's sampler decodes it back to linear automatically. When no
// frame matches the timecode, the last loaded frame is held. Progress and errors are
// surfaced on the node status.
struct PlaybackClipNode : NodeContext
{
	std::optional<vkss::Resource> OutputTexture;
	std::string LoadedFrame;          // file name currently held on the output pin
	nosVec2u LoadedResolution = {};   // held frame's resolution
	float LoadedFrameRate = 0.0f;     // frame rate recorded in the held frame's header (0 = unset)
	std::string StatusText;
	int StatusType = -1;

	PlaybackClipNode(nosFbNodePtr node) : NodeContext(node) {}

	// Sets the node status, skipping the update when nothing changed.
	void ShowStatus(const std::string& text, fb::NodeStatusMessageType type)
	{
		if (int(type) == StatusType && text == StatusText)
			return;
		StatusText = text;
		StatusType = int(type);
		SetNodeStatusMessage(text, type);
	}

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		nos::NodeExecuteParams execParams(params);

		const Timecode* tc = execParams.GetPinData<Timecode>(NOS_NAME_STATIC("Timecode"));
		if (!tc)
		{
			ShowStatus("Missing Timecode input", fb::NodeStatusMessageType::WARNING);
			return NOS_RESULT_SUCCESS; // hold last frame
		}

		const char* pathC = "";
		auto& pathPin = execParams[NOS_NAME_STATIC("Path")];
		if (pathPin.Data && pathPin.Data->Data && pathPin.Data->Size)
			pathC = static_cast<const char*>(pathPin.Data->Data);
		if (!*pathC)
		{
			ShowStatus("Set input directory", fb::NodeStatusMessageType::WARNING);
			return NOS_RESULT_SUCCESS; // hold last frame
		}

		std::string fileName = dpx::TimecodeToFileName(*tc);
		if (fileName == LoadedFrame && OutputTexture)
		{
			// Already holding this frame, but re-check timing so a cadence change still warns.
			ReportPlaybackStatus(params, fileName);
			return NOS_RESULT_SUCCESS;
		}

		std::filesystem::path filePath = nos::Utf8ToPath(std::string(pathC)) / fileName;
		std::error_code ec;
		if (!std::filesystem::exists(filePath, ec))
		{
			ShowStatus(fileName + " not found - holding last frame",
				fb::NodeStatusMessageType::WARNING);
			return NOS_RESULT_SUCCESS; // hold last frame
		}

		if (LoadFrame(filePath))
		{
			LoadedFrame = fileName;
			ReportPlaybackStatus(params, fileName);
		}
		return NOS_RESULT_SUCCESS; // load failure also holds the last frame
	}

	// Shows "Playing <file> - 1920x1080 @ 59.94 FPS", warning when the executing path can't
	// reproduce the clip's recorded frame rate. DPX stores the rate as a float, so the compare uses
	// a small tolerance - enough to tell 59.94 from 60. A variable-step path has no fixed rate and
	// always warns; a fixed-step path warns only when its rate differs. A clip with no recorded rate
	// (0) and a 0/0 path delta are both left unchecked.
	void ReportPlaybackStatus(nosNodeExecuteParams* params, const std::string& fileName)
	{
		// "Playing <file> - 1920x1080 @ 59.94 FPS" (FPS omitted when the clip records no rate).
		std::string info = "Playing " + fileName + " - "
			+ std::to_string(LoadedResolution.x) + "x" + std::to_string(LoadedResolution.y);
		if (LoadedFrameRate > 0.0f)
			info += " @ " + FrameRateToString(LoadedFrameRate) + " FPS";

		if (LoadedFrameRate > 0.0f)
		{
			// A variable-step path can't reproduce a fixed recorded rate; always warn.
			if (params->TimingInfo.TimingMode != NOS_EXECUTION_TIMING_MODE_FIXED_STEP)
			{
				ShowStatus(info + " - timing mismatch: path is variable-step",
					fb::NodeStatusMessageType::WARNING);
				return;
			}
			// 0 here means the fixed-step path has no nominal rate yet - skip the check.
			float pathRate = FrameRateFromTiming(params);
			if (pathRate > 0.0f && std::fabs(pathRate - LoadedFrameRate) > 0.01f)
			{
				ShowStatus(info + " - timing mismatch: path " + FrameRateToString(pathRate) + " FPS",
					fb::NodeStatusMessageType::WARNING);
				return;
			}
		}

		ShowStatus(info, fb::NodeStatusMessageType::INFO);
	}

	bool LoadFrame(const std::filesystem::path& filePath)
	{
		std::string utf8Path = nos::PathToUtf8(filePath);
		std::string fileName = nos::PathToUtf8(filePath.filename());
		std::ifstream file(filePath, std::ios::binary);

		uint8_t header[dpx::HEADER_SIZE];
		if (!file || !file.read(reinterpret_cast<char*>(header), dpx::HEADER_SIZE))
		{
			nosEngine.LogE("PlaybackClip: cannot read %s", utf8Path.c_str());
			ShowStatus("Cannot read " + fileName, fb::NodeStatusMessageType::FAILURE);
			return false;
		}

		dpx::ImageDesc desc;
		uint32_t dataOffset = 0;
		bool bigEndian = false;
		if (!dpx::ReadHeader(header, desc, dataOffset, bigEndian))
		{
			nosEngine.LogE("PlaybackClip: %s is not a supported DPX file", utf8Path.c_str());
			ShowStatus(fileName + " is not a supported DPX file",
				fb::NodeStatusMessageType::FAILURE);
			return false;
		}

		// An sRGB-encoded file loads into an _SRGB texture so the sampler decodes it back
		// to the graph's linear space; a linear file loads into a plain _UNORM texture.
		bool linear = desc.Transfer == dpx::TRANSFER_LINEAR;
		nosFormat format = NOS_FORMAT_NONE;
		if (desc.Channels == 4)
		{
			if (desc.BitDepth == 16) format = NOS_FORMAT_R16G16B16A16_UNORM;
			else format = linear ? NOS_FORMAT_R8G8B8A8_UNORM : NOS_FORMAT_R8G8B8A8_SRGB;
		}
		else if (desc.Channels == 3)
		{
			if (desc.BitDepth == 16) format = NOS_FORMAT_R16G16B16_UNORM;
			else format = linear ? NOS_FORMAT_R8G8B8_UNORM : NOS_FORMAT_R8G8B8_SRGB;
		}
		if (format == NOS_FORMAT_NONE)
		{
			nosEngine.LogE("PlaybackClip: unsupported DPX pixel layout in %s", utf8Path.c_str());
			ShowStatus(fileName + " has an unsupported pixel layout",
				fb::NodeStatusMessageType::FAILURE);
			return false;
		}

		std::vector<uint8_t> pixels(dpx::ImageDataSize(desc));
		file.seekg(dataOffset);
		if (!file.read(reinterpret_cast<char*>(pixels.data()), std::streamsize(pixels.size())))
		{
			nosEngine.LogE("PlaybackClip: %s is truncated", utf8Path.c_str());
			ShowStatus(fileName + " is truncated", fb::NodeStatusMessageType::FAILURE);
			return false;
		}
		// Our recorder writes little-endian; byte-swap 16-bit samples from big-endian DPX.
		if (bigEndian && desc.BitDepth == 16)
			for (size_t i = 0; i + 1 < pixels.size(); i += 2)
				std::swap(pixels[i], pixels[i + 1]);

		nosResourceShareInfo texInfo = {};
		texInfo.Info.Type = NOS_RESOURCE_TYPE_TEXTURE;
		texInfo.Info.Texture.Width = desc.Width;
		texInfo.Info.Texture.Height = desc.Height;
		texInfo.Info.Texture.Format = format;
		texInfo.Info.Texture.FieldType = NOS_TEXTURE_FIELD_TYPE_PROGRESSIVE;

		auto texture = vkss::Resource::Create(texInfo, "PlaybackClip Texture");
		if (!texture)
		{
			nosEngine.LogE("PlaybackClip: failed to allocate output texture");
			ShowStatus("Failed to allocate output texture", fb::NodeStatusMessageType::FAILURE);
			return false;
		}

		nosCmd cmd = vkss::BeginCmd(NOS_NAME("PlaybackClip Upload"), NodeId);
		nosVulkan->ImageLoad(cmd, pixels.data(), nosVec2u(desc.Width, desc.Height),
			format, &*texture, nullptr);
		nosCmdEndParams endParams{ .ForceSubmit = true };
		nosVulkan->End(cmd, &endParams);

		OutputTexture = std::move(texture);
		LoadedResolution = {desc.Width, desc.Height};
		LoadedFrameRate = desc.FrameRate;
		SetPinValue(NOS_NAME_STATIC("Texture"), OutputTexture->ToPinData());
		return true;
	}
};

nosResult RegisterPlaybackClip(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("PlaybackClip"), PlaybackClipNode, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::mediaio
