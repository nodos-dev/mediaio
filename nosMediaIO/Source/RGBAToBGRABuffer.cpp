#include <Nodos/Plugin.hpp>
#include <nosVulkanSubsystem/Helpers.hpp>

namespace nos::mediaio {

struct RGBA2BGRABufferNodeContext : NodeContext
{
	nosResult ExecuteNode(NodeExecuteParams const& params) override
	{
		nosTextureInfo inputInfo =
			*vkss::GetResourceInfo(params.GetPinObject<vkss::Texture>(NOS_NAME_STATIC("Source")));
		auto optOutputInfo = vkss::GetResourceInfo(params.GetPinObject<vkss::Buffer>(NOS_NAME_STATIC("Output")));

		nosVec2u ext = {inputInfo.Width, inputInfo.Height};

		uint32_t bufSize = ext.x * ext.y * 4;
		constexpr auto outMemoryFlags = nosMemoryFlags(NOS_MEMORY_FLAGS_DEVICE_MEMORY);
		if (!optOutputInfo || optOutputInfo->Size != bufSize || optOutputInfo->MemoryFlags != outMemoryFlags)
		{
			SetPinObject(NOS_NAME_STATIC("Output"), vkss::CreateBuffer(nosBufferInfo{
					.Size = (uint32_t)bufSize,
					.Usage = nosBufferUsage(NOS_BUFFER_USAGE_TRANSFER_SRC | NOS_BUFFER_USAGE_STORAGE_BUFFER),
					.MemoryFlags = outMemoryFlags,
					.FieldType = nosTextureFieldType::NOS_TEXTURE_FIELD_TYPE_PROGRESSIVE,
				}, "BGRABuffer"));
		}
		else
			nosVulkan->SetResourceFieldType(params.GetPinObject<vkss::Buffer>(NOS_NAME_STATIC("Output")),
											nosTextureFieldType::NOS_TEXTURE_FIELD_TYPE_PROGRESSIVE);
		SetPinValue(NOS_NAME("DispatchSize"), nosVec2u(ext.x / 4, ext.y));
		return nosVulkan->ExecuteGPUNode(this, params.RawParams);
	}
};

nosResult RegisterRGBAToBGRABuffer(nosNodeFunctions* funcs)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("nos.mediaio.RGBAToBGRABuffer"), RGBA2BGRABufferNodeContext, funcs);
	return NOS_RESULT_SUCCESS;
}

}