// Copyright MediaZ Teknoloji A.S. All Rights Reserved.
#include <Nodos/PluginHelpers.hpp>

#include <nosVulkanSubsystem/Helpers.hpp>

#include "Names.h"

NOS_REGISTER_NAME(ShouldOutputOdd);
NOS_REGISTER_NAME(IsOdd);
NOS_REGISTER_NAME_SPACED(ClassName_MediaIO_Interlace, "nos.mediaio.Interlace")
NOS_REGISTER_NAME_SPACED(ClassName_MediaIO_Deinterlace, "nos.mediaio.Deinterlace")

NOS_REGISTER_NAME(MediaIO_Interlace_Fragment_Shader);
NOS_REGISTER_NAME(MediaIO_Interlace_Pass);

NOS_REGISTER_NAME(MediaIO_Deinterlace_Fragment_Shader);
NOS_REGISTER_NAME(MediaIO_Deinterlace_Pass);

namespace nos::mediaio
{
struct InterlaceNode : NodeContext
{
	nosTextureFieldType Field;

	nosResult CopyFrom(nosCopyInfo* copyInfo) override
	{
		vkss::SetFieldType(copyInfo->ID, *copyInfo->PinData, Field);
		Field = vkss::FlippedField(Field);
		return NOS_RESULT_SUCCESS;
	}

	void OnPathStart() override
	{
		Field = NOS_TEXTURE_FIELD_TYPE_EVEN;
	}

	virtual nosResult ExecuteNode(nosNodeExecuteParams* params)
	{
		auto pinIds = GetPinIds(params);
		auto pinValues = GetPinValues(params);
		auto inputTextureInfo = vkss::DeserializeTextureInfo(pinValues[NSN_Input]);
		auto outputTextureInfo = vkss::DeserializeTextureInfo(pinValues[NSN_Output]);
		nosRunPassParams interlacePass = {};
		interlacePass.Key = NSN_MediaIO_Interlace_Pass;
		uint32_t isOdd = Field - 1;
		std::vector bindings = {
			vkss::ShaderBinding(NSN_Input, inputTextureInfo),
			vkss::ShaderBinding(NSN_ShouldOutputOdd, isOdd),
		};
		interlacePass.Bindings = bindings.data();
		interlacePass.BindingCount = bindings.size();
		interlacePass.Output = outputTextureInfo;
		nosCmd cmd;
		nosCmdBeginParams begin{ .Name = NOS_NAME("Interlace Pass"), .AssociatedNodeId = params->NodeId, .OutCmdHandle = &cmd };
		nosVulkan->Begin(&begin);
		nosVulkan->RunPass(cmd, &interlacePass);
		nosVulkan->End(cmd, nullptr);
		return NOS_RESULT_SUCCESS;
	}

	static nosResult GetFunctions(size_t* count, nosName* names, nosPfnNodeFunctionExecute* fns)
	{
		*count = 0;
		if (!names || !fns)
			return NOS_RESULT_SUCCESS;
		return NOS_RESULT_SUCCESS;
	}
};

struct FieldJugglerNode : NodeContext
{
	nosTextureFieldType Field;

	void OnPathStart() override
	{
		Field = NOS_TEXTURE_FIELD_TYPE_EVEN;
	}

	virtual nosResult ExecuteNode(nosNodeExecuteParams* params)
	{
		auto values = GetPinValues(params);
		bool isInterlaced = *GetPinValue<bool>(values, NOS_NAME("IsInterlaced"));
		if (!isInterlaced)
		{
			Field = NOS_TEXTURE_FIELD_TYPE_PROGRESSIVE;
		}
		else
		{
			Field = vkss::FlippedField(Field);
		}
		SetPinValue(NOS_NAME("FieldType"), nos::Buffer::From((sys::vulkan::FieldType)Field));
		return NOS_RESULT_SUCCESS;
	}
};

struct DeinterlaceNode : NodeContext
{
	nosResult CopyFrom(nosCopyInfo* copyInfo) override
	{
		vkss::SetFieldType(copyInfo->ID, *copyInfo->PinData, NOS_TEXTURE_FIELD_TYPE_PROGRESSIVE);
		return NOS_RESULT_SUCCESS;
	}

