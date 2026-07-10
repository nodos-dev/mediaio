// Copyright MediaZ Teknoloji A.S. All Rights Reserved.
#include <Nodos/PluginHelpers.hpp>

#include <nosVulkanSubsystem/Helpers.hpp>
#include <glm/glm.hpp>
#include "Names.h"

NOS_REGISTER_NAME(Channel);
NOS_REGISTER_NAME(Format);
NOS_REGISTER_NAME(Channel_Viewer_Pass);
NOS_REGISTER_NAME(Channel_Viewer_Shader);
NOS_REGISTER_NAME_SPACED(Nos_MediaIO_ChannelViewer, "nos.mediaio.ChannelViewer")

namespace nos::mediaio
{
static nosResult MigrateNode(nosFbNodePtr nodePtr, nosBuffer* outBuffer)
{
	fb::TNode tNode;
	nodePtr->UnPackTo(&tNode);
	bool migrated = false;
	for (auto& pin : tNode.pins)
	{
		if (!pin || pin->name != "Format")
			continue;
		bool legacyType = pin->type_name == "nos.utilities.ChannelViewerFormats" ||
		                  pin->type_name == "nos.fb.ChannelViewerFormats";
		const char* newValue = nullptr;
		if (!pin->data.empty())
		{
			std::string_view oldValue(reinterpret_cast<const char*>(pin->data.data()), pin->data.size() - 1);
			if (oldValue == "Rec_601") newValue = "REC601";
			else if (oldValue == "Rec_709") newValue = "REC709";
			else if (oldValue == "Rec_2020") newValue = "REC2020";
		}
		if (!legacyType && !newValue)
			continue;
		pin->type_name = "nos.mediaio.ColorSpace";
		if (newValue)
		{
			std::string s = newValue;
			pin->data = std::vector<uint8_t>(s.c_str(), s.c_str() + s.size() + 1);
		}
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
	auto format = *(uint32_t*)values[NSN_Format];

	glm::vec4 val{};
	val[channel & 3] = 1;

	// Indexed by nos.mediaio.ColorSpace: REC709=0, REC601=1, REC2020=2
	constexpr glm::vec3 coeffs[3] = {{.2126f, .7152f, .0722f}, {.299f, .587f, .114f}, {.2627f, .678f, .0593f}};

	glm::vec4 multipliers = glm::vec4(coeffs[format], channel > 3);
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

