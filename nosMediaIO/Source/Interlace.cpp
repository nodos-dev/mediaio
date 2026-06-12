// Copyright MediaZ Teknoloji A.S. All Rights Reserved.
#include <Nodos/PluginHelpers.hpp>

#include <nosVulkanSubsystem/Helpers.hpp>
#include <nosVulkanSubsystem/Types_generated.h>

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

	InterlaceNode(nosFbNodePtr node)
		: NodeContext(node)
	{
		SetNodeStatusMessage("Internal — not intended for use.",
							 fb::NodeStatusMessageType::WARNING);
	}

	~InterlaceNode()
	{
	}

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

	FieldJugglerNode(nosFbNodePtr node)
		: NodeContext(node)
	{
	}

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
	DeinterlaceNode(nosFbNodePtr node)
		: NodeContext(node)
	{
		SetNodeStatusMessage("Deprecated: low-quality single-field deinterlace.\nUse YADIF instead.",
							 fb::NodeStatusMessageType::WARNING);
	}

	~DeinterlaceNode()
	{}

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
	fs::path root = nosEngine.Module->RootFolderPath;
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

	fs::path root = nosEngine.Module->RootFolderPath;
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

struct SetInterlacedFieldTypeNode : NodeContext
{
	SetInterlacedFieldTypeNode(nosFbNodePtr node)
		: NodeContext(node)
	{
	}

	nosResult OnResolvePinDataTypes(nosResolvePinDataTypesParams* params) override
	{
		nosName incomingType = params->IncomingTypeName;
		if (incomingType != NOS_NAME(sys::vulkan::Buffer::GetFullyQualifiedName()) && 
			incomingType != NOS_NAME(sys::vulkan::Texture::GetFullyQualifiedName()))
		{
			// Reject connection if incoming type is not Buffer or Texture
			const char* errorMsg = "SetInterlacedFieldType only accepts nos.sys.vulkan.Buffer or nos.sys.vulkan.Texture types";
			strncpy(params->OutErrorMessage, errorMsg, sizeof(params->OutErrorMessage) - 1);
			params->OutErrorMessage[sizeof(params->OutErrorMessage) - 1] = '\0';
			return NOS_RESULT_FAILED;
		}
		return NOS_RESULT_SUCCESS;
	}

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		nos::NodeExecuteParams execParams(params);
		
		// Get the input and new field type
		const nosBuffer* inputPinData = execParams[NSN_Input].Data;
		auto newFieldType = *GetPinValue<sys::vulkan::FieldType>(GetPinValues(params), NOS_NAME("NewFieldType"));
		
		// Check the actual type name of the input pin to determine if it's Buffer or Texture
		auto inputPin = GetPin(NSN_Input);
		if (!inputPin)
			return NOS_RESULT_FAILED;
		
		if (inputPin->TypeName == NOS_NAME(sys::vulkan::Buffer::GetFullyQualifiedName()))
		{
			// Handle Buffer type
			auto& inputBufferDesc = *InterpretPinValue<sys::vulkan::Buffer>(inputPinData->Data);
			inputBufferDesc.mutate_field_type(newFieldType);
			nosEngine.SetPinValueByName(NodeId, NSN_Output, nos::Buffer::From(inputBufferDesc));
		}
		else if (inputPin->TypeName == NOS_NAME(sys::vulkan::Texture::GetFullyQualifiedName()))
		{
			// Handle Texture type
			auto& inputTextureDesc = *InterpretPinValue<sys::vulkan::Texture>(inputPinData->Data);
			if (!inputTextureDesc.mutate_field_type(newFieldType))
			{
				// If mutation fails, recreate the texture descriptor with the new field type
				sys::vulkan::TTexture newDesc = nos::Buffer::From(inputTextureDesc);
				newDesc.field_type = newFieldType;
				nosEngine.SetPinValueByName(NodeId, NSN_Output, nos::Buffer::From(newDesc));
			}
			else
				nosEngine.SetPinValueByName(NodeId, NSN_Output, nos::Buffer::From(inputTextureDesc));
		}
		else
		{
			// Unsupported type
			return NOS_RESULT_FAILED;
		}
		
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterSetInterlacedFieldType(nosNodeFunctions* nodeFunctions)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("SetInterlacedFieldType"), SetInterlacedFieldTypeNode, nodeFunctions);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::MediaIO
