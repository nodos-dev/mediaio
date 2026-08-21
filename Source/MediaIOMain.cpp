// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

// Includes
#include <Nodos/Plugin.hpp>
#include <glm/glm.hpp>
#include <Builtins_generated.h>
#include <vector>

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
	TextureToBuffer,
	YUY2ToRGBA,
	NV12ToRGBA,
	FieldJuggler,
	Debayer,
	ExtractTimecode,
	InjectTimecode,
	InjectHDRMetadata,
	EncodeHDRMetadata,
	TimecodeFromFrameNumber,
	TimecodeToFrameNumber,
	TimecodeToString,
	TimecodePlayer,
	SLog3ToLinear,
	LinearToSLog3,
	WriteDPX,
	PlaybackClip,
	ReadEXR,
	ReadEXRSequence,
	StbiLoad,
	WriteImage,
	LoadCubeLUT,
	ReadImage,
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
nosResult RegisterNV12ToRGBA(nosNodeFunctions*);
nosResult RegisterFieldJuggler(nosNodeFunctions*);
nosResult RegisterDebayer(nosNodeFunctions*);
nosResult RegisterTextureToBuffer(nosNodeFunctions*);
nosResult RegisterExtractTimecode(nosNodeFunctions*);
nosResult RegisterInjectTimecode(nosNodeFunctions*);
nosResult RegisterInjectHDRMetadata(nosNodeFunctions*);
nosResult RegisterEncodeHDRMetadata(nosNodeFunctions*);
nosResult RegisterTimecodeFromFrameNumber(nosNodeFunctions*);
nosResult RegisterTimecodeToFrameNumber(nosNodeFunctions*);
nosResult RegisterTimecodeToString(nosNodeFunctions*);
nosResult RegisterTimecodePlayer(nosNodeFunctions*);
nosResult RegisterSLog3ToLinear(nosNodeFunctions*);
nosResult RegisterLinearToSLog3(nosNodeFunctions*);
nosResult RegisterWriteDPX(nosNodeFunctions*);
nosResult RegisterPlaybackClip(nosNodeFunctions*);
nosResult RegisterReadEXR(nosNodeFunctions*);
nosResult RegisterReadEXRSequence(nosNodeFunctions*);

} // namespace nos::mediaio

namespace nos::mediaio
{
nosResult RegisterStbiLoad(nosNodeFunctions*);
nosResult RegisterWriteImage(nosNodeFunctions*);
nosResult RegisterLoadCubeLUT(nosNodeFunctions*);
nosResult RegisterReadImage(nosNodeFunctions*);
}

namespace nos::mediaio
{
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
				GEN_CASE_NODE(TextureToBuffer)
				GEN_CASE_NODE(YUY2ToRGBA)
				GEN_CASE_NODE(NV12ToRGBA)
				GEN_CASE_NODE(FieldJuggler)
				GEN_CASE_NODE(Debayer)
				GEN_CASE_NODE(ExtractTimecode)
				GEN_CASE_NODE(InjectTimecode)
				GEN_CASE_NODE(InjectHDRMetadata)
				GEN_CASE_NODE(EncodeHDRMetadata)
				GEN_CASE_NODE(TimecodeFromFrameNumber)
				GEN_CASE_NODE(TimecodeToFrameNumber)
				GEN_CASE_NODE(TimecodeToString)
				GEN_CASE_NODE(TimecodePlayer)
				GEN_CASE_NODE(SLog3ToLinear)
				GEN_CASE_NODE(LinearToSLog3)
				GEN_CASE_NODE(WriteDPX)
				GEN_CASE_NODE(PlaybackClip)
				GEN_CASE_NODE(ReadEXR)
				GEN_CASE_NODE(ReadEXRSequence)
				GEN_CASE_NODE(StbiLoad)
				GEN_CASE_NODE(WriteImage)
				GEN_CASE_NODE(LoadCubeLUT)
				GEN_CASE_NODE(ReadImage)
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

	outFunctions->GetRenamedTypes = [](nosName* outRenamedFrom, nosName* outRenamedTo, size_t* outSize) {
		static std::vector<std::pair<nos::Name, nos::Name>> renames = {
			{NOS_NAME("nos.utilities.TextureSyncTuple"), NOS_NAME("nos.mediaio.TextureSyncTuple")},
		};
		if (!outRenamedFrom)
		{
			*outSize = renames.size();
			return;
		}
		for (size_t i = 0; i < renames.size(); ++i)
		{
			outRenamedFrom[i] = renames[i].first;
			outRenamedTo[i] = renames[i].second;
		}
	};

	outFunctions->GetRenamedNodeClasses = [](nosName* outRenamedFrom, nosName* outRenamedTo, size_t* outSize) {
		static std::vector<std::pair<nos::Name, nos::Name>> renames = {
			{NOS_NAME("nos.utilities.StbiLoad"), NOS_NAME("nos.mediaio.StbiLoad")},
			{NOS_NAME("nos.utilities.WriteImage"), NOS_NAME("nos.mediaio.WriteImage")},
			{NOS_NAME("nos.utilities.LoadCubeLUT"), NOS_NAME("nos.mediaio.LoadCubeLUT")},
			{NOS_NAME("nos.utilities.ReadImage"), NOS_NAME("nos.mediaio.ReadImage")},
			{NOS_NAME("zd.utilities.StbiLoad"), NOS_NAME("nos.mediaio.StbiLoad")},
			{NOS_NAME("zd.utilities.WriteImage"), NOS_NAME("nos.mediaio.WriteImage")},
			{NOS_NAME("zd.utilities.LoadCubeLUT"), NOS_NAME("nos.mediaio.LoadCubeLUT")},
			{NOS_NAME("zd.utilities.ReadImage"), NOS_NAME("nos.mediaio.ReadImage")},
		};
		if (!outRenamedFrom)
		{
			*outSize = renames.size();
			return;
		}
		for (size_t i = 0; i < renames.size(); ++i)
		{
			outRenamedFrom[i] = renames[i].first;
			outRenamedTo[i] = renames[i].second;
		}
	};
	return NOS_RESULT_SUCCESS;
}
}
