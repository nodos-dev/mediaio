#include <Nodos/Plugin.hpp>
#include <nosSysVulkan/Helpers.hpp>
#include "nosMediaio/SensorInfo_generated.h"

namespace nos::mediaio
{

struct DebayerContext : NodeContext
{
	using NodeContext::NodeContext;

	nosResult ExecuteNode(NodeExecuteParams const& params) override
	{
		auto out   = params.GetPinObject<sys::vulkan::Texture>(NOS_NAME("Output"));
		auto outResInfo = sys::vulkan::GetResourceInfo(out);
		if (!outResInfo || outResInfo->Width != 3840 || outResInfo->Height != 2160)
		{
			outResInfo = {
					.Width = 3840,
					.Height = 2160,
					.Format = NOS_FORMAT_R8G8B8A8_UNORM,
					.Usage = nosImageUsage(NOS_IMAGE_USAGE_SAMPLED | NOS_IMAGE_USAGE_RENDER_TARGET | NOS_IMAGE_USAGE_STORAGE),
			};
			out = sys::vulkan::CreateTexture(*outResInfo, "Debayer Output Texture");
			outResInfo = sys::vulkan::GetResourceInfo(out);
			if (!out || !outResInfo)
				return NOS_RESULT_FAILED;
			SetPinObject(NOS_NAME("Output"), out);
		}

		auto left = params.GetPinObject<nos::sys::vulkan::Buffer>(NOS_NAME("SourceLeft"));
		auto right = params.GetPinObject<nos::sys::vulkan::Buffer>(NOS_NAME("SourceRight"));
		uint32_t bitwidth  = 8u + 2u * (*params.GetPinValue<uint32_t>(NOS_NAME("BitWidth")));
		uint32_t wb  = *params.GetPinValue<uint32_t>(NOS_NAME("WhiteBalance"));

		auto sens  = *params.GetPinValue<nos::mediaio::ISOSensitivity>(NOS_NAME("ISOSensitivity"));

        // Determine dynamic range scaling factor based on ISO sensitivity setting.
        // Lower effective ISO values yield higher dynamic range (smaller drangeScale).
        // Higher effective ISO values reduce dynamic range (larger drangeScale).
        // The scale values appear to correspond to normalized exposure multipliers or
        // inverse signal gain factors derived from sensor calibration data.
		float drangeScale = 1.0f;
		switch(sens)
		{
		case nos::mediaio::ISOSensitivity::IS0400_SENS200: 
		case nos::mediaio::ISOSensitivity::ISO2500_SENS1250: 
			drangeScale = 0.088388; break;
		case nos::mediaio::ISOSensitivity::ISO400_SENS250:
		case nos::mediaio::ISOSensitivity::ISO2500_SENS1600:
			drangeScale = 0.111878; break;
		case nos::mediaio::ISOSensitivity::ISO400_SENS320:
		case nos::mediaio::ISOSensitivity::ISO2500_SENS2000:
			drangeScale = 0.140632; break;
		case nos::mediaio::ISOSensitivity::ISO400_SENS400:
		case nos::mediaio::ISOSensitivity::ISO2500_SENS2500:
			drangeScale = 0.176777; break;
		case nos::mediaio::ISOSensitivity::ISO800_SENS500:
		case nos::mediaio::ISOSensitivity::ISO5000_SENS3200:
			drangeScale = 0.223756; break;
		case nos::mediaio::ISOSensitivity::ISO800_SENS640:
		case nos::mediaio::ISOSensitivity::ISO5000_SENS4000:
			drangeScale = 0.281265; break;
		case nos::mediaio::ISOSensitivity::ISO800_SENS800:
		case nos::mediaio::ISOSensitivity::ISO5000_SENS5000:
			drangeScale = 0.353553; break;
		}	

		std::vector<nosShaderBinding> bindings = {
			sys::vulkan::ShaderBufferBinding(NOS_NAME("SourceLeft"), left),
			sys::vulkan::ShaderBufferBinding(NOS_NAME("SourceRight"), right),
			sys::vulkan::ShaderTextureBindingFromPin(params[NOS_NAME("Output")].Id, NOS_NAME("Output"), out),
			sys::vulkan::ShaderDataBinding(NOS_NAME("BitWidth"), bitwidth),
			sys::vulkan::ShaderDataBinding(NOS_NAME("WhiteBalance"), wb),
			sys::vulkan::ShaderDataBinding(NOS_NAME("ISOSensitivity"), drangeScale),
		};

		nosRunComputePassParams debayerPass = {
			.Key = NOS_NAME("DEBAYER_PASS"),
			.Bindings = bindings.data(),
			.BindingCount = (u32)bindings.size(),
            // WorkGroupSize = (8,8), NumWorkGroups(DispatchSize) = (240,135)
            // Each thread processes 2x2 pixels 
            // (8,8) * (240,135) * (2,2) = (3840,2160) output
			.DispatchSize = {240, 135},
		};
        
		auto cmd = sys::vulkan::BeginCmd(NOS_NAME("Debayer"), NodeId);
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
