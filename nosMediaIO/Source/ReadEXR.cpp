// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>
#include <nosVulkanSubsystem/Helpers.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#define TINYEXR_IMPLEMENTATION
#define TINYEXR_USE_MINIZ (1)
#include <tinyexr.h>

#include "ReadEXR_generated.h"

namespace nos::mediaio
{

// Combo-box entry standing for "load the file's default RGBA channels" (an empty layer
// name). Listed first so it is always selectable, even for files with no named layers.
static constexpr const char* DEFAULT_LAYER = "(default)";

// Reads an OpenEXR image from Path and emits it as a Texture, with the file's header on
// the Metadata output. Decodes only when Path or Layer changes; a load failure holds the
// previously loaded frame. HALF files load into an RGBA16F texture, FLOAT/UINT files into
// RGBA32F, so the output keeps the file's precision. tinyexr always hands back
// interleaved float RGBA - for a HALF target the floats are packed back down to half.
struct ReadEXRNode : NodeContext
{
	std::optional<vkss::Resource> OutputTexture;
	std::optional<vkss::Resource> OutputDepth;
	std::string LoadedPath;   // Path currently held on the output pins
	std::string LoadedLayer;  // Layer currently held on the output pins
	std::string StatusText;
	int StatusType = -1;

	ReadEXRNode(nosFbNodePtr node) : NodeContext(node)
	{
		// Turn Layer into a combo box populated per node instance. Starts with just the
		// default entry; RefreshLayerList fills in the file's layers once Path is set.
		UpdateStringList(GetLayerListName(), { DEFAULT_LAYER });
		SetPinVisualizer(NOS_NAME_STATIC("Layer"),
			{ .type = nos::fb::VisualizerType::COMBO_BOX, .name = GetLayerListName() });
	}

	// Per-instance string-list name backing the Layer combo box.
	std::string GetLayerListName() const
	{
		return "mediaio.ReadEXR.Layers." + std::string(NodeId);
	}

	// Sets the node status, skipping the update when nothing changed.
	void ShowStatus(const std::string& text, fb::NodeStatusMessageType type)
	{
		if (int(type) == StatusType && text == StatusText)
			return;
		StatusText = text;
		StatusType = int(type);
		SetNodeStatusMessage(text, type);
	}

	static std::string ReadStringPin(nos::NodeExecuteParams& params, nosName name)
	{
		auto& pin = params[name];
		if (pin.Data && pin.Data->Data && pin.Data->Size)
			return std::string(static_cast<const char*>(pin.Data->Data));
		return {};
	}

	nosResult ExecuteNode(nosNodeExecuteParams* execParams) override
	{
		nos::NodeExecuteParams params(execParams);

		std::string path = ReadStringPin(params, NOS_NAME_STATIC("Path"));
		std::string layer = ReadStringPin(params, NOS_NAME_STATIC("Layer"));
		if (path.empty())
		{
			ShowStatus("Set input file", fb::NodeStatusMessageType::WARNING);
			return NOS_RESULT_SUCCESS; // hold last frame
		}

		if (path == LoadedPath && layer == LoadedLayer && OutputTexture)
			return NOS_RESULT_SUCCESS; // already holding this image

		if (LoadFile(path, layer))
		{
			LoadedPath = path;
			LoadedLayer = layer;
		}
		return NOS_RESULT_SUCCESS; // load failure holds the last frame
	}

