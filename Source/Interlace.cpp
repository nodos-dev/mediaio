// Copyright MediaZ Teknoloji A.S. All Rights Reserved.
#include <Nodos/Plugin.hpp>

#include <nosSysVulkan/Helpers.hpp>

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

	nosResult OnCreate(nosFbNodePtr node) override
	{
		SetNodeStatusMessage("Internal - not intended for use.", fb::NodeStatusMessageType::WARNING);
		return NOS_RESULT_SUCCESS;
	}

	nosResult CopyFrom(nosCopyFromInfo* copyInfo) override
	{
		nosVulkan->SetResourceFieldType(*copyInfo->PinObjectHandle, Field);
		Field = sys::vulkan::FlippedField(Field);
		return NOS_RESULT_SUCCESS;
	}

	void OnPathStart() override { Field = NOS_TEXTURE_FIELD_TYPE_EVEN; }

	nosResult ExecuteNode(NodeExecuteParams const& params) override
	{
		nosRunPassParams interlacePass = {};
		interlacePass.Key = NSN_MediaIO_Interlace_Pass;
		uint32_t isOdd = Field - 1;
		std::vector bindings = {
			sys::vulkan::ShaderTextureBindingFromPin(params[NSN_Input].Id, NSN_Input, params.GetPinObject(NSN_Input)),
			sys::vulkan::ShaderDataBinding(NSN_ShouldOutputOdd, isOdd),
		};
		interlacePass.Bindings = bindings.data();
		interlacePass.BindingCount = bindings.size();
		interlacePass.Output = params.GetPinObject(NSN_Output);
		nosCmd cmd;
		nosCmdBeginParams begin{
			.Name = NOS_NAME("Interlace Pass"), .AssociatedNodeId = params.NodeId, .OutCmdHandle = &cmd};
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

	void OnPathStart() override { Field = NOS_TEXTURE_FIELD_TYPE_EVEN; }

	nosResult ExecuteNode(NodeExecuteParams const& params) override
	{
		bool isInterlaced = *params.GetPinValue<bool>(NOS_NAME("IsInterlaced"));
		if (!isInterlaced)
		{
			Field = NOS_TEXTURE_FIELD_TYPE_PROGRESSIVE;
		}
		else
		{
			Field = sys::vulkan::FlippedField(Field);
		}
		SetPinValue(NOS_NAME("FieldType"), (sys::vulkan::FieldType)Field);
		return NOS_RESULT_SUCCESS;
	}
};

struct DeinterlaceNode : NodeContext
{
	nosResult OnCreate(nosFbNodePtr node) override
	{
		SetNodeStatusMessage("Deprecated: low-quality single-field deinterlace.\nUse YADIF instead.",
							 fb::NodeStatusMessageType::WARNING);
		return NOS_RESULT_SUCCESS;
	}

	nosResult CopyFrom(nosCopyFromInfo* copyInfo) override
	{
		nosVulkan->SetResourceFieldType(*copyInfo->PinObjectHandle, NOS_TEXTURE_FIELD_TYPE_PROGRESSIVE);
		return NOS_RESULT_SUCCESS;
	}

	nosResult ExecuteNode(NodeExecuteParams const& params) override
	{
		auto inputTex = params.GetPinObject<sys::vulkan::Texture>(NSN_Input);
		auto outputTex = params.GetPinObject<sys::vulkan::Texture>(NSN_Output);
		nosRunPassParams deinterlacePass = {};
		deinterlacePass.Key = NSN_MediaIO_Deinterlace_Pass;
		auto inTexInfo = sys::vulkan::GetResourceInfo(inputTex);
		nosTextureFieldType field = sys::vulkan::GetResourceFieldType(inputTex);
		bool isInterlaced = sys::vulkan::IsTextureFieldTypeInterlaced(field);
		if (!isInterlaced)
		{
			nosEngine.LogW("Deinterlace Node: Input is not interlaced!");
			return NOS_RESULT_FAILED;
		}
		uint32_t isOdd = field - 1;
		std::vector bindings = {sys::vulkan::ShaderTextureBindingFromPin(params[NSN_Input].Id, NSN_Input, inputTex),
								sys::vulkan::ShaderDataBinding(NSN_IsOdd, isOdd)};
		deinterlacePass.Bindings = bindings.data();
		deinterlacePass.BindingCount = bindings.size();
		deinterlacePass.Output = outputTex;
		deinterlacePass.DoNotClear = true;
		nosCmd cmd;
		nosCmdBeginParams begin{
			.Name = NOS_NAME("Deinterlace Pass"), .AssociatedNodeId = params.NodeId, .OutCmdHandle = &cmd};
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
							.Source = {.Stage = NOS_SHADER_STAGE_FRAG, .GLSLPath = interlacePath.c_str()},
							.AssociatedNodeClassName = NSN_ClassName_MediaIO_Interlace};
	auto ret = nosVulkan->RegisterShaders(1, &shader);
	if (NOS_RESULT_SUCCESS != ret)
		return ret;
	nosPassInfo pass = {
		.Key = NSN_MediaIO_Interlace_Pass,
		.Shader = NSN_MediaIO_Interlace_Fragment_Shader,
		.MultiSample = 1,
	};
	return nosVulkan->RegisterPasses(1, &pass);
}

nosResult RegisterDeinterlace(nosNodeFunctions* nodeFunctions)
{
	NOS_BIND_NODE_CLASS(NSN_ClassName_MediaIO_Deinterlace, DeinterlaceNode, nodeFunctions);

	fs::path root = nosEngine.Plugin->RootFolderPath;
	auto deinterlacePath = (root / "Shaders" / "Deinterlace.frag").generic_string();
	nosShaderInfo shader = {.ShaderName = NSN_MediaIO_Deinterlace_Fragment_Shader,
							.Source = {.Stage = NOS_SHADER_STAGE_FRAG, .GLSLPath = deinterlacePath.c_str()},
							.AssociatedNodeClassName = NSN_ClassName_MediaIO_Deinterlace};
	auto ret = nosVulkan->RegisterShaders(1, &shader);
	if (NOS_RESULT_SUCCESS != ret)
		return ret;
	nosPassInfo pass = {
		.Key = NSN_MediaIO_Deinterlace_Pass,
		.Shader = NSN_MediaIO_Deinterlace_Fragment_Shader,
		.MultiSample = 1,
	};
	return nosVulkan->RegisterPasses(1, &pass);
}

nosResult RegisterFieldJuggler(nosNodeFunctions* nodeFunctions)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("FieldJuggler"), FieldJugglerNode, nodeFunctions);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::mediaio
