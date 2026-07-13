// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>
#include <nosSysVulkan/Helpers.hpp>

#include <string>
#include <vector>

#include "ExrDecode.h"

namespace nos::mediaio
{

// Combo-box entry standing for "load the file's default RGBA channels" (an empty layer
// name). Listed first so it is always selectable, even for files with no named layers.
static constexpr const char* DEFAULT_LAYER = "(default)";

// Reads an OpenEXR file from Path and emits it as a Texture, its Z channel (if any) on Depth,
// and its header on Metadata. Decodes only when Path or Layer changes; a load failure holds
// the previously loaded frame. Output keeps the file's precision (HALF -> RGBA16F, else
// RGBA32F). Layer is a combo box populated from the file's layers. Decoding is shared with
// the sequence player via ExrDecode.
struct ReadEXRNode : NodeContext
{
	TypedObjectRef<sys::vulkan::Texture> OutputTexture;
	TypedObjectRef<sys::vulkan::Texture> OutputDepth;
	std::string LoadedPath;   // Path currently held on the output pins
	std::string LoadedLayer;  // Layer currently held on the output pins
	std::string StatusText;
	int StatusType = -1;

	nosResult OnCreate(nosFbNodePtr node) override
	{
		// Turn Layer into a combo box populated per node instance. Starts with just the
		// default entry; a load fills in the file's layers.
		UpdateStringList(GetLayerListName(), { DEFAULT_LAYER });
		SetPinVisualizer(NOS_NAME_STATIC("Layer"),
			{ .type = nos::fb::VisualizerType::COMBO_BOX, .name = GetLayerListName() });
		return NOS_RESULT_SUCCESS;
	}

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

	nosResult ExecuteNode(NodeExecuteParams const& params) override
	{
		const char* pathPin = params.GetPinValue<const char*>(NOS_NAME_STATIC("Path"));
		const char* layerPin = params.GetPinValue<const char*>(NOS_NAME_STATIC("Layer"));
		std::string path = pathPin ? pathPin : "";
		std::string layer = layerPin ? layerPin : "";
		if (path.empty())
		{
			ShowStatus("Set input file", fb::NodeStatusMessageType::WARNING);
			return NOS_RESULT_SUCCESS; // hold last frame
		}

		if (path == LoadedPath && layer == LoadedLayer && OutputTexture.IsValid())
			return NOS_RESULT_SUCCESS; // already holding this image

		std::string layerArg = (layer.empty() || layer == DEFAULT_LAYER) ? std::string() : layer;
		std::string fileName = nos::PathToUtf8(nos::Utf8ToPath(path).filename());

		ExrFrame frame;
		std::string err;
		bool ok = DecodeExr(path, layerArg, frame, err);

		// Refresh the Layer combo from whatever the file exposed, even on a decode failure.
		std::vector<std::string> options;
		options.reserve(frame.Layers.size() + 1);
		options.emplace_back(DEFAULT_LAYER);
		options.insert(options.end(), frame.Layers.begin(), frame.Layers.end());
		UpdateStringList(GetLayerListName(), options);

		if (!ok)
		{
			nosEngine.LogE("ReadEXR: %s: %s", path.c_str(), err.c_str());
			ShowStatus(fileName + ": " + err, fb::NodeStatusMessageType::FAILURE);
			return NOS_RESULT_SUCCESS; // hold last frame
		}

		auto texture = UploadColor(frame, NodeId);
		if (!texture.IsValid())
		{
			ShowStatus("Failed to allocate output texture", fb::NodeStatusMessageType::FAILURE);
			return NOS_RESULT_SUCCESS;
		}
		OutputTexture = std::move(texture);
		SetPinObject(NOS_NAME_STATIC("Texture"), OutputTexture);

		if (frame.HasDepth)
		{
			if (auto depth = UploadDepth(frame, NodeId); depth.IsValid())
			{
				OutputDepth = std::move(depth);
				SetPinObject(NOS_NAME_STATIC("Depth"), OutputDepth);
			}
		}

		SetPinValue(NOS_NAME_STATIC("Metadata"), BuildExrMetadata(frame));

		LoadedPath = path;
		LoadedLayer = layer;

		std::string info = "Loaded " + fileName + " - " + std::to_string(frame.Width) + "x"
			+ std::to_string(frame.Height) + " "
			+ (frame.ColorFormat == NOS_FORMAT_R16G16B16A16_SFLOAT ? "RGBA16F" : "RGBA32F")
			+ (frame.HasDepth ? " +depth" : "");
		ShowStatus(info, fb::NodeStatusMessageType::INFO);
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterReadEXR(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("ReadEXR"), ReadEXRNode, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::mediaio
