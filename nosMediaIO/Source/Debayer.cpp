#include <Nodos/PluginHelpers.hpp>
#include <nosVulkanSubsystem/Helpers.hpp>
#include "Debayer_generated.h"

namespace nos::mediaio
{

struct DebayerContext : NodeContext
{
	using NodeContext::NodeContext;

	nosResult ExecuteNode(nosNodeExecuteParams* inParams) override
	{
		auto params = nos::NodeExecuteParams(inParams);
		auto left  = nos::vkss::ConvertToResourceInfo(*params.GetPinData<nos::sys::vulkan::Buffer>(nos::Name("SourceLeft")));
		auto right = nos::vkss::ConvertToResourceInfo(*params.GetPinData<nos::sys::vulkan::Buffer>(nos::Name("SourceRight")));
		auto out   = nos::vkss::DeserializeTextureInfo(params[nos::Name("Output")].Data->Data);
		uint32_t bitwidth  = 8u + 2u**(uint32_t*)(params[nos::Name("BitWidth")].Data->Data);
		uint32_t wb  = *(uint32_t*)(params[nos::Name("WhiteBalance")].Data->Data);

		auto sens  = *(nos::mediaio::ISOSensitivity*)(params[nos::Name("ISOSensitivity")].Data->Data);
		float drange_scale = 1.0f;
		switch(sens)
		{
		case nos::mediaio::ISOSensitivity::IS0400_SENS200: 
		case nos::mediaio::ISOSensitivity::ISO2500_SENS1250: 
			drange_scale = 0.088388; break;
		case nos::mediaio::ISOSensitivity::ISO400_SENS250:
		case nos::mediaio::ISOSensitivity::ISO2500_SENS1600:
			drange_scale = 0.111878; break;
		case nos::mediaio::ISOSensitivity::ISO400_SENS320:
		case nos::mediaio::ISOSensitivity::ISO2500_SENS2000:
			drange_scale = 0.140632; break;
		case nos::mediaio::ISOSensitivity::ISO400_SENS400:
		case nos::mediaio::ISOSensitivity::ISO2500_SENS2500:
			drange_scale = 0.176777; break;
		case nos::mediaio::ISOSensitivity::ISO800_SENS500:
		case nos::mediaio::ISOSensitivity::ISO5000_SENS3200:
			drange_scale = 0.223756; break;
		case nos::mediaio::ISOSensitivity::ISO800_SENS640:
		case nos::mediaio::ISOSensitivity::ISO5000_SENS4000:
			drange_scale = 0.281265; break;
		case nos::mediaio::ISOSensitivity::ISO800_SENS800:
		case nos::mediaio::ISOSensitivity::ISO5000_SENS5000:
			drange_scale = 0.353553; break;
		}	

		std::vector<nosShaderBinding> bindings = {
			nos::vkss::ShaderBinding(nos::Name("SourceLeft"), left),
			nos::vkss::ShaderBinding(nos::Name("SourceRight"), right),
			nos::vkss::ShaderBinding(nos::Name("Output"), out),
			nos::vkss::ShaderBinding(nos::Name("BitWidth"), bitwidth),
			nos::vkss::ShaderBinding(nos::Name("WhiteBalance"), wb),
			nos::vkss::ShaderBinding(nos::Name("ISOSensitivity"), drange_scale),
		};

		nosRunComputePassParams debayerPass = {
			.Key = nos::Name("DEBAYER_PASS"),
			.Bindings = bindings.data(),
			.BindingCount = (u32)bindings.size(),
			.DispatchSize = {240, 135},
		};
		
		if(out.Info.Texture.Width != 3840 || out.Info.Texture.Height != 2160)
		{
			nosResourceShareInfo tex{.Info = {
				.Type = NOS_RESOURCE_TYPE_TEXTURE,
				.Texture = {
					.Width = 3840,
					.Height = 2160,
					.Format = NOS_FORMAT_R8G8B8A8_UNORM,
					.Usage = nosImageUsage(NOS_IMAGE_USAGE_SAMPLED | NOS_IMAGE_USAGE_RENDER_TARGET | NOS_IMAGE_USAGE_STORAGE),
				}
			}};
			nosEngine.SetPinValueByName(NodeId, NOS_NAME("Output"), nos::Buffer::From(nos::vkss::ConvertTextureInfo(tex)));
			return NOS_RESULT_SUCCESS;
		}

		auto cmd = nos::vkss::BeginCmd(NOS_NAME("Debayer"), NodeId);
		nosVulkan->RunComputePass(cmd, &debayerPass);
		nosVulkan->End(cmd, 0);
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterDebayer(nosNodeFunctions* nodeFunctions)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("nos.mediaio.Debayer"), DebayerContext, nodeFunctions);
    return NOS_RESULT_SUCCESS;
}

}
