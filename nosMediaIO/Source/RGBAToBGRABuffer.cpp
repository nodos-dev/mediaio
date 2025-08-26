#include <Nodos/Plugin.hpp>
#include <nosVulkanSubsystem/Helpers.hpp>

namespace nos::mediaio {

struct RGBA2BGRABufferNodeContext : NodeContext
{
	RGBA2BGRABufferNodeContext()
	{
	}

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		nos::NodeExecuteParams execParams(params);
		const nosBuffer* inputPinData = execParams[NOS_NAME_STATIC("Source")].Data;
		const nosBuffer* outputPinData = execParams[NOS_NAME_STATIC("Output")].Data;
		auto input = nos::vkss::DeserializeTextureInfo(inputPinData->Data);
		auto& output = *InterpretPinValue<nos::sys::vulkan::Buffer>(outputPinData->Data);
		output.mutate_field_type(nos::sys::vulkan::FieldType::PROGRESSIVE);

		nosVec2u ext = { input.Info.Texture.Width, input.Info.Texture.Height };

		uint32_t bufSize = ext.x * ext.y * 4;
		constexpr auto outMemoryFlags = nosMemoryFlags(NOS_MEMORY_FLAGS_DEVICE_MEMORY);
		if (output.size_in_bytes() != bufSize || output.memory_flags() != (nos::sys::vulkan::MemoryFlags)(outMemoryFlags))
		{
			nosResourceShareInfo bufInfo = {
				.Info = {
					.Type = NOS_RESOURCE_TYPE_BUFFER,
					.Buffer = nosBufferInfo{
						.Size = (uint32_t)bufSize,
						.Usage = nosBufferUsage(NOS_BUFFER_USAGE_TRANSFER_SRC | NOS_BUFFER_USAGE_STORAGE_BUFFER),
						.MemoryFlags = outMemoryFlags,
						.FieldType = nosTextureFieldType::NOS_TEXTURE_FIELD_TYPE_PROGRESSIVE,
					}} };
			auto bufferDesc = nos::vkss::ConvertBufferInfo(bufInfo);
			nosEngine.SetPinValueByName(NodeId, NOS_NAME_STATIC("Output"), nos::Buffer::From(bufferDesc));
		}
		auto* dispatchSize = execParams.GetPinData<nosVec2u>(NOS_NAME_STATIC("DispatchSize"));
		*dispatchSize = { ext.x / 4, ext.y };
		return nosVulkan->ExecuteGPUNode(this, params);
	}
};

nosResult RegisterRGBAToBGRABuffer(nosNodeFunctions* funcs)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("nos.mediaio.RGBAToBGRABuffer"), RGBA2BGRABufferNodeContext, funcs);
	return NOS_RESULT_SUCCESS;
}

}