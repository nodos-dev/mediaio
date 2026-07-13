// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include "ExrDecode.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include <exr.h> // tinyexr v3 (pure C11; libdeflate zlib backend, SIMD kernels)

#include "nosMediaio/ReadEXR_generated.h"

#ifdef _MSC_VER
#include <intrin.h>
// tinyexr v3's HTJ2K sources call __builtin_clz unguarded; MSVC has no such intrinsic.
extern "C" int __builtin_clz(unsigned x)
{
	unsigned long i;
	_BitScanReverse(&i, x);
	return 31 - int(i);
}
#endif

namespace nos::mediaio
{
namespace
{

// ---- Buffer pool -----------------------------------------------------------------------

// Large blocks are recycled by exact size under one mutex; small ones go straight to malloc.
// Lock traffic is a few dozen ops per frame, so contention is negligible next to the
// VirtualAlloc + soft-page-fault cost of allocating ~300 MB fresh per decoded frame.
constexpr size_t POOL_MIN_BLOCK = 1u << 20;         // pool only blocks >= 1 MB
constexpr size_t POOL_CAP_BYTES = 2ull << 30;       // beyond this, frees really free

struct BufferPool
{
	std::mutex Mtx;
	std::multimap<size_t, void*> Free; // exact size -> block
	size_t Bytes = 0;

	~BufferPool()
	{
		for (auto& [size, ptr] : Free)
			free(ptr);
	}

	void* Take(size_t size)
	{
		std::lock_guard<std::mutex> lk(Mtx);
		auto it = Free.find(size);
		if (it == Free.end())
			return nullptr;
		void* p = it->second;
		Free.erase(it);
		Bytes -= size;
		return p;
	}

	bool Put(void* ptr, size_t size)
	{
		std::lock_guard<std::mutex> lk(Mtx);
		if (Bytes + size > POOL_CAP_BYTES)
			return false;
		Free.emplace(size, ptr);
		Bytes += size;
		return true;
	}
};

BufferPool& GetPool()
{
	static BufferPool pool;
	return pool;
}

void* PoolAllocNoThrow(size_t bytes)
{
	if (bytes >= POOL_MIN_BLOCK)
		if (void* p = GetPool().Take(bytes))
			return p;
	return malloc(bytes);
}

// tinyexr v3 allocator hooks. exr_allocator's free callback gets no size, so stash it in a
// 16-byte header (keeps the payload 16-byte aligned for the SIMD kernels).
void* ExrAlloc(void*, size_t size)
{
	void* base = PoolAllocNoThrow(size + 16);
	if (!base)
		return nullptr;
	*static_cast<size_t*>(base) = size + 16;
	return static_cast<char*>(base) + 16;
}

void ExrFree(void*, void* ptr)
{
	if (!ptr)
		return;
	void* base = static_cast<char*>(ptr) - 16;
	PoolFree(base, *static_cast<size_t*>(base));
}

constexpr exr_allocator POOLED_EXR_ALLOCATOR = { nullptr, ExrAlloc, ExrFree };

} // namespace

void* PoolAlloc(size_t bytes)
{
	void* p = PoolAllocNoThrow(bytes);
	if (!p)
		throw std::bad_alloc();
	return p;
}

void PoolFree(void* ptr, size_t bytes) noexcept
{
	if (!ptr)
		return;
	if (bytes >= POOL_MIN_BLOCK && GetPool().Put(ptr, bytes))
		return;
	free(ptr);
}

namespace
{

// float -> IEEE 754 binary16 (round-to-nearest-even). Handles denormals, overflow and NaN.
uint16_t FloatToHalf(float f)
{
	uint16_t out;
	exr_float_to_half(&f, &out, 1);
	return out;
}

// IEEE 754 binary16 -> float.
float HalfToFloat(uint16_t h)
{
	float out;
	exr_half_to_float(&h, &out, 1);
	return out;
}

// Reads decoded channel sample p as float, honoring its pixel type.
float SampleFloat(const void* src, exr_pixel_type type, size_t p)
{
	if (type == EXR_PIXEL_HALF)
		return HalfToFloat(static_cast<const uint16_t*>(src)[p]);
	if (type == EXR_PIXEL_UINT)
		return float(static_cast<const uint32_t*>(src)[p]);
	return static_cast<const float*>(src)[p];
}

// Splits an EXR channel name into layer + base: "diffuse.R" -> ("diffuse","R"), "R" -> ("","R").
void SplitChannel(const std::string& name, std::string& layer, std::string& base)
{
	auto dot = name.rfind('.');
	if (dot == std::string::npos) { layer.clear(); base = name; }
	else { layer.assign(name, 0, dot); base.assign(name, dot + 1, std::string::npos); }
}

// Index of the channel in layerArg with the given base name, or -1.
int ChannelIndex(const exr_header& header, const std::string& layerArg, const char* base)
{
	std::string layer, b;
	for (int i = 0; i < header.num_channels; ++i)
	{
		SplitChannel(header.channels[i].name, layer, b);
		if (layer == layerArg && b == base)
			return i;
	}
	return -1;
}

// Distinct layer names present in the file (channels carrying a '.' prefix).
std::vector<std::string> DeriveLayerNames(const exr_header& header)
{
	std::vector<std::string> out;
	std::set<std::string> seen;
	std::string layer, base;
	for (int i = 0; i < header.num_channels; ++i)
	{
		SplitChannel(header.channels[i].name, layer, base);
		if (!layer.empty() && seen.insert(layer).second)
			out.push_back(layer);
	}
	return out;
}

const char* CompressionName(exr_compression type)
{
	switch (type)
	{
	case EXR_COMPRESSION_NONE:      return "none";
	case EXR_COMPRESSION_RLE:       return "rle";
	case EXR_COMPRESSION_ZIPS:      return "zips";
	case EXR_COMPRESSION_ZIP:       return "zip";
	case EXR_COMPRESSION_PIZ:       return "piz";
	case EXR_COMPRESSION_PXR24:     return "pxr24";
	case EXR_COMPRESSION_B44:       return "b44";
	case EXR_COMPRESSION_B44A:      return "b44a";
	case EXR_COMPRESSION_DWAA:      return "dwaa";
	case EXR_COMPRESSION_DWAB:      return "dwab";
	case EXR_COMPRESSION_HTJ2K256:  return "htj2k256";
	case EXR_COMPRESSION_HTJ2K32:   return "htj2k32";
	case EXR_COMPRESSION_ZSTD:      return "zstd";
	default:                        return "unknown";
	}
}

// Copies the header's fixed metadata (windows, compression, aspect, channels) into the frame.
void FillMetadata(const exr_header& header, ExrFrame& out)
{
	out.Compression = CompressionName(header.compression);
	out.DataWindow[0] = header.data_window.min_x; out.DataWindow[1] = header.data_window.min_y;
	out.DataWindow[2] = header.data_window.max_x; out.DataWindow[3] = header.data_window.max_y;
	out.DisplayWindow[0] = header.display_window.min_x; out.DisplayWindow[1] = header.display_window.min_y;
	out.DisplayWindow[2] = header.display_window.max_x; out.DisplayWindow[3] = header.display_window.max_y;
	out.PixelAspectRatio = header.pixel_aspect_ratio;
	out.Channels.clear();
	out.Channels.reserve(header.num_channels);
	for (int i = 0; i < header.num_channels; ++i)
		out.Channels.emplace_back(header.channels[i].name);
}

// Precision of the loaded RGBA set: HALF only when every present colour channel is HALF.
// outHalf drives the texture format; the returned EXRPixelType is for the Metadata pin.
uint32_t DecidePixelType(const exr_header& header, const int idx[4], bool& outHalf)
{
	bool anyPresent = false, allHalf = true, allFloat = true, allUint = true;
	for (int k = 0; k < 4; ++k)
		if (idx[k] >= 0)
		{
			anyPresent = true;
			exr_pixel_type t = header.channels[idx[k]].pixel_type;
			allHalf &= (t == EXR_PIXEL_HALF);
			allFloat &= (t == EXR_PIXEL_FLOAT);
			allUint &= (t == EXR_PIXEL_UINT);
		}
	outHalf = anyPresent && allHalf;
	if (!anyPresent) return uint32_t(EXRPixelType::MIXED);
	if (allHalf) return uint32_t(EXRPixelType::HALF);
	if (allFloat) return uint32_t(EXRPixelType::FLOAT);
	if (allUint) return uint32_t(EXRPixelType::UINT);
	return uint32_t(EXRPixelType::MIXED);
}

// Interleaves the four (possibly absent) colour channels of the planar part into out.Color,
// keeping the file's precision. Absent channels default to 0, alpha to 1.
void BuildColor(const exr_part& part, const int idx[4], bool half, ExrFrame& out)
{
	const exr_header& header = part.header;
	const size_t count = size_t(part.width) * size_t(part.height);
	if (half)
	{
		// All present channels are HALF: one fused pass, sequential reads and writes.
		out.Color.resize(count * 4 * sizeof(uint16_t));
		auto* dst = reinterpret_cast<uint16_t*>(out.Color.data());
		static const uint16_t def[4] = { 0, 0, 0, 0x3C00 }; // 0,0,0,1.0h
		const uint16_t* src[4];
		for (int k = 0; k < 4; ++k)
			src[k] = idx[k] >= 0 ? static_cast<const uint16_t*>(part.images[idx[k]]) : nullptr;
		for (size_t p = 0; p < count; ++p)
		{
			dst[p * 4 + 0] = src[0] ? src[0][p] : def[0];
			dst[p * 4 + 1] = src[1] ? src[1][p] : def[1];
			dst[p * 4 + 2] = src[2] ? src[2][p] : def[2];
			dst[p * 4 + 3] = src[3] ? src[3][p] : def[3];
		}
	}
	else
	{
		out.Color.resize(count * 4 * sizeof(float));
		auto* dst = reinterpret_cast<float*>(out.Color.data());
		static const float def[4] = { 0.f, 0.f, 0.f, 1.f };
		for (int k = 0; k < 4; ++k)
		{
			if (idx[k] < 0)
			{
				for (size_t p = 0; p < count; ++p) dst[p * 4 + k] = def[k];
				continue;
			}
			const void* src = part.images[idx[k]];
			const exr_pixel_type t = header.channels[idx[k]].pixel_type;
			if (t == EXR_PIXEL_FLOAT)
			{
				const float* s = static_cast<const float*>(src);
				for (size_t p = 0; p < count; ++p) dst[p * 4 + k] = s[p];
			}
			else
				for (size_t p = 0; p < count; ++p) dst[p * 4 + k] = SampleFloat(src, t, p);
		}
	}
}

// Fills out.Depth (R32F) from channel zIdx of the planar part.
void BuildDepth(const exr_part& part, int zIdx, ExrFrame& out)
{
	const size_t count = size_t(part.width) * size_t(part.height);
	out.Depth.resize(count);
	const void* src = part.images[zIdx];
	const exr_pixel_type type = part.header.channels[zIdx].pixel_type;
	if (type == EXR_PIXEL_FLOAT)
		std::memcpy(out.Depth.data(), src, count * sizeof(float));
	else if (type == EXR_PIXEL_HALF)
		exr_half_to_float(static_cast<const uint16_t*>(src), out.Depth.data(), count);
	else
		for (size_t p = 0; p < count; ++p) out.Depth[p] = SampleFloat(src, type, p);
	out.HasDepth = true;
}

} // namespace

bool DecodeExrFromMemory(const uint8_t* data, size_t size, const std::string& layerArg,
	ExrFrame& out, std::string& err)
{
	exr_reader* reader = nullptr;
	exr_result res = exr_reader_open_memory(data, size, &POOLED_EXR_ALLOCATOR, &reader);
	if (!EXR_OK(res))
	{
		err = exr_result_string(res);
		return false;
	}

	res = exr_reader_parse_header(reader);
	if (!EXR_OK(res))
	{
		err = exr_result_string(res);
		exr_reader_close(reader);
		return false;
	}

	out.NumParts = uint32_t(exr_reader_num_parts(reader));
	const exr_header* header = exr_reader_part_header(reader, 0);
	out.Layers = DeriveLayerNames(*header);
	out.LoadedLayer = layerArg;
	FillMetadata(*header, out);

	int idx[4] = {
		ChannelIndex(*header, layerArg, "R"),
		ChannelIndex(*header, layerArg, "G"),
		ChannelIndex(*header, layerArg, "B"),
		ChannelIndex(*header, layerArg, "A"),
	};
	if (idx[0] < 0 && idx[1] < 0 && idx[2] < 0 && idx[3] < 0)
	{
		err = "layer '" + (layerArg.empty() ? std::string("(default)") : layerArg) + "' has no RGBA channels";
		exr_reader_close(reader);
		return false;
	}
	int zIdx = ChannelIndex(*header, layerArg, "Z");
	if (zIdx < 0)
		zIdx = ChannelIndex(*header, layerArg, "z");

	// The interleave below indexes each selected plane at full resolution.
	for (int i = 0; i < header->num_channels; ++i)
		if ((header->channels[i].x_sampling != 1 || header->channels[i].y_sampling != 1)
			&& (i == idx[0] || i == idx[1] || i == idx[2] || i == idx[3] || i == zIdx))
		{
			err = "subsampled channels are not supported";
			exr_reader_close(reader);
			return false;
		}

	exr_part part;
	std::memset(&part, 0, sizeof(part));
	res = exr_reader_read_part(reader, 0, &part);
	if (!EXR_OK(res))
	{
		err = exr_result_string(res);
		exr_reader_close(reader);
		return false;
	}
	if (part.is_deep || !part.images)
	{
		err = part.is_deep ? "deep images are not supported" : "no readable channels";
		exr_part_free(&POOLED_EXR_ALLOCATOR, &part);
		exr_reader_close(reader);
		return false;
	}

	bool half = false;
	out.PixelType = DecidePixelType(*header, idx, half);
	out.Width = part.width;
	out.Height = part.height;
	out.ColorFormat = half ? NOS_FORMAT_R16G16B16A16_SFLOAT : NOS_FORMAT_R32G32B32A32_SFLOAT;
	BuildColor(part, idx, half, out);
	if (zIdx >= 0)
		BuildDepth(part, zIdx, out);

	exr_part_free(&POOLED_EXR_ALLOCATOR, &part);
	exr_reader_close(reader);
	return true;
}

bool DecodeExr(const std::string& path, const std::string& layerArg, ExrFrame& out, std::string& err)
{
	// fopen + one unbuffered fread: several times faster than MSVC ifstream on 45 MB files.
#ifdef _WIN32
	FILE* file = _wfopen(nos::Utf8ToPath(path).wstring().c_str(), L"rb");
#else
	FILE* file = fopen(nos::Utf8ToPath(path).string().c_str(), "rb");
#endif
	if (!file)
	{
		err = "cannot open file";
		return false;
	}
	setvbuf(file, nullptr, _IONBF, 0);
	fseek(file, 0, SEEK_END);
#ifdef _WIN32
	long long size = _ftelli64(file);
#else
	long long size = ftello(file);
#endif
	fseek(file, 0, SEEK_SET);
	PooledVector<uint8_t> bytes(size_t(size < 0 ? 0 : size));
	if (bytes.empty() || fread(bytes.data(), 1, bytes.size(), file) != bytes.size())
	{
		fclose(file);
		err = "cannot read file";
		return false;
	}
	fclose(file);
	return DecodeExrFromMemory(bytes.data(), bytes.size(), layerArg, out, err);
}

TypedObjectRef<sys::vulkan::Texture> UploadColor(const ExrFrame& frame, nos::uuid nodeId)
{
	if (frame.Color.empty() || frame.ColorFormat == NOS_FORMAT_NONE)
		return {};

	nosTextureInfo texInfo = {
		.Width = uint32_t(frame.Width),
		.Height = uint32_t(frame.Height),
		.Format = frame.ColorFormat,
	};

	auto texture = sys::vulkan::CreateTexture(texInfo, "EXR Texture");
	if (!texture.IsValid())
		return {};
	nosVulkan->SetResourceFieldType(texture, NOS_TEXTURE_FIELD_TYPE_PROGRESSIVE);

	nosCmd cmd = sys::vulkan::BeginCmd(NOS_NAME("EXR Upload"), nodeId);
	nosVulkan->ImageLoad(cmd, frame.Color.data(), nosVec2u(frame.Width, frame.Height),
		frame.ColorFormat, texture, NOS_TEXTURE_FILTER_NEAREST);
	nosGPUEvent event = 0;
	nosCmdEndParams endParams{ .ForceSubmit = true, .OutGPUEventHandle = &event };
	nosVulkan->End(cmd, &endParams);
	nosVulkan->WaitGpuEvent(&event, UINT64_MAX); // upload must finish before the texture is used
	return texture;
}

TypedObjectRef<sys::vulkan::Texture> UploadDepth(const ExrFrame& frame, nos::uuid nodeId)
{
	if (!frame.HasDepth || frame.Depth.empty())
		return {};

	nosTextureInfo texInfo = {
		.Width = uint32_t(frame.Width),
		.Height = uint32_t(frame.Height),
		.Format = NOS_FORMAT_R32_SFLOAT,
	};

	auto texture = sys::vulkan::CreateTexture(texInfo, "EXR Depth");
	if (!texture.IsValid())
		return {};
	nosVulkan->SetResourceFieldType(texture, NOS_TEXTURE_FIELD_TYPE_PROGRESSIVE);

	nosCmd cmd = sys::vulkan::BeginCmd(NOS_NAME("EXR Depth Upload"), nodeId);
	nosVulkan->ImageLoad(cmd, frame.Depth.data(), nosVec2u(frame.Width, frame.Height),
		NOS_FORMAT_R32_SFLOAT, texture, NOS_TEXTURE_FILTER_NEAREST);
	nosGPUEvent event = 0;
	nosCmdEndParams endParams{ .ForceSubmit = true, .OutGPUEventHandle = &event };
	nosVulkan->End(cmd, &endParams);
	nosVulkan->WaitGpuEvent(&event, UINT64_MAX);
	return texture;
}

nos::Buffer BuildExrMetadata(const ExrFrame& frame)
{
	flatbuffers::FlatBufferBuilder fbb;
	auto compression = fbb.CreateString(frame.Compression);
	auto channelsVec = fbb.CreateVectorOfStrings(frame.Channels);
	auto layersVec = fbb.CreateVectorOfStrings(frame.Layers);
	auto loadedLayerStr = fbb.CreateString(frame.LoadedLayer);
	Box2i dataWindow(frame.DataWindow[0], frame.DataWindow[1], frame.DataWindow[2], frame.DataWindow[3]);
	Box2i displayWindow(frame.DisplayWindow[0], frame.DisplayWindow[1],
		frame.DisplayWindow[2], frame.DisplayWindow[3]);

	EXRMetadataBuilder mb(fbb);
	mb.add_width(uint32_t(frame.Width));
	mb.add_height(uint32_t(frame.Height));
	mb.add_data_window(&dataWindow);
	mb.add_display_window(&displayWindow);
	mb.add_pixel_aspect_ratio(frame.PixelAspectRatio);
	mb.add_pixel_type(EXRPixelType(frame.PixelType));
	mb.add_compression(compression);
	mb.add_channels(channelsVec);
	mb.add_layers(layersVec);
	mb.add_num_parts(frame.NumParts);
	mb.add_loaded_layer(loadedLayerStr);
	fbb.Finish(mb.Finish());
	return nos::Buffer(fbb.Release());
}

} // namespace nos::mediaio