	// float -> IEEE 754 binary16 (round-to-nearest-even), for packing tinyexr's float
	// output into a HALF texture. Handles denormals, overflow-to-inf and NaN.
	static uint16_t FloatToHalf(float f)
	{
		uint32_t x;
		std::memcpy(&x, &f, sizeof(x));
		uint32_t sign = (x >> 16) & 0x8000u;
		int32_t exp = int32_t((x >> 23) & 0xFF) - 127 + 15;
		uint32_t mant = x & 0x7FFFFFu;

		if (((x >> 23) & 0xFF) == 0xFF) // Inf / NaN
			return uint16_t(sign | 0x7C00u | (mant ? 0x200u : 0u));
		if (exp >= 0x1F) // overflow -> Inf
			return uint16_t(sign | 0x7C00u);
		if (exp <= 0) // subnormal / underflow
		{
			if (exp < -10)
				return uint16_t(sign);
			mant |= 0x800000u;
			uint32_t shift = uint32_t(14 - exp);
			uint32_t half = mant >> shift;
			if ((mant >> (shift - 1)) & 1u) // round to nearest even
				half += 1;
			return uint16_t(sign | half);
		}
		uint16_t half = uint16_t(sign | (uint32_t(exp) << 10) | (mant >> 13));
		if (mant & 0x1000u) // round to nearest even
			half += 1;
		return half;
	}

	// IEEE 754 binary16 -> float, for reading HALF channel samples (e.g. a HALF Z channel).
	static float HalfToFloat(uint16_t h)
	{
		uint32_t sign = uint32_t(h & 0x8000u) << 16;
		uint32_t exp = (h >> 10) & 0x1Fu;
		uint32_t mant = h & 0x3FFu;
		uint32_t f;
		if (exp == 0)
		{
			if (mant == 0)
				f = sign; // +/- 0
			else
			{
				exp = 127 - 15 + 1; // normalize subnormal
				while ((mant & 0x400u) == 0) { mant <<= 1; --exp; }
				mant &= 0x3FFu;
				f = sign | (exp << 23) | (mant << 13);
			}
		}
		else if (exp == 0x1Fu)
			f = sign | 0x7F800000u | (mant << 13); // Inf / NaN
		else
			f = sign | ((exp - 15 + 127) << 23) | (mant << 13);
		float out;
		std::memcpy(&out, &f, sizeof(out));
		return out;
	}

	static EXRPixelType MapPixelType(const EXRHeader& header)
	{
		if (header.num_channels == 0)
			return EXRPixelType::MIXED;
		int first = header.pixel_types[0];
		for (int i = 1; i < header.num_channels; ++i)
			if (header.pixel_types[i] != first)
				return EXRPixelType::MIXED;
		switch (first)
		{
		case TINYEXR_PIXELTYPE_UINT:  return EXRPixelType::UINT;
		case TINYEXR_PIXELTYPE_HALF:  return EXRPixelType::HALF;
		case TINYEXR_PIXELTYPE_FLOAT: return EXRPixelType::FLOAT;
		default:                      return EXRPixelType::MIXED;
		}
	}

	static const char* CompressionName(int type)
	{
		switch (type)
		{
		case TINYEXR_COMPRESSIONTYPE_NONE:  return "none";
		case TINYEXR_COMPRESSIONTYPE_RLE:   return "rle";
		case TINYEXR_COMPRESSIONTYPE_ZIPS:  return "zips";
		case TINYEXR_COMPRESSIONTYPE_ZIP:   return "zip";
		case TINYEXR_COMPRESSIONTYPE_PIZ:   return "piz";
		case TINYEXR_COMPRESSIONTYPE_PXR24: return "pxr24";
		case TINYEXR_COMPRESSIONTYPE_B44:   return "b44";
		case TINYEXR_COMPRESSIONTYPE_B44A:  return "b44a";
		case TINYEXR_COMPRESSIONTYPE_DWAA:  return "dwaa";
		case TINYEXR_COMPRESSIONTYPE_DWAB:  return "dwab";
		default:                            return "unknown";
		}
	}

	bool LoadFile(const std::string& path, const std::string& layer)
	{
		std::filesystem::path fsPath = nos::Utf8ToPath(path);
		std::string fileName = nos::PathToUtf8(fsPath.filename());
		const char* cPath = path.c_str();

		// Parse the header first: it drives output precision and the Metadata pin, and
		// gives a clear diagnostic when the file is not a valid EXR.
		EXRVersion version;
		if (ParseEXRVersionFromFile(&version, cPath) != TINYEXR_SUCCESS)
		{
			nosEngine.LogE("ReadEXR: %s is not an EXR file", path.c_str());
			ShowStatus(fileName + " is not an EXR file", fb::NodeStatusMessageType::FAILURE);
			return false;
		}

		EXRHeader header;
		InitEXRHeader(&header);
		const char* err = nullptr;
		if (ParseEXRHeaderFromFile(&header, &version, cPath, &err) != TINYEXR_SUCCESS)
		{
			std::string msg = err ? err : "cannot read header";
			nosEngine.LogE("ReadEXR: %s: %s", path.c_str(), msg.c_str());
			ShowStatus(fileName + ": " + msg, fb::NodeStatusMessageType::FAILURE);
			FreeEXRErrorMessage(err);
			return false;
		}

		// Count parts for the Metadata pin. Single-part is the common case (1); a true
		// multipart file needs the multipart header parse just to learn how many.
		uint32_t numParts = 1;
		if (version.multipart)
		{
			EXRHeader** mheaders = nullptr;
			int mcount = 0;
			const char* mErr = nullptr;
			if (ParseEXRMultipartHeaderFromFile(&mheaders, &mcount, &version, cPath, &mErr)
				== TINYEXR_SUCCESS)
			{
				numParts = uint32_t(mcount);
				for (int i = 0; i < mcount; ++i)
				{
					FreeEXRHeader(mheaders[i]);
					free(mheaders[i]);
				}
				free(mheaders);
			}
			FreeEXRErrorMessage(mErr);
		}

		// Populate the Layer combo box from this file so the user can see and pick the
		// layers it contains. Done before decoding so the list is fresh even if the
		// currently selected layer then fails to load.
		std::vector<std::string> layerNames = ReadLayerNames(path);
		std::vector<std::string> layerOptions;
		layerOptions.reserve(layerNames.size() + 1);
		layerOptions.emplace_back(DEFAULT_LAYER);
		layerOptions.insert(layerOptions.end(), layerNames.begin(), layerNames.end());
		UpdateStringList(GetLayerListName(), layerOptions);

		// The default entry (or an empty pin) loads the file's default channels.
		std::string layerArg = (layer.empty() || layer == DEFAULT_LAYER) ? std::string() : layer;

		// Decode to interleaved float RGBA.
		float* rgba = nullptr;
		int width = 0, height = 0;
		err = nullptr;
		int ret = LoadEXRWithLayer(&rgba, &width, &height, cPath,
			layerArg.empty() ? nullptr : layerArg.c_str(), &err);
		if (ret != TINYEXR_SUCCESS)
		{
			std::string msg = err ? err : "decode failed";
			if (ret == TINYEXR_ERROR_LAYER_NOT_FOUND)
				msg = "layer '" + layerArg + "' not found";
			nosEngine.LogE("ReadEXR: %s: %s", path.c_str(), msg.c_str());
			ShowStatus(fileName + ": " + msg, fb::NodeStatusMessageType::FAILURE);
			FreeEXRErrorMessage(err);
			FreeEXRHeader(&header);
			return false;
		}

		// HALF files stay half; everything else (FLOAT, UINT, mixed) goes to full float.
		EXRPixelType pixelType = MapPixelType(header);
		bool half = pixelType == EXRPixelType::HALF;
		nosFormat format = half ? NOS_FORMAT_R16G16B16A16_SFLOAT
		                        : NOS_FORMAT_R32G32B32A32_SFLOAT;
		const size_t texelCount = size_t(width) * size_t(height) * 4;

		nosResourceShareInfo texInfo = {};
		texInfo.Info.Type = NOS_RESOURCE_TYPE_TEXTURE;
		texInfo.Info.Texture.Width = uint32_t(width);
		texInfo.Info.Texture.Height = uint32_t(height);
		texInfo.Info.Texture.Format = format;
		texInfo.Info.Texture.FieldType = NOS_TEXTURE_FIELD_TYPE_PROGRESSIVE;

		auto texture = vkss::Resource::Create(texInfo, "ReadEXR Texture");
		if (!texture)
		{
			nosEngine.LogE("ReadEXR: failed to allocate output texture");
			ShowStatus("Failed to allocate output texture", fb::NodeStatusMessageType::FAILURE);
			free(rgba);
			FreeEXRHeader(&header);
			return false;
		}

		nosCmd cmd = vkss::BeginCmd(NOS_NAME("ReadEXR Upload"), NodeId);
		if (half)
		{
			std::vector<uint16_t> halfPixels(texelCount);
			for (size_t i = 0; i < texelCount; ++i)
				halfPixels[i] = FloatToHalf(rgba[i]);
			nosVulkan->ImageLoad(cmd, halfPixels.data(), nosVec2u(width, height),
				format, &*texture, nullptr);
			nosCmdEndParams endParams{ .ForceSubmit = true };
			nosVulkan->End(cmd, &endParams);
		}
		else
		{
			nosVulkan->ImageLoad(cmd, rgba, nosVec2u(width, height), format, &*texture, nullptr);
			nosCmdEndParams endParams{ .ForceSubmit = true };
			nosVulkan->End(cmd, &endParams);
		}

		OutputTexture = std::move(texture);
		SetPinValue(NOS_NAME_STATIC("Texture"), OutputTexture->ToPinData());
		EmitMetadata(header, pixelType, width, height, numParts, layerNames, layerArg);

		// Depth is a separate output. LoadEXRWithLayer only returns RGBA, so pull the Z
		// channel with a second, low-level decode - done only when a Z channel exists.
		LoadDepth(cPath, header, layerArg, width, height);

		free(rgba);
		FreeEXRHeader(&header);

		std::string info = "Loaded " + fileName + " - " + std::to_string(width) + "x"
			+ std::to_string(height) + " " + (half ? "RGBA16F" : "RGBA32F");
		ShowStatus(info, fb::NodeStatusMessageType::INFO);
		return true;
	}

	// Finds the depth channel ('Z', base-name match) belonging to the selected layer.
	// For the default layer (empty layerArg) that is a top-level channel named "Z".
	// Returns its channel index, or -1 when the layer has no Z channel.
	static int FindDepthChannel(const EXRHeader& header, const std::string& layerArg)
	{
		for (int i = 0; i < header.num_channels; ++i)
		{
			std::string name = header.channels[i].name;
			std::string prefix, base;
			auto dot = name.rfind('.');
			if (dot == std::string::npos)
				base = name;
			else { prefix = name.substr(0, dot); base = name.substr(dot + 1); }
			if (prefix == layerArg && base.size() == 1 && (base[0] == 'Z' || base[0] == 'z'))
				return i;
		}
		return -1;
	}

	// Emits the selected layer's Z channel on the Depth output as an R32F texture. No-op
	// (leaving the Depth output as it was) when the file / layer has no Z channel.
	void LoadDepth(const char* cPath, const EXRHeader& header, const std::string& layerArg,
		int width, int height)
	{
		int zIdx = FindDepthChannel(header, layerArg);
		if (zIdx < 0)
			return;

		EXRImage image;
		InitEXRImage(&image);
		const char* err = nullptr;
		if (LoadEXRImageFromFile(&image, &header, cPath, &err) != TINYEXR_SUCCESS)
		{
			nosEngine.LogW("ReadEXR: could not decode depth: %s", err ? err : "unknown");
			FreeEXRErrorMessage(err);
			return;
		}
		if (image.width != width || image.height != height || zIdx >= image.num_channels
			|| !image.images)
		{
			FreeEXRImage(&image);
			return;
		}

		const size_t count = size_t(width) * size_t(height);
		std::vector<float> depth(count);
		const unsigned char* src = image.images[zIdx];
		int type = header.pixel_types[zIdx];
		for (size_t i = 0; i < count; ++i)
		{
			if (type == TINYEXR_PIXELTYPE_HALF)
				depth[i] = HalfToFloat(reinterpret_cast<const uint16_t*>(src)[i]);
			else if (type == TINYEXR_PIXELTYPE_UINT)
				depth[i] = float(reinterpret_cast<const uint32_t*>(src)[i]);
			else
				depth[i] = reinterpret_cast<const float*>(src)[i];
		}
		FreeEXRImage(&image);

		nosResourceShareInfo texInfo = {};
		texInfo.Info.Type = NOS_RESOURCE_TYPE_TEXTURE;
		texInfo.Info.Texture.Width = uint32_t(width);
		texInfo.Info.Texture.Height = uint32_t(height);
		texInfo.Info.Texture.Format = NOS_FORMAT_R32_SFLOAT;
		texInfo.Info.Texture.FieldType = NOS_TEXTURE_FIELD_TYPE_PROGRESSIVE;

		auto texture = vkss::Resource::Create(texInfo, "ReadEXR Depth");
		if (!texture)
		{
			nosEngine.LogE("ReadEXR: failed to allocate depth texture");
			return;
		}

		nosCmd cmd = vkss::BeginCmd(NOS_NAME("ReadEXR Depth Upload"), NodeId);
		nosVulkan->ImageLoad(cmd, depth.data(), nosVec2u(width, height),
			NOS_FORMAT_R32_SFLOAT, &*texture, nullptr);
		nosCmdEndParams endParams{ .ForceSubmit = true };
		nosVulkan->End(cmd, &endParams);

		OutputDepth = std::move(texture);
		SetPinValue(NOS_NAME_STATIC("Depth"), OutputDepth->ToPinData());
	}

	// Reads the selectable layer names from the EXR file (empty for a plain RGBA file).
	static std::vector<std::string> ReadLayerNames(const std::string& path)
	{
		std::vector<std::string> layerNames;
		const char** layers = nullptr;
		int numLayers = 0;
		const char* err = nullptr;
		if (EXRLayers(path.c_str(), &layers, &numLayers, &err) == TINYEXR_SUCCESS && layers)
		{
			for (int i = 0; i < numLayers; ++i)
			{
				if (layers[i])
				{
					layerNames.emplace_back(layers[i]);
					free((void*)layers[i]);
				}
			}
			free((void*)layers);
		}
		FreeEXRErrorMessage(err);
		return layerNames;
	}

	// Builds the EXRMetadata table from the parsed header and the file's layer list.
	void EmitMetadata(const EXRHeader& header, EXRPixelType pixelType, int width, int height,
		uint32_t numParts, const std::vector<std::string>& layerNames, const std::string& loadedLayer)
	{
		std::vector<std::string> channels;
		channels.reserve(header.num_channels);
		for (int i = 0; i < header.num_channels; ++i)
			channels.emplace_back(header.channels[i].name);

		flatbuffers::FlatBufferBuilder fbb;
		auto compression = fbb.CreateString(CompressionName(header.compression_type));
		auto channelsVec = fbb.CreateVectorOfStrings(channels);
		auto layersVec = fbb.CreateVectorOfStrings(layerNames);
		auto loadedLayerStr = fbb.CreateString(loadedLayer);
		Box2i dataWindow(header.data_window.min_x, header.data_window.min_y,
			header.data_window.max_x, header.data_window.max_y);
		Box2i displayWindow(header.display_window.min_x, header.display_window.min_y,
			header.display_window.max_x, header.display_window.max_y);

		EXRMetadataBuilder mb(fbb);
		mb.add_width(uint32_t(width));
		mb.add_height(uint32_t(height));
		mb.add_data_window(&dataWindow);
		mb.add_display_window(&displayWindow);
		mb.add_pixel_aspect_ratio(header.pixel_aspect_ratio);
		mb.add_pixel_type(pixelType);
		mb.add_compression(compression);
		mb.add_channels(channelsVec);
		mb.add_layers(layersVec);
		mb.add_num_parts(numParts);
		mb.add_loaded_layer(loadedLayerStr);
		fbb.Finish(mb.Finish());

		SetPinValue(NOS_NAME_STATIC("Metadata"), nos::Buffer(fbb.Release()));
	}
};

nosResult RegisterReadEXR(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("ReadEXR"), ReadEXRNode, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::mediaio