	virtual nosResult ExecuteNode(nosNodeExecuteParams* params)
	{
		auto pinValues = GetPinValues(params);
		auto inputTextureInfo = vkss::DeserializeTextureInfo(pinValues[NSN_Input]);
		auto outputTextureInfo = vkss::DeserializeTextureInfo(pinValues[NSN_Output]);
		nosRunPassParams deinterlacePass = {};
		deinterlacePass.Key = NSN_MediaIO_Deinterlace_Pass;
		auto field = inputTextureInfo.Info.Texture.FieldType;
		bool isInterlaced = vkss::IsTextureFieldTypeInterlaced(field);
		if (!isInterlaced)
		{
			nosEngine.LogW("Deinterlace Node: Input is not interlaced!");
			return NOS_RESULT_FAILED;
		}
		uint32_t isOdd = field - 1;
		std::vector bindings = {
			vkss::ShaderBinding(NSN_Input, inputTextureInfo),
			vkss::ShaderBinding(NSN_IsOdd, isOdd)
		};
		deinterlacePass.Bindings = bindings.data();
		deinterlacePass.BindingCount = bindings.size();
		deinterlacePass.Output = outputTextureInfo;
		deinterlacePass.DoNotClear = true;
		nosCmd cmd;
		nosCmdBeginParams begin {.Name = NOS_NAME("Deinterlace Pass"), .AssociatedNodeId = params->NodeId, .OutCmdHandle = &cmd};
		nosVulkan->Begin(&begin);
		nosVulkan->RunPass(cmd, &deinterlacePass);
		nosVulkan->End(cmd, nullptr);
		return NOS_RESULT_SUCCESS;
	}

	static nosResult GetFunctions(size_t* count, nosName* names, nosPfnNodeFunctionExecute* fns)
	{
		*count = 0;
		if (!names || !fns)
			return NOS_RESULT_SUCCESS;
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterInterlace(nosNodeFunctions* nodeFunctions)
{
	NOS_BIND_NODE_CLASS(NSN_ClassName_MediaIO_Interlace, InterlaceNode, nodeFunctions);
	fs::path root = nosEngine.Plugin->RootFolderPath;
	auto interlacePath = (root / "Shaders" / "Interlace.frag").generic_string();
	nosShaderInfo shader = {.ShaderName = NSN_MediaIO_Interlace_Fragment_Shader,
	                        .Source = {.Stage = NOS_SHADER_STAGE_FRAG, .GLSLPath = interlacePath.c_str()}, .AssociatedNodeClassName = NSN_ClassName_MediaIO_Interlace};
	auto ret = nosVulkan->RegisterShaders(1, &shader);
	if (NOS_RESULT_SUCCESS != ret)
		return ret;
	nosPassInfo pass = {.Key = NSN_MediaIO_Interlace_Pass,
	                    .Shader = NSN_MediaIO_Interlace_Fragment_Shader,
	                    .MultiSample = 1,};
	return nosVulkan->RegisterPasses(1, &pass);
}

nosResult RegisterDeinterlace(nosNodeFunctions* nodeFunctions)
{
	NOS_BIND_NODE_CLASS(NSN_ClassName_MediaIO_Deinterlace, DeinterlaceNode, nodeFunctions);

	fs::path root = nosEngine.Plugin->RootFolderPath;
	auto deinterlacePath = (root / "Shaders" / "Deinterlace.frag").generic_string();
	nosShaderInfo shader = {.ShaderName = NSN_MediaIO_Deinterlace_Fragment_Shader,
							.Source = {.Stage = NOS_SHADER_STAGE_FRAG, .GLSLPath = deinterlacePath.c_str()}, .AssociatedNodeClassName = NSN_ClassName_MediaIO_Deinterlace};
	auto ret = nosVulkan->RegisterShaders(1, &shader);
	if (NOS_RESULT_SUCCESS != ret)
		return ret;
	nosPassInfo pass = {
		.Key = NSN_MediaIO_Deinterlace_Pass,
		.Shader = NSN_MediaIO_Deinterlace_Fragment_Shader,
		.MultiSample = 1,};
	return nosVulkan->RegisterPasses(1, &pass);
}

nosResult RegisterFieldJuggler(nosNodeFunctions* nodeFunctions)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("FieldJuggler"), FieldJugglerNode, nodeFunctions);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::MediaIO
