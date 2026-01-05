// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

// Includes
#include <Nodos/Plugin.hpp>
#include <glm/glm.hpp>
#include <Builtins_generated.h>

#include <nosSysVulkan/nosVulkanSubsystem.h>

NOS_INIT()
NOS_VULKAN_INIT()

NOS_BEGIN_IMPORT_DEPS()
	NOS_VULKAN_IMPORT()
NOS_END_IMPORT_DEPS()

namespace nos::mediaio
{

enum Nodes : int
{	// CPU nodes
	Interlace,
	Deinterlace,
	RGB2YCbCr,
	YCbCr2RGB,
	YUVBufferSizeCalculator,
	GammaLUT,
	ColorSpaceMatrix,
	YUY2ToRGBA,
	TextureFormatConverter,
	NV12ToRGBA,
	RGBAToBGR24Buffer,
	FieldJuggler,
	RGBAToBGRABuffer,
	SetFieldType,
	GetFieldType,
	Debayer,
	Count
};

nosResult RegisterInterlace(nosNodeFunctions*);
nosResult RegisterDeinterlace(nosNodeFunctions*);
nosResult RegisterRGB2YCbCr(nosNodeFunctions*);
nosResult RegisterYCbCr2RGB(nosNodeFunctions*);
nosResult RegisterYUVBufferSizeCalculator(nosNodeFunctions*);
nosResult RegisterGammaLUT(nosNodeFunctions*);
nosResult RegisterColorSpaceMatrix(nosNodeFunctions*);
nosResult RegisterYUY2ToRGBA(nosNodeFunctions*);
nosResult RegisterTextureFormatConverter(nosNodeFunctions* fn);
nosResult RegisterNV12ToRGBA(nosNodeFunctions*);
nosResult RegisterRGBAToBGR24Buffer(nosNodeFunctions*);
nosResult RegisterFieldJuggler(nosNodeFunctions*);
nosResult RegisterRGBAToBGRABuffer(nosNodeFunctions*);
nosResult RegisterSetFieldType(nosNodeFunctions*);
nosResult RegisterGetFieldType(nosNodeFunctions*);
nosResult RegisterDebayer(nosNodeFunctions*);

struct MediaIOPluginFunctions : nos::PluginFunctions
{
	nosResult ExportNodeFunctions(size_t& outSize, nosNodeFunctions** outList) override
	{
		outSize = Nodes::Count;
		if (!outList)
			return NOS_RESULT_SUCCESS;

#define GEN_CASE_NODE(name)				\
	case Nodes::name: {					\
		auto ret = Register##name(node);	\
		if (NOS_RESULT_SUCCESS != ret)		\
			return ret;						\
		break;								\
	}

		for (int i = 0; i < Nodes::Count; ++i)
		{
			auto node = outList[i];
			switch ((Nodes)i) {
			default:
				break;
				GEN_CASE_NODE(Interlace)
				GEN_CASE_NODE(Deinterlace)
				GEN_CASE_NODE(RGB2YCbCr)
				GEN_CASE_NODE(YCbCr2RGB)
				GEN_CASE_NODE(YUVBufferSizeCalculator)
				GEN_CASE_NODE(GammaLUT)
				GEN_CASE_NODE(ColorSpaceMatrix)
				GEN_CASE_NODE(YUY2ToRGBA)
				GEN_CASE_NODE(TextureFormatConverter)
				GEN_CASE_NODE(NV12ToRGBA)
				GEN_CASE_NODE(RGBAToBGR24Buffer)
				GEN_CASE_NODE(FieldJuggler)
				GEN_CASE_NODE(RGBAToBGRABuffer)
				GEN_CASE_NODE(SetFieldType)
				GEN_CASE_NODE(GetFieldType)
				GEN_CASE_NODE(Debayer)
			}
		}
		return NOS_RESULT_SUCCESS;
	}
};

nosResult NOSAPI_CALL Export(uint32_t minorVersion, void** outAPI);

extern "C" NOSAPI_ATTR nosResult NOSAPI_CALL nosExportPlugin(nosPluginFunctions* outFunctions)
{
	static MediaIOPluginFunctions pluginFunctions{};
	outFunctions->Initialize = []() -> nosResult { return pluginFunctions.Initialize(); };
	outFunctions->ExportNodeFunctions = [](size_t* outSize, nosNodeFunctions** outList) -> nosResult {
		return pluginFunctions.ExportNodeFunctions(*outSize, outList);
	};
	outFunctions->OnPreUnloadPlugin = []() -> nosResult { return pluginFunctions.OnPreUnloadPlugin(); };
	outFunctions->OnRequestAPI = Export;

	outFunctions->GetRenamedNodeClasses = [](nosName* outRenamedFrom, nosName* outRenamedTo, size_t* outSize) {
		if (!outRenamedFrom)
		{
			*outSize = 3;
			return;
		}
		outRenamedFrom[0] = NOS_NAME("nos.interop.TextureFormatConverter");
		outRenamedTo[0] = NOS_NAME("nos.mediaio.TextureFormatConverter");
		outRenamedFrom[1] = NOS_NAME("zd.ndi.RGBAToBGRABuffer");
		outRenamedTo[1] = NOS_NAME("nos.mediaio.RGBAToBGRABuffer");
		outRenamedFrom[2] = NOS_NAME("nos.ndi.RGBAToBGRABuffer");
		outRenamedTo[2] = NOS_NAME("nos.mediaio.RGBAToBGRABuffer");
	};
	return NOS_RESULT_SUCCESS;
}
}
