// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>
#include <nosVulkanSubsystem/Helpers.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "Dpx.h"

namespace nos::mediaio
{

// Reads a DPX file from Path and emits it on the Texture output. Loads only when Path
// changes; a load failure holds the previously loaded frame. The output texture format
// follows the DPX transfer tag - an sRGB-encoded file becomes an _SRGB texture so the
// graph's sampler decodes it back to linear automatically. Progress and errors are
// surfaced on the node status.
struct ReadDPXNode : NodeContext
{
	std::optional<vkss::Resource> OutputTexture;
	std::string LoadedPath;           // Path currently held on the output pin
	std::string StatusText;
	int StatusType = -1;

	ReadDPXNode(nosFbNodePtr node) : NodeContext(node) {}

	// Sets the node status, skipping the update when nothing changed.
	void ShowStatus(const std::string& text, fb::NodeStatusMessageType type)
	{
		if (int(type) == StatusType && text == StatusText)
			return;
		StatusText = text;
		StatusType = int(type);
		SetNodeStatusMessage(text, type);
	}

	nosResult ExecuteNode(nosNodeExecuteParams* execParams) override
	{
		nos::NodeExecuteParams params(execParams);

		auto* pathPin = params.GetPinData<const char>(NOS_NAME_STATIC("Path"));
		std::string path = pathPin ? pathPin : "";
		if (path.empty())
		{
			ShowStatus("Set input file", fb::NodeStatusMessageType::WARNING);
			return NOS_RESULT_SUCCESS; // hold last frame
		}

		if (path == LoadedPath && OutputTexture)
			return NOS_RESULT_SUCCESS; // already holding this image

		if (LoadFrame(nos::Utf8ToPath(path)))
			LoadedPath = path;
		return NOS_RESULT_SUCCESS; // load failure holds the last frame
	}

	bool LoadFrame(const std::filesystem::path& filePath)
	{
		std::string utf8Path = nos::PathToUtf8(filePath);
		std::string fileName = nos::PathToUtf8(filePath.filename());
		std::ifstream file(filePath, std::ios::binary);

		uint8_t header[dpx::HEADER_SIZE];
		if (!file || !file.read(reinterpret_cast<char*>(header), dpx::HEADER_SIZE))
		{
			nosEngine.LogE("ReadDPX: cannot read %s", utf8Path.c_str());
			ShowStatus("Cannot read " + fileName, fb::NodeStatusMessageType::FAILURE);
			return false;
		}

		dpx::ImageDesc desc;
		uint32_t dataOffset = 0;
		bool bigEndian = false;
		if (!dpx::ReadHeader(header, desc, dataOffset, bigEndian))
		{
			nosEngine.LogE("ReadDPX: %s is not a supported DPX file", utf8Path.c_str());
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
			nosEngine.LogE("ReadDPX: unsupported DPX pixel layout in %s", utf8Path.c_str());
			ShowStatus(fileName + " has an unsupported pixel layout",
				fb::NodeStatusMessageType::FAILURE);
			return false;
		}

		std::vector<uint8_t> pixels(dpx::ImageDataSize(desc));
		file.seekg(dataOffset);
		if (!file.read(reinterpret_cast<char*>(pixels.data()), std::streamsize(pixels.size())))
		{
			nosEngine.LogE("ReadDPX: %s is truncated", utf8Path.c_str());
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

		auto texture = vkss::Resource::Create(texInfo, "ReadDPX Texture");
		if (!texture)
		{
			nosEngine.LogE("ReadDPX: failed to allocate output texture");
			ShowStatus("Failed to allocate output texture", fb::NodeStatusMessageType::FAILURE);
			return false;
		}

		nosCmd cmd = vkss::BeginCmd(NOS_NAME("ReadDPX Upload"), NodeId);
		nosVulkan->ImageLoad(cmd, pixels.data(), nosVec2u(desc.Width, desc.Height),
			format, &*texture, nullptr);
		nosCmdEndParams endParams{ .ForceSubmit = true };
		nosVulkan->End(cmd, &endParams);

		OutputTexture = std::move(texture);
		SetPinValue(NOS_NAME_STATIC("Texture"), OutputTexture->ToPinData());

		ShowStatus("Loaded " + fileName + " - " + std::to_string(desc.Width) + "x"
			+ std::to_string(desc.Height), fb::NodeStatusMessageType::INFO);
		return true;
	}
};

nosResult RegisterReadDPX(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("ReadDPX"), ReadDPXNode, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::mediaio
