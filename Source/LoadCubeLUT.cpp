// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>

// External
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
#include <fstream>
#include <cstring>

using Clock = std::chrono::high_resolution_clock;

namespace nos::mediaio
{
NOS_REGISTER_NAME_SPACED(Nos_Utilities_LoadCubeLUT, "nos.mediaio.LoadCubeLUT")
NOS_REGISTER_NAME(Path)

enum State
{
	Loaded = 0,
	Loading = 1,
	Failed = 2,
};

template <typename T>
struct CubeLUT
{
	using Type = T;
	std::string Title;
	uint32_t LUT3DSize = 0;
	std::array<float, 3> DomainMin = {0.0f, 0.0f, 0.0f};
	std::array<float, 3> DomainMax = {1.0f, 1.0f, 1.0f};
	std::vector<T> LUT3DData;
};

template <typename T>
inline static std::unique_ptr<CubeLUT<T>> LoadCubeFile(const std::filesystem::path& path, bool pushAlpha = true)
{
	CubeLUT<T> file{};
	std::ifstream in(path);
	if (!in.is_open())
	{
		return nullptr;
	}

	std::string line;
	size_t dataLines = 0;
	uint32_t componentCount = pushAlpha ? 4 : 3; // RGB or RGBA
	while (std::getline(in, line))
	{
		// Trim whitespace
		line.erase(0, line.find_first_not_of(" \t\r\n"));
		line.erase(line.find_last_not_of(" \t\r\n") + 1);
		if (line.empty() || line[0] == '#')
			continue;
		if (line.find("TITLE", 0) == 0)
		{
			auto firstQuote = line.find('"');
			auto lastQuote = line.rfind('"');
			if (firstQuote != std::string::npos && lastQuote != std::string::npos && lastQuote > firstQuote)
				file.Title = line.substr(firstQuote + 1, lastQuote - firstQuote - 1);
		}
		else if (line.find("LUT_3D_SIZE", 0) == 0)
		{
			std::istringstream iss(line.substr(12));
			iss >> file.LUT3DSize;
			file.LUT3DData.reserve(file.LUT3DSize * file.LUT3DSize * file.LUT3DSize * componentCount);
		}
		else if (line.find("DOMAIN_MIN", 0) == 0)
		{
			std::istringstream iss(line.substr(10));
			iss >> file.DomainMin[0] >> file.DomainMin[1] >> file.DomainMin[2];
		}
		else if (line.find("DOMAIN_MAX", 0) == 0)
		{
			std::istringstream iss(line.substr(10));
			iss >> file.DomainMax[0] >> file.DomainMax[1] >> file.DomainMax[2];
		}
		else
		{
			// Try to parse LUT data
			float r, g, b;
			std::istringstream iss(line);
			if (iss >> r >> g >> b)
			{
				T rPush = T(std::clamp(r, 0.0f, 1.0f) * std::numeric_limits<T>::max());
				T gPush = T(std::clamp(g, 0.0f, 1.0f) * std::numeric_limits<T>::max());
				T bPush = T(std::clamp(b, 0.0f, 1.0f) * std::numeric_limits<T>::max());
				T aPush = T(std::numeric_limits<T>::max());
				if constexpr (std::is_same_v<T, float>)
				{
					rPush = r;
					gPush = g;
					bPush = b;
					aPush = 1.0f;
				}
				file.LUT3DData.push_back(rPush);
				file.LUT3DData.push_back(gPush);
				file.LUT3DData.push_back(bPush);
				if (pushAlpha)
					file.LUT3DData.push_back(aPush); // Push alpha channel as 1
				++dataLines;
			}
		}
	}
	// Validate
	size_t expected = file.LUT3DSize * file.LUT3DSize * file.LUT3DSize * componentCount;
	if (file.LUT3DSize == 0 || file.LUT3DData.size() != expected)
	{
		return nullptr;
	}
	return std::make_unique<CubeLUT<T>>(std::move(file));
}

struct LoadCubeLUTContext : NodeContext
{
	decltype(Clock::now()) TimeStarted = Clock::now();

	nosResult ExecuteNode(NodeExecuteParams const& params) override
	{
		nos::uuid outPinId = params[NSN_Out].Id;
		std::filesystem::path FilePath =
			nos::Utf8ToPath(params.GetPinData<const char>(NSN_Path));
		return LoadCubeFile(FilePath, outPinId);
	}

	void UpdateStatus(State newState, std::filesystem::path const& path)
	{
		auto messageDetailsFileRef = std::string("[File](") + NOS_URI_EXPLORER_PREFIX + nos::PathToUtf8(path) + ")";
		switch (newState)
		{
		case State::Loading:
			TimeStarted = Clock::now();
			SetNodeStatusMessage("Loading image: " + nos::PathToUtf8(path), fb::NodeStatusMessageType::INFO);
			break;
		case State::Loaded: {
			std::stringstream ss;
			auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - TimeStarted).count();
			std::string fileName = nos::PathToUtf8(path.filename());
			auto messageDetails = messageDetailsFileRef + " is loaded in " + std::to_string(dt) + "ms";
			SetNodeStatusMessages({{{},
									"3D LUT loaded: " + path.generic_string(),
									fb::NodeStatusMessageType::INFO,
									messageDetails,
									5,
									true,
									true}});
			break;
		}
		case State::Failed: {
			auto messageDetails = messageDetailsFileRef + " failed to load";
			SetNodeStatusMessages(
				{{{}, "Failed to load image", fb::NodeStatusMessageType::FAILURE, messageDetails, 10, true, true}});
			break;
		}
		}
	}

	nosResult LoadCubeFile(std::filesystem::path path, nosUUID outPinId)
	{
		UpdateStatus(State::Loading, path);
		std::string pathAsString = nos::PathToUtf8(path);
		try
		{
			auto cubeFile = ::nos::mediaio::LoadCubeFile<float>(path);
			if (!cubeFile)
			{
				nosEngine.LogE("Couldn't load cube LUT from %s.", path.c_str());
				UpdateStatus(State::Failed, path);
				return NOS_RESULT_FAILED;
			}

			nosResourceInfo resInfo = {
				.Type = NOS_RESOURCE_TYPE_TEXTURE3D,
						 .Texture3D = {.Base{.Width = cubeFile->LUT3DSize,
											 .Height = cubeFile->LUT3DSize,
											 .Format = NOS_FORMAT_R32G32B32A32_SFLOAT,},
									   .Depth = cubeFile->LUT3DSize} };

			auto outRes = sys::vulkan::CreateResource(resInfo, "Cube LUT");
			if (!outRes)
			{
				nosEngine.LogE("Failed to create 3D texture resource for cube LUT %s.", path.c_str());
				UpdateStatus(State::Failed, path);
				return NOS_RESULT_FAILED;
			}

			nosCmd cmd{};
			nosCmdBeginParams beginParams{
				.Name = NOS_NAME("ReadCubeFile Load"), .AssociatedNodeId = this->NodeId, .OutCmdHandle = &cmd};
			nosBufferInfo stagingBufInfo = {
				.Size = (uint32_t)(cubeFile->LUT3DData.size() * sizeof(*cubeFile->LUT3DData.data())),
				.Usage = NOS_BUFFER_USAGE_TRANSFER_SRC,
				.MemoryFlags = NOS_MEMORY_FLAGS_HOST_VISIBLE,
			};
			auto stagingBuf = sys::vulkan::CreateBuffer(stagingBufInfo, "LoadCubeLUT_StagingBuf");
			if (!stagingBuf)
			{
				nosEngine.LogE("Failed to create staging buffer for cube LUT file %s.", path.c_str());
				UpdateStatus(State::Failed, path);
				return NOS_RESULT_FAILED;
			}
			auto stagingBufPtr = nosVulkan->Map(stagingBuf);
			if (!stagingBufPtr)
			{
				nosEngine.LogE("Failed to map staging buffer for cube LUT file %s.", path.c_str());
				UpdateStatus(State::Failed, path);
				return NOS_RESULT_FAILED;
			}
			std::memcpy(stagingBufPtr,
						cubeFile->LUT3DData.data(),
						cubeFile->LUT3DData.size() * sizeof(*cubeFile->LUT3DData.data()));
			nosVulkan->Begin(&beginParams);
			nosVulkan->Copy(cmd, stagingBuf, outRes, nullptr);
			nosCmdEndParams endParams{.ForceSubmit = true};
			nosVulkan->End(cmd, &endParams);

			SetPinObject(outPinId, outRes);
			UpdateStatus(State::Loaded, path);
		}
		catch (const std::exception& e)
		{
			nosEngine.LogE("Error while loading cube file: %s", e.what());
			UpdateStatus(State::Failed, path);
		}
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterLoadCubeLUT(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NSN_Nos_Utilities_LoadCubeLUT, LoadCubeLUTContext, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::mediaio
