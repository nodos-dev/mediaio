// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>

// External
#define STBI_WINDOWS_UTF8
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <nosCppUtilities.hpp>

// Framework
#include <Builtins_generated.h>
#include <AppService_generated.h>

// nosNodes
#include <nosSysVulkan/Helpers.hpp>

#include "Names.h"

#include <atomic>
#include <chrono>
#include <sstream>

using Clock = std::chrono::high_resolution_clock;

namespace nos::mediaio
{
NOS_REGISTER_NAME_SPACED(Nos_Utilities_StbiLoad, "nos.mediaio.StbiLoad")

enum State
{
    Idle = 0,
    Loading = 1,
    Failed = 2,
};

struct StbiLoadContext : NodeContext
{
	decltype(Clock::now()) TimeStarted = Clock::now();

	nosResult ExecuteNode(NodeExecuteParams const& params) override {
		nos::uuid outPinId = params[NSN_Out].Id;
		bool sRGB = *params.GetPinValue<bool>(NSN_sRGB);
		std::filesystem::path FilePath = nos::Utf8ToPath(params.GetPinValue<const char*>(NSN_Path));
		return LoadImage(FilePath, outPinId, sRGB);
	}

	void UpdateStatus(State newState, std::filesystem::path path)
	{
		auto messageDetailsFileRef = std::string("[File](") + NOS_URI_EXPLORER_PREFIX + nos::PathToUtf8(path) + ")";
        switch(newState)
        {
        case State::Loading:
		    TimeStarted = Clock::now();
			SetNodeStatusMessage("Loading image: " + path.generic_string(), fb::NodeStatusMessageType::INFO);
            break;
        case State::Idle:
        {
			std::stringstream ss;
			auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - TimeStarted).count();
			std::string fileName = nos::PathToUtf8(path.filename());
			auto messageDetails = messageDetailsFileRef + " is loaded in " + std::to_string(dt) + "ms";
			SetNodeStatusMessages({{{}, "Image loaded: " + path.generic_string(), fb::NodeStatusMessageType::INFO, messageDetails, 5, true, true}});
            break;
        }
        case State::Failed:
		{
			auto messageDetails = messageDetailsFileRef + " failed to load";
			SetNodeStatusMessages({{{}, "Failed to load image", fb::NodeStatusMessageType::FAILURE, messageDetails, 10, true, true} });
			break;
		}
        }
	}

	nosResult LoadImage(std::filesystem::path path, nosUUID outPinId, bool sRGB)
	{
		UpdateStatus(State::Loading, path);
		std::string pathUtf8 = nos::PathToUtf8(path);
		try
		{
			int w, h, n;
			uint8_t* img = stbi_load(pathUtf8.c_str(), &w, &h, &n, 4);
			if (!img)
			{
				nosEngine.LogE("Couldn't load image from %s.", path.c_str());
				UpdateStatus(State::Failed, path);
				return NOS_RESULT_FAILED;
			}

			nosTextureInfo texInfo = {.Width = (uint32_t)w, .Height = (uint32_t)h, .Format = NOS_FORMAT_R16G16B16A16_UNORM };

			// unless reading raw bytes, this is useless since samplers convert to linear space automatically
			if (sRGB)
				texInfo.Format = NOS_FORMAT_R8G8B8A8_SRGB;

			auto outTex = sys::vulkan::CreateTexture(texInfo, "ReadImage Texture");
			if (!outTex.IsValid())
			{
				nosEngine.LogE("Failed to create texture resource for image %s.", path.c_str());
				UpdateStatus(State::Failed, path);
				return NOS_RESULT_FAILED;
			}

			nosCmd cmd{};
			nosCmdBeginParams beginParams{
			.Name = NOS_NAME("ReadImage Load"),
			.AssociatedNodeId = this->NodeId,
			.OutCmdHandle = &cmd
			};
			nosVulkan->Begin(&beginParams);
			// TODO: Transfer filter?
			nosVulkan->ImageLoad(cmd, img, nosVec2u(w, h), NOS_FORMAT_R8G8B8A8_SRGB, outTex, NOS_TEXTURE_FILTER_NEAREST);
			nosCmdEndParams endParams{ .ForceSubmit = true };
			nosVulkan->End(cmd, &endParams);

			nosEngine.SetPinObject(outPinId, outTex);

			free(img);
			UpdateStatus(State::Idle, path);
		}
		catch (const std::exception& e)
		{
			nosEngine.LogE("Error while loading image: %s", e.what());
			UpdateStatus(State::Failed, path);
		}
		return NOS_RESULT_SUCCESS;
	}

};

nosResult RegisterStbiLoad(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NSN_Nos_Utilities_StbiLoad, StbiLoadContext, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::mediaio