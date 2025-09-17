// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>
#include <nosVulkanSubsystem/Helpers.hpp>

#include "Conversion_generated.h"

#include <glm/glm.hpp>

namespace nos::mediaio
{

static std::set<uint32_t> const& FindDivisors(const uint32_t N)
{
	static std::mutex Mutex;
	std::unique_lock _(Mutex);
	static std::map<uint32_t, std::set<uint32_t>> Map;

	auto it = Map.find(N);
	if(it != Map.end()) 
		return it->second;

	uint32_t p2 = 0, p3 = 0, p5 = 0;
	std::set<uint32_t> D;

	static std::set<uint32_t> Empty;

	if (N == 0)
		return Empty;

	uint32_t n = N;
	while(0 == n % 2) n /= 2, p2++;
	while(0 == n % 3) n /= 3, p3++;
	while(0 == n % 5) n /= 5, p5++;

	for(uint32_t i = 0; i <= p2; ++i)
		for(uint32_t j = 0; j <= p3; ++j)
			for(uint32_t k = 0; k <= p5; ++k)
				D.insert(pow(2, i) * pow(3, j) * pow(5, k));

	std::set<uint32_t> const& re = (Map[N] = std::move(D));
	return re;
}

nosVec2u GetSuitableDispatchSize(nosVec2u dispatchSize, nosVec2u outSize, uint8_t bitWidth, bool interlaced)
{
	constexpr auto BestFit = [](int64_t val, int64_t res) -> uint32_t {
		if (res == 0)
			return val;
		auto d = FindDivisors(res);
		auto it = d.upper_bound(val);
		if (it == d.begin())
			return *it;
		if (it == d.end())
			return res;
		const int64_t hi = *it;
		const int64_t lo = *--it;
		return uint32_t(abs(val - lo) < abs(val - hi) ? lo : hi);
	};

	const uint32_t q = 0;//TODO: IsQuad(); ?
	float x = glm::clamp<uint32_t>(dispatchSize.x, 1, outSize.x) * (1 + q) * (.25 * bitWidth - 1);
	float y = glm::clamp<uint32_t>(dispatchSize.y, 1, outSize.y) * (1. + q) * (1 + uint8_t(interlaced));

	return nosVec2u(BestFit(x + .5, outSize.x >> (bitWidth - 5)),
					 BestFit(y + .5, outSize.y / 9));
}

nosVec2u GetYCbCrBufferResolution(nosVec2u res, YCbCrPixelFormat fmt, bool interlaced)
{
	nosVec2u yCbCrExt((fmt == YCbCrPixelFormat::V210) ? ((res.x + (48 - res.x % 48) % 48) / 3) << 1 : res.x >> 1,
					  res.y >> int(interlaced));
	return yCbCrExt;
}

struct RGB2YCbCrNodeContext : NodeContext
{
	void OnPathStart() override { FieldType = NOS_TEXTURE_FIELD_TYPE_EVEN; }

	nosTextureFieldType FieldType{};
	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		nos::NodeExecuteParams execParams(params);
		auto inputTex = execParams.GetPinObject<vkss::Texture>(NOS_NAME("Source"));
		auto outputBuf = execParams.GetPinObject<vkss::Buffer>(NOS_NAME("Output"));
		auto inputTexInfo = *vkss::GetResourceInfo(inputTex);
		auto outputBufInfo = vkss::GetResourceInfo(outputBuf);
		auto outputPinData = execParams[NOS_NAME_STATIC("Output")].Data;
		auto inputFieldType = *execParams.GetPinData<nos::sys::vulkan::FieldType>(NOS_NAME("InputFieldType"));
		auto outputFieldType = *execParams.GetPinData<nos::sys::vulkan::FieldType>(NOS_NAME("OutputFieldType"));
		auto isOutInterlaced = *execParams.GetPinData<bool>(NOS_NAME("IsOutputInterlaced"));
		auto fmt = *execParams.GetPinData<YCbCrPixelFormat>(NOS_NAME("PixelFormat"));

		// TODO: Transfer
		//*inputFieldType = (nos::sys::vulkan::FieldType)inputTexInfo.FieldType;
		//*outputFieldType = *inputFieldType;
		bool isInInterlaced = vkss::IsTextureFieldTypeInterlaced(inputTexInfo.FieldType);

		// TODO: Transfer
		// if (isOutInterlaced)
		//{
		//	if (!isInInterlaced)
		//	{
		//		*outputFieldType = (nos::sys::vulkan::FieldType)FieldType; // Deinterlace: Override with locally tracked
		//field 		FieldType = vkss::FlippedField(FieldType);
		//	}
		//	output.mutate_field_type(*outputFieldType);
		//}
		// else
		//	output.mutate_field_type(sys::vulkan::FieldType::PROGRESSIVE);

		nosVec2u ext = {inputTexInfo.Width, inputTexInfo.Height};
		nosVec2u yCbCrExt = GetYCbCrBufferResolution(ext, fmt, isOutInterlaced);

		uint32_t bufSize = yCbCrExt.x * yCbCrExt.y * 4;
		constexpr auto outMemoryFlags = NOS_MEMORY_FLAGS_DEVICE_MEMORY;
		if (!outputBufInfo || outputBufInfo->Size != bufSize || outputBufInfo->MemoryFlags != outMemoryFlags)
		{
			SetPinObject(
				NOS_NAME_STATIC("Output"),
				vkss::CreateBuffer(
					nosBufferInfo{
						.Size = (uint32_t)bufSize,
						.Usage = nosBufferUsage(NOS_BUFFER_USAGE_TRANSFER_SRC | NOS_BUFFER_USAGE_STORAGE_BUFFER),
						.MemoryFlags = outMemoryFlags,
						.FieldType = (nosTextureFieldType)inputFieldType,
					},
					"YCbCrBuffer"));
		}
		SetPinValue(NOS_NAME("DispatchSize"),
					GetSuitableDispatchSize(*execParams.GetPinData<nosVec2u>(NOS_NAME("DispatchSize")),
											yCbCrExt,
											fmt == YCbCrPixelFormat::V210 ? 10 : 8,
											isOutInterlaced));
		return nosVulkan->ExecuteGPUNode(this, params);
	}
};

nosResult RegisterRGB2YCbCr(nosNodeFunctions* funcs)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("nos.mediaio.RGB2YCbCr"), RGB2YCbCrNodeContext, funcs);
	return NOS_RESULT_SUCCESS;
}

NOS_REGISTER_NAME(Resolution);

struct YCbCr2RGBNodeContext : NodeContext
{
	nosResult OnCreate(nosFbNodePtr node) override
	{
		AddPinValueWatcher(NSN_Resolution, [this](const nos::Buffer& newVal, std::optional<nos::Buffer> oldVal) {
			auto newDispatchSize = nosVec2u(120, 120);
			nosEngine.SetPinValueByName(NodeId, NOS_NAME_STATIC("DispatchSize"), Buffer::From(newDispatchSize));
		});
		return NOS_RESULT_SUCCESS;
	}

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		nos::NodeExecuteParams execParams(params);
		auto fmt = *execParams.GetPinData<YCbCrPixelFormat>(NOS_NAME("PixelFormat"));
		auto res = *execParams.GetPinData<nos::fb::vec2u>(NOS_NAME("Resolution"));
		auto inputBuf = execParams.GetPinObject<vkss::Buffer>(NOS_NAME("Source"));
		auto inputBufInfo = vkss::GetResourceInfo(inputBuf);
		if (!inputBufInfo || !inputBuf.IsValid())
		{
			nosEngine.LogE("YCbCr2RGB Node: Input buffer is not valid!");
			return NOS_RESULT_FAILED;
		}
		auto outputTex = execParams.GetPinObject<vkss::Texture>(NOS_NAME("Output"));
		auto outputTexInfo = vkss::GetResourceInfo(outputTex);

		bool isInterlaced = inputBufInfo->FieldType == NOS_TEXTURE_FIELD_TYPE_EVEN ||
							inputBufInfo->FieldType == NOS_TEXTURE_FIELD_TYPE_ODD;
		SetPinValue(NOS_NAME("IsInterlaced"), isInterlaced);

		nosVec2u ext = {res.x(), res.y()};
		nosVec2u yCbCrExt = GetYCbCrBufferResolution(ext, fmt, isInterlaced);

		sys::vulkan::TTexture texDef;
		if (!outputTexInfo || outputTexInfo->Width != ext.x || outputTexInfo->Height != ext.y)
		{
			SetPinObject(NOS_NAME("Output"),
						 vkss::CreateTexture(
							 {
								 .Width = ext.x,
								 .Height = ext.y,
								 .Format = NOS_FORMAT_R16G16B16A16_UNORM,
								 .FieldType = inputBufInfo->FieldType,
							 },
							 "YCbCr2RGBResult"));
		}
		else
		{
			// TODO: Transfer
			// output.UnPackTo(&texDef);
			// texDef.field_type = input.field_type();
			// nosEngine.SetPinValueByName(NodeId, NOS_NAME("Output"), Buffer::From(texDef));
		}

		SetPinValue(NOS_NAME("DispatchSize"),
					GetSuitableDispatchSize(*execParams.GetPinData<nosVec2u>(NOS_NAME("DispatchSize")),
											yCbCrExt,
											fmt == YCbCrPixelFormat::V210 ? 10 : 8,
											isInterlaced));
		return nosVulkan->ExecuteGPUNode(this, params);
	}
};

nosResult RegisterYCbCr2RGB(nosNodeFunctions* funcs)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("nos.mediaio.YCbCr2RGB"), YCbCr2RGBNodeContext, funcs);
	return NOS_RESULT_SUCCESS;
}

struct YUVBufferSizeCalculator : NodeContext
{
	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		nos::NodeExecuteParams execParams(params);
		auto fmt = *execParams.GetPinData<YCbCrPixelFormat>(NOS_NAME("PixelFormat"));
		auto res = *execParams.GetPinData<nos::fb::vec2u>(NOS_NAME("Resolution"));
		auto isInterlaced = *execParams.GetPinData<bool>(NOS_NAME("IsInterlaced"));
		nosVec2u ext = { res.x(), res.y() };
		nosVec2u yCbCrExt = GetYCbCrBufferResolution(ext, fmt, isInterlaced);
		uint64_t bufSize = yCbCrExt.x * yCbCrExt.y * 4;
		SetPinValue(NOS_NAME("Output"), bufSize);
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterYUVBufferSizeCalculator(nosNodeFunctions* funcs)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("nos.mediaio.YUVBufferSizeCalculator"), YUVBufferSizeCalculator, funcs);
	return NOS_RESULT_SUCCESS;
}

struct GammaLUTNodeContext : NodeContext
{
	TypedObjectRef<vkss::Buffer> StagingBuffer;
	nosBufferInfo StagingBufInfo;
	static constexpr auto SSBO_SIZE = 10; // Can have a better name.

	nosResult OnCreate(nosFbNodePtr node)
	{
		StagingBufInfo = {.Size = (1 << (SSBO_SIZE)) * sizeof(uint16_t),
						  .Usage = nosBufferUsage(NOS_BUFFER_USAGE_TRANSFER_SRC),
						  .MemoryFlags = NOS_MEMORY_FLAGS_HOST_VISIBLE};
		StagingBuffer = vkss::CreateBuffer(StagingBufInfo,
										   "GammaLUT Staging Buffer");
		if (!StagingBuffer.IsValid())
			return NOS_RESULT_FAILED;
		return NOS_RESULT_SUCCESS;
	}
	NOS_REGISTER_NAME(LUT)
	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		nos::NodeExecuteParams execParams(params);
		auto outputBuf = execParams.GetPinObject<vkss::Buffer>(NSN_LUT);
		const auto& curve = *execParams.GetPinData<GammaCurve>(NOS_NAME_STATIC("GammaCurve"));
		const auto& dir = *execParams.GetPinData<GammaConversionType>(NOS_NAME_STATIC("Type"));
		if (Curve == curve && Type == dir)
			return NOS_RESULT_SUCCESS;
		constexpr auto outMemoryFlags = NOS_MEMORY_FLAGS_DEVICE_MEMORY;

		{
			auto outputBufInfo = vkss::GetResourceInfo(outputBuf);
			if (!outputBufInfo || outputBufInfo->MemoryFlags != outMemoryFlags ||
				outputBufInfo->Size != StagingBufInfo.Size)
			{
				outputBuf = vkss::CreateBuffer({.Size = StagingBufInfo.Size,
													.Usage = nosBufferUsage(NOS_BUFFER_USAGE_TRANSFER_DST | NOS_BUFFER_USAGE_TRANSFER_SRC | NOS_BUFFER_USAGE_STORAGE_BUFFER),
													.MemoryFlags = outMemoryFlags},
					"GammaLUT Buffer");
				SetPinObject(NSN_LUT, outputBuf);
			}
		}
		auto data = GetGammaLUT(dir == GammaConversionType::DECODE, curve, SSBO_SIZE);
		auto* ptr = nosVulkan->Map(StagingBuffer);
		memcpy(ptr, data.data(), data.size() * sizeof(uint16_t));
		nosEngine.LogI("GammaLUT: Buffer updated");
		Curve = curve;
		Type = dir;
		nosCmd cmd = vkss::BeginCmd(NOS_NAME("GammaLUT Staging Copy"), NodeId);
		nosVulkan->Copy(cmd, StagingBuffer, outputBuf, 0);
		nosVulkan->End(cmd, nullptr);
		return NOS_RESULT_SUCCESS;
	}
	
	static auto GetLUTFunction(bool toLinear, GammaCurve curve) -> double (*)(double)
	{
		switch (curve)
		{
		case GammaCurve::REC709:
		default:
			return toLinear ? [](double c) -> double { return (c < 0.081) ? (c / 4.5) : pow((c + 0.099) / 1.099, 1.0 / 0.45); }
			: [](double c) -> double { return (c < 0.018) ? (c * 4.5) : (pow(c, 0.45) * 1.099 - 0.099); };
		case GammaCurve::HLG:
			return toLinear
				   ? [](double c)
						 -> double { return (c < 0.5) ? (c * c / 3) : (exp(c / 0.17883277 - 5.61582460179) + 0.02372241); }
			: [](double c) -> double {
				return (c < 1. / 12.) ? sqrt(c * 3) : (std::log(c - 0.02372241) * 0.17883277 + 1.00429346);
		};
		case GammaCurve::ST2084:
			return toLinear ? 
					[](double c) -> double { c = pow(c, 0.01268331); return pow(glm::max(c - 0.8359375f, 0.) / (18.8515625  - 18.6875 * c), 6.27739463); } : 
						[](double c) -> double { c = pow(c, 0.15930175); return pow((0.8359375 + 18.8515625 * c) / (1 + 18.6875 * c), 78.84375); };
        case GammaCurve::SRGB:
            return toLinear ? [](double c) -> double { return (c <= 0.04045) ? (c / 12.92) : pow((c + 0.055) / 1.055, 2.4); }
                        : [](double c) -> double { return (c <= 0.0031308) ? (c * 12.92) : (pow(c, 1.0/2.4) * 1.055 - 0.055); };
        case GammaCurve::IDENTITY:
            return [](double c) { return c; };
		}
	}

	static std::vector<uint16_t> GetGammaLUT(bool toLinear, GammaCurve curve, uint16_t bits)
	{
		std::vector<uint16_t> re(1 << bits, 0.f);
		auto fn = GetLUTFunction(toLinear, curve);
		for (uint32_t i = 0; i < 1 << bits; ++i)
		{
			re[i] = uint16_t(double((1 << 16) - 1) * fn(double(i) / double((1 << bits) - 1)) + 0.5);
		}
		return re;
	}
	
	std::optional<GammaCurve> Curve = std::nullopt;
	std::optional<GammaConversionType> Type = std::nullopt;
};

nosResult RegisterGammaLUT(nosNodeFunctions* funcs)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("nos.mediaio.GammaLUT"), GammaLUTNodeContext, funcs);
	return NOS_RESULT_SUCCESS;
}

struct ColorSpaceMatrixNodeContext : NodeContext
{
	static std::array<double, 2> GetCoeffs(ColorSpace colorSpace)
	{
		switch (colorSpace)
		{
		case ColorSpace::REC601:
			return { .299, .114 };
		case ColorSpace::REC2020:
			return { .2627, .0593 };
		case ColorSpace::REC709:
		default:
			return { .2126, .0722 };
		}
	}

	template<class T>
	static glm::mat<4, 4, T> GetMatrix(ColorSpace colorSpace, uint32_t bitWidth, bool narrowRange)
	{
		// https://registry.khronos.org/DataFormat/specs/1.3/dataformat.1.3.html#MODEL_CONVERSION
		const auto [R, B] = GetCoeffs(colorSpace);
		const T G = T(1) - R - B; // Colorspace

		/*
		* https://registry.khronos.org/DataFormat/specs/1.3/dataformat.1.3.html#QUANTIZATION_NARROW
			Dequantization:
				n = Bit Width {8, 10, 12}
				Although unnoticable, quantization scales differs between bit widths
				This is merely mathematical perfection the error terms is less than 0.001
		*/

		const T QuantizationScalar = T(1 << (bitWidth - 8)) / T((1 << bitWidth) - 1);
		const T Y = narrowRange ? 219 * QuantizationScalar : 1;
		const T C = narrowRange ? 224 * QuantizationScalar : 1;
		const T YT = narrowRange ? 16 * QuantizationScalar : 0;
		const T CT = 128 * QuantizationScalar;
		const T CB = .5 * C / (B - 1);
		const T CR = .5 * C / (R - 1);

		const auto V0 = glm::vec<3, T>(R, G, B);
		const auto V1 = V0 - glm::vec<3, T>(0, 0, 1);
		const auto V2 = V0 - glm::vec<3, T>(1, 0, 0);

		return glm::transpose(glm::mat<4, 4, T>(
			glm::vec<4, T>(Y * V0, YT),
			glm::vec<4, T>(CB * V1, CT),
			glm::vec<4, T>(CR * V2, CT),
			glm::vec<4, T>(0, 0, 0, 1)));
	}

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		nos::NodeExecuteParams execParams(params);
		const auto& colorSpace = *execParams.GetPinData<ColorSpace>(NOS_NAME_STATIC("ColorSpace"));
		auto fmt = *execParams.GetPinData<YCbCrPixelFormat>(NOS_NAME_STATIC("PixelFormat"));
		const auto& dir = *execParams.GetPinData<GammaConversionType>(NOS_NAME_STATIC("Type"));
		auto narrowRange = *execParams.GetPinData<bool>(NOS_NAME_STATIC("NarrowRange"));
		glm::mat4 matrix = GetMatrix<double>(colorSpace, fmt == YCbCrPixelFormat::V210 ? 10 : 8, narrowRange);
		if(dir == GammaConversionType::DECODE)
			matrix = glm::inverse(matrix);
		SetPinValue(NOS_NAME_STATIC("Output"), matrix);
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterColorSpaceMatrix(nosNodeFunctions* funcs)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("nos.mediaio.ColorSpaceMatrix"), ColorSpaceMatrixNodeContext, funcs);
	return NOS_RESULT_SUCCESS;
}

struct YUY2ToRGBANodeContext : NodeContext
{
	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		nos::NodeExecuteParams execParams(params);
		auto res = *execParams.GetPinData<nos::fb::vec2u>(NOS_NAME("Resolution"));
		auto outputTex = execParams.GetPinObject<vkss::Texture>(NOS_NAME("Output"));
		auto inputBuf = execParams.GetPinObject<vkss::Buffer>(NOS_NAME("Input"));
		auto inputBufInfo = vkss::GetResourceInfo(inputBuf);
		if (!inputBufInfo || !inputBuf.IsValid())
		{
			nosEngine.LogE("YUY2ToRGBA Node: Input buffer is not valid!");
			return NOS_RESULT_FAILED;
		}
		auto outputTexInfo = vkss::GetResourceInfo(outputTex);

		constexpr auto reqFormat = NOS_FORMAT_R8G8B8A8_UNORM;

		if (!outputTexInfo || outputTexInfo->Width != res.x() || outputTexInfo->Height != res.y() ||
			outputTexInfo->Format != reqFormat)
		{
			SetPinObject(NOS_NAME("Output"),
						 vkss::CreateTexture(
							 {
								 .Width = res.x(),
								 .Height = res.y(),
								 .Format = reqFormat,
								 .FieldType = inputBufInfo->FieldType,
							 },
							 "YUY2RGBAResult"));
		}
		SetPinValue(NOS_NAME("DispatchSize"), nosVec2u(glm::ceil(res.x() / 16.0f), glm::ceil(res.y() / 8.0f)));
		return nosVulkan->ExecuteGPUNode(this, params);
	}
};

nosResult RegisterYUY2ToRGBA(nosNodeFunctions* funcs)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("nos.mediaio.YUY2ToRGBA"), YUY2ToRGBANodeContext, funcs);
	return NOS_RESULT_SUCCESS;
}

struct NV12ToRGBANodeContext : NodeContext
{
	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		nos::NodeExecuteParams execParams(params);
		auto res = *execParams.GetPinData<nos::fb::vec2u>(NOS_NAME("Resolution"));
		auto outputTex = execParams.GetPinObject<vkss::Texture>(NOS_NAME("Output"));
		auto inputBuf = execParams.GetPinObject<vkss::Buffer>(NOS_NAME("Input"));
		auto inputBufInfo = vkss::GetResourceInfo(inputBuf);
		if (!inputBufInfo || !inputBuf.IsValid())
		{
			nosEngine.LogE("YUY2ToRGBA Node: Input buffer is not valid!");
			return NOS_RESULT_FAILED;
		}
		auto outputTexInfo = vkss::GetResourceInfo(outputTex);
		constexpr auto reqFormat = NOS_FORMAT_R16G16B16A16_UNORM;


		if (!outputTexInfo || outputTexInfo->Width != res.x() || outputTexInfo->Height != res.y() ||
			outputTexInfo->Format != reqFormat)
		{
			SetPinObject(NOS_NAME("Output"),
						 vkss::CreateTexture(
							 {
								 .Width = res.x(),
								 .Height = res.y(),
								 .Format = reqFormat,
								 .FieldType = inputBufInfo->FieldType,
							 },
							 "NV12ToRGBAResult"));
		}
		// Work group size is 16x16, each thread processes 4x2 pixels
		SetPinValue(NOS_NAME("DispatchSize"), nosVec2u(glm::ceil(res.x()/32.0f), glm::ceil(res.y()/16.0f)));
		return nosVulkan->ExecuteGPUNode(this, params);
	}
};

nosResult RegisterNV12ToRGBA(nosNodeFunctions* funcs)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("nos.mediaio.NV12ToRGBA"), NV12ToRGBANodeContext, funcs);
	return NOS_RESULT_SUCCESS;
}

struct RGBA2BGR24BufferNodeContext : NodeContext
{
	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		nos::NodeExecuteParams execParams(params);
		auto inputTex = execParams.GetPinObject<vkss::Texture>(NOS_NAME_STATIC("Source"));
		auto outputBuf = execParams.GetPinObject<vkss::Buffer>(NOS_NAME_STATIC("Output"));
		auto inputTexInfo = *vkss::GetResourceInfo(inputTex);
		auto outputBufInfo = vkss::GetResourceInfo(outputBuf);
		// TODO: Transfer
		// output.mutate_field_type(sys::vulkan::FieldType::PROGRESSIVE);

		nosVec2u ext = {inputTexInfo.Width, inputTexInfo.Height};

		uint32_t bufSize = ext.x * ext.y * 3;
		constexpr auto outMemoryFlags = NOS_MEMORY_FLAGS_DEVICE_MEMORY;
		if (!outputBufInfo || !outputBufInfo->Size != bufSize || outputBufInfo->MemoryFlags != outMemoryFlags)
		{
			SetPinObject(
				NOS_NAME_STATIC("Output"),
				vkss::CreateBuffer(
					{
						.Size = (uint32_t)bufSize,
						.Usage = nosBufferUsage(NOS_BUFFER_USAGE_TRANSFER_SRC | NOS_BUFFER_USAGE_STORAGE_BUFFER),
						.MemoryFlags = outMemoryFlags,
						.FieldType = nosTextureFieldType::NOS_TEXTURE_FIELD_TYPE_PROGRESSIVE,
					},
					"RGBA2BGR24Result"));
		}
		SetPinValue(NOS_NAME("DispatchSize"), nosVec2u(ext.x / 4, ext.y));
		return nosVulkan->ExecuteGPUNode(this, params);
	}
};

nosResult RegisterRGBAToBGR24Buffer(nosNodeFunctions* funcs)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("nos.mediaio.RGBAToBGR24Buffer"), RGBA2BGR24BufferNodeContext, funcs);
	return NOS_RESULT_SUCCESS;
}

}
