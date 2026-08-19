// Copyright MediaZ Teknoloji A.S. All Rights Reserved.
#include <Nodos/PluginHelpers.hpp>

#include <nosVulkanSubsystem/Helpers.hpp>
#include <glm/glm.hpp>
#include <optional>
#include "ColorSpaceCoeffs.hpp"
#include "Names.h"

NOS_REGISTER_NAME(Channel);
NOS_REGISTER_NAME(Format);
NOS_REGISTER_NAME(Channel_Viewer_Pass);
NOS_REGISTER_NAME(Channel_Viewer_Shader);
NOS_REGISTER_NAME_SPACED(Nos_MediaIO_ChannelViewer, "nos.mediaio.ChannelViewer")

namespace nos::mediaio
{
// The Format pin used to be nos.utilities.ChannelViewerFormats, and nos.fb.ChannelViewerFormats
// before that. Taking this node over from nos.utilities in 2.12.0 replaced them with
// nos.mediaio.ColorSpace, which numbers its values differently:
//     old: Rec_601 = 0, Rec_709 = 1, Rec_2020 = 2
//     new: REC709  = 0, REC601   = 1, REC2020  = 2
// A pin saved while the old type still existed holds four bytes in the old numbering. Once the
// old type was gone the engine could no longer parse the pin and kept the JSON text instead,
// quotes and all. Both shapes have to become four bytes of ColorSpace here: nothing further
// down the line reads pin data back as text. 2.12.0 renamed the type but left the value
// alone, so the pin ended up holding text under a four byte type; 2.13.1 converts the value.
static std::optional<ColorSpace> ReadLegacyFormat(std::vector<uint8_t> const& data)
{
	if (data.size() == sizeof(uint32_t))
	{
		switch (*reinterpret_cast<const uint32_t*>(data.data()))
		{
		case 0: return ColorSpace::REC601;
		case 1: return ColorSpace::REC709;
		case 2: return ColorSpace::REC2020;
		}
		return std::nullopt;
	}
	std::string_view name(reinterpret_cast<const char*>(data.data()), data.size());
	while (!name.empty() && (name.back() == '\0' || name.back() == '"'))
		name.remove_suffix(1);
	if (!name.empty() && name.front() == '"')
		name.remove_prefix(1);
	if (name == "Rec_601" || name == "REC601") return ColorSpace::REC601;
	if (name == "Rec_709" || name == "REC709") return ColorSpace::REC709;
	if (name == "Rec_2020" || name == "REC2020") return ColorSpace::REC2020;
	return std::nullopt;
}

static void MigrateFormatValue(std::vector<uint8_t>& data)
{
	if (data.empty())
		return;
	auto colorSpace = ReadLegacyFormat(data);
	if (!colorSpace)
	{
		nosEngine.LogW("ChannelViewer: unreadable legacy Format value, using REC709");
		colorSpace = ColorSpace::REC709;
	}
	auto value = static_cast<uint32_t>(*colorSpace);
	auto* bytes = reinterpret_cast<const uint8_t*>(&value);
	data.assign(bytes, bytes + sizeof(value));
}

static nosResult MigrateNode(nosFbNodePtr nodePtr, nosBuffer* outBuffer)
{
	fb::TNode tNode;
	nodePtr->UnPackTo(&tNode);
	bool migrated = false;
	for (auto& pin : tNode.pins)
	{
		if (!pin || pin->name != "Format")
			continue;
		if (pin->type_name != "nos.utilities.ChannelViewerFormats" &&
		    pin->type_name != "nos.fb.ChannelViewerFormats")
			continue;
		pin->type_name = "nos.mediaio.ColorSpace";
		MigrateFormatValue(pin->data);
		MigrateFormatValue(pin->def);
		migrated = true;
	}
	if (!migrated)
		return NOS_RESULT_SUCCESS;
	*outBuffer = EngineBuffer::CopyFrom(tNode).Release();
	return NOS_RESULT_SUCCESS;
}

static nosResult ExecuteNode(void* ctx, nosNodeExecuteParams* pins)
{
	auto values = GetPinValues(pins);
	const nosResourceShareInfo input = vkss::DeserializeTextureInfo(values[NSN_Input]);
	const nosResourceShareInfo output = vkss::DeserializeTextureInfo(values[NSN_Output]);

	auto channel = *(uint32_t*)values[NSN_Channel];
	auto colorSpace = *(ColorSpace*)values[NSN_Format];

	glm::vec4 val{};
	val[channel & 3] = 1;

	auto const [red, blue] = LumaCoeffs(colorSpace);
	glm::vec4 multipliers(float(red), float(1.0 - red - blue), float(blue), channel > 3);
	std::vector bindings = {
		vkss::ShaderBinding(NSN_Input, input),
		vkss::ShaderBinding(NSN_Channel, val), 
		vkss::ShaderBinding(NSN_Format, multipliers)
	};

	nosRunPassParams pass = {
		.Key = NSN_Channel_Viewer_Pass,
		.Bindings = bindings.data(),
		.BindingCount = (uint32_t)bindings.size(),
		.Output = output,
		.Wireframe = false,
	};
	auto cmd = vkss::BeginCmd(NOS_NAME("ChannelViewer"), pins->NodeId);
	nosVulkan->RunPass(cmd, &pass);
	nosVulkan->End(cmd, nullptr);
	return NOS_RESULT_SUCCESS;
}

nosResult RegisterChannelViewer(nosNodeFunctions* out)
{
	out->ClassName = NSN_Nos_MediaIO_ChannelViewer;
	out->ExecuteNode = ExecuteNode;
	out->MigrateNode = MigrateNode;

	fs::path root = nosEngine.Module->RootFolderPath;
	auto chViewerPath = (root / "Shaders" / "ChannelViewer.frag").generic_string();

	nosShaderInfo shader = {
		.ShaderName = NSN_Channel_Viewer_Shader, .Source = {.Stage = NOS_SHADER_STAGE_FRAG, .GLSLPath = chViewerPath.c_str()},
		.AssociatedNodeClassName = NSN_Nos_MediaIO_ChannelViewer
	};
	auto ret = nosVulkan->RegisterShaders(1, &shader);
	if (NOS_RESULT_SUCCESS != ret)
		return ret;

	nosPassInfo pass = {
		.Key = NSN_Channel_Viewer_Pass,
		.Shader = NSN_Channel_Viewer_Shader,
		.MultiSample = 1
	};
	return nosVulkan->RegisterPasses(1, &pass);
}
} 

