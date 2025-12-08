#include <Nodos/Plugin.hpp>

#include "Names.h"

#include <nosVulkanSubsystem/Helpers.hpp>

namespace nos::mediaio
{

struct GetFieldType : NodeContext
{
	using NodeContext::NodeContext;

	std::optional<nos::TypeInfo> TypeInfo;

	nosResult OnCreate(nosFbNodePtr node) override
	{
		for (auto pin : *node->pins())
		{
			if (pin->name()->string_view() == NSN_Input)
			{
				if (pin->type_name()->string_view() != NSN_Generic)
				{
					TypeInfo = nos::TypeInfo(nos::Name(pin->type_name()->string_view()));
				}
			}
		}
		return NOS_RESULT_SUCCESS;
	}

	nosResult OnResolvePinDataTypes(nosResolvePinDataTypesParams* params) override
	{
		if (!(params->IncomingTypeName == NOS_NAME(nos::sys::vulkan::Texture::GetFullyQualifiedName()) ||
			  params->IncomingTypeName == NOS_NAME(nos::sys::vulkan::Buffer::GetFullyQualifiedName())))
		{
			strncpy(params->OutErrorMessage, "GetFieldType only supports Vulkan Texture or Buffer types", 58);
			return NOS_RESULT_FAILED;
		}

		TypeInfo = nos::TypeInfo(params->IncomingTypeName);

		for (size_t i = 0; i < params->PinCount; i++)
		{
			auto& pinInfo = params->Pins[i];
			std::string pinName = nosEngine.GetString(pinInfo.Name);
			if (pinName == "Input")
				pinInfo.OutResolvedTypeName = TypeInfo->TypeName;
		}

		return NOS_RESULT_SUCCESS;
	}

	nosResult ExecuteNode(NodeExecuteParams const& params) override
	{
		if (!TypeInfo || TypeInfo->TypeName == NSN_Generic)
			return NOS_RESULT_FAILED;
		auto inputObj = params.GetPinObject(NOS_NAME("Input"));
		if (!inputObj)
			return NOS_RESULT_FAILED;
		auto fieldType = sys::vulkan::GetResourceFieldType(inputObj);
		SetPinValue(NOS_NAME("FieldType"), fieldType);
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterGetFieldType(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("GetFieldType"), GetFieldType, fn);
	return NOS_RESULT_SUCCESS;
}

}