#include <Nodos/Plugin.hpp>
#include <nosSysVulkan/Helpers.hpp>
#include <nosSysVulkan/Types_generated.h>

#include "Names.h"

namespace nos::mediaio
{
NOS_REGISTER_NAME(Input)
NOS_REGISTER_NAME(Output)
NOS_REGISTER_NAME(OutputFormat)
NOS_REGISTER_NAME(TextureFormatConverter)
NOS_REGISTER_NAME(InputTexture)
NOS_REGISTER_NAME(FloatToIntFormat)
NOS_REGISTER_NAME(FloatToIntFormat_Pass)
NOS_REGISTER_NAME(DST_TEXTURE_UINT32)
NOS_REGISTER_NAME(DST_TEXTURE_UINT16)
NOS_REGISTER_NAME(DST_TEXTURE_UINT8)
NOS_REGISTER_NAME(DST_TEXTURE_INT32)
NOS_REGISTER_NAME(DST_TEXTURE_INT16)
NOS_REGISTER_NAME(DST_TEXTURE_INT8)

std::pair<nos::Name, std::vector<uint8_t>> FloatToIntFormatShader;

struct TextureFormatConverter : nos::NodeContext
{
	// TODO: Transfer rewrite this?
	nos::ObjectRef InputTexture = {};
	nos::ObjectRef OutputTexture = {};
	nos::sys::vulkan::Format OutputFormat = {};;

	void OnPinObjectChanged(nos::Name pinName, uuid const& pinId, nosObjectId newHandle) override
	{
		if (NSN_Input == pinName)
		{
			InputTexture = ObjectRef::FromObjectId(newHandle);
			PrepareOutput();
		}
		if (NSN_OutputFormat == pinName)
		{
			auto newFormat = nos::InterpretObject<nos::sys::vulkan::Format>(newHandle);
			if (newFormat)
			{
				OutputFormat = *newFormat;
				PrepareOutput();
			}
		}
	}

	nosResult ExecuteNode(nos::NodeExecuteParams const& params) override
	{
		InputTexture = params.GetPinObject(NSN_Input);
		OutputTexture = params.GetPinObject(NSN_Output);
		auto inputResInfo = sys::vulkan::GetResourceInfo(InputTexture);
		auto outputResInfo = sys::vulkan::GetResourceInfo(OutputTexture);
		if (!inputResInfo || !outputResInfo)
			return NOS_RESULT_INVALID_ARGUMENT;
		//TODO: Also should be able to convert from INT texture to FLOAT textures
		//TODO: Also should be able to convert between signed integer and unsigned integers
		//TODO: Editor view and AJA does not expects INTEGER formats hence both the editor and ajaOut view does not show correct image.
		if (!nosVulkan->IsBlitCompatible(inputResInfo->Texture.Format, outputResInfo->Texture.Format))
		{
			struct OutputType
			{
				int outputType;
			};
			OutputType out = {};
			std::vector<nosShaderBinding> inputs;
			switch (outputResInfo->Texture.Format)
			{
			case NOS_FORMAT_R32G32B32A32_UINT: {
				out.outputType = 0;
				break;
			}
			case NOS_FORMAT_R16G16B16A16_UINT: {
				out.outputType = 1;
				break;
			}
			case NOS_FORMAT_R8G8B8A8_UINT: {
				out.outputType = 2;
				break;
			}
			case NOS_FORMAT_R32G32B32A32_SINT: {
				out.outputType = 3;
				break;
			}
			case NOS_FORMAT_R16G16B16A16_SINT: {
				out.outputType = 4;
				break;
			}
			default: {
				out.outputType = -1;
				break;
			}
			}
			if (out.outputType != -1)
			{
				inputs.emplace_back(nos::sys::vulkan::ShaderTextureBinding(NSN_InputTexture, InputTexture, NOS_TEXTURE_FILTER_NEAREST));
				inputs.emplace_back(nos::sys::vulkan::ShaderDataBinding<OutputType>(NSN_Output, out));

				//Validation layer does not like this but we sure that only true desired texture will be used in shader
				inputs.emplace_back(nos::sys::vulkan::ShaderTextureBinding(NSN_DST_TEXTURE_UINT32, OutputTexture, NOS_TEXTURE_FILTER_NEAREST));
				inputs.emplace_back(nos::sys::vulkan::ShaderTextureBinding(NSN_DST_TEXTURE_UINT16, OutputTexture, NOS_TEXTURE_FILTER_NEAREST));
				inputs.emplace_back(nos::sys::vulkan::ShaderTextureBinding(NSN_DST_TEXTURE_UINT8, OutputTexture, NOS_TEXTURE_FILTER_NEAREST));
				inputs.emplace_back(nos::sys::vulkan::ShaderTextureBinding(NSN_DST_TEXTURE_INT32, OutputTexture, NOS_TEXTURE_FILTER_NEAREST));
				inputs.emplace_back(nos::sys::vulkan::ShaderTextureBinding(NSN_DST_TEXTURE_INT16, OutputTexture, NOS_TEXTURE_FILTER_NEAREST));
				inputs.emplace_back(nos::sys::vulkan::ShaderTextureBinding(NSN_DST_TEXTURE_INT8, OutputTexture, NOS_TEXTURE_FILTER_NEAREST));

				nosCmd cmdRunPass = {};
				nosCmdBeginParams beginParams{
					.Name = NOS_NAME("Texture Format Conversion: Float to Int"),
					.AssociatedNodeId = NodeId,
					.OutCmdHandle = &cmdRunPass
				};
				nosVulkan->Begin(&beginParams);
				nosRunComputePassParams pass = {};
				nosCmdEndParams endParams = {};
				pass.Key = NSN_FloatToIntFormat_Pass;
				pass.DispatchSize = nosVec2u(inputResInfo->Texture.Width / 16,
				                             inputResInfo->Texture.Height / 16);
				pass.Bindings = inputs.data();
				pass.BindingCount = inputs.size();
				pass.Benchmark = 0;
				nosVulkan->RunComputePass(cmdRunPass, &pass);
				nosVulkan->End(cmdRunPass, &endParams);
			}
		}
		else
		{
			nosCmd cmd = {};
			nosCmdBeginParams beginParams = {
				.Name = NOS_NAME("Texture Format Conversion: Blit"),
				.AssociatedNodeId = NodeId,
				.OutCmdHandle = &cmd,
			};
			nosCmdEndParams endParams = {};
			nosVulkan->Begin(&beginParams);
			nosVulkan->Copy(cmd, InputTexture, OutputTexture, nullptr);
			nosVulkan->End(cmd, &endParams);
		}
		return NOS_RESULT_SUCCESS;
	}

	void PrepareOutput()
	{
		auto inputResInfo = sys::vulkan::GetResourceInfo(InputTexture);
		auto outputResInfo = sys::vulkan::GetResourceInfo(OutputTexture);
		if (outputResInfo && outputResInfo->Texture.Width == inputResInfo->Texture.Width &&
		    outputResInfo->Texture.Height == inputResInfo->Texture.Height &&
		    outputResInfo->Texture.Format == nosFormat((int)OutputFormat))
		{
			return;
		}

		OutputTexture = {};
		nosTextureInfo newTexInfo = {};

		newTexInfo.Format = nosFormat((int)OutputFormat);
		newTexInfo.Height = inputResInfo->Texture.Height;
		newTexInfo.Usage = inputResInfo->Texture.Usage;
		newTexInfo.Width = inputResInfo->Texture.Width;
		
		nosResourceInfo newInfo {
			.Type = NOS_RESOURCE_TYPE_TEXTURE,
			.Texture = newTexInfo
		};

		OutputTexture = nos::sys::vulkan::CreateResource(newInfo, "TextureFormatConverter Output Texture");
		if (OutputTexture)
			SetPinObject(NSN_Output, OutputTexture);
	}
};

nosResult RegisterTextureFormatConverter(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NSN_TextureFormatConverter, TextureFormatConverter, fn);

	std::filesystem::path root = nosEngine.Plugin->RootFolderPath;
	auto shaderPath = (root / "Shaders" / "FloatToInt.comp").generic_string();
	nosShaderInfo FloatToIntShaderInfo = {
		.ShaderName = NSN_FloatToIntFormat,
		.Source = {.Stage = NOS_SHADER_STAGE_COMP, .GLSLPath = shaderPath.c_str()},
		.AssociatedNodeClassName = NSN_TextureFormatConverter
	};
	nosResult ret = nosVulkan->RegisterShaders(1, &FloatToIntShaderInfo);
	if (NOS_RESULT_SUCCESS != ret)
		return ret;

	nosPassInfo pass = {.Key = NSN_FloatToIntFormat_Pass, .Shader = NSN_FloatToIntFormat, .MultiSample = 1};
	return nosVulkan->RegisterPasses(1, &pass);
	return NOS_RESULT_SUCCESS;
}

}