// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <Nodos/Plugin.hpp>
#include <nosSysVulkan/Helpers.hpp>

namespace nos::mediaio
{

// Recycling pool for the large per-frame buffers (decoded channels, interleaved pixels, file
// bytes). EXR sequences allocate the same sizes every frame, so exact-size reuse hits nearly
// always and avoids the VirtualAlloc/page-fault/TLB-shootdown churn that serialises concurrent
// decodes (frees ~100 ms/frame at high worker counts). Thread-safe.
void* PoolAlloc(size_t bytes);              // throws std::bad_alloc on failure
void PoolFree(void* ptr, size_t bytes) noexcept;

template <class T>
struct PoolAllocator
{
	using value_type = T;
	PoolAllocator() = default;
	template <class U> PoolAllocator(const PoolAllocator<U>&) noexcept {}
	T* allocate(size_t n) { return static_cast<T*>(PoolAlloc(n * sizeof(T))); }
	void deallocate(T* p, size_t n) noexcept { PoolFree(p, n * sizeof(T)); }
	bool operator==(const PoolAllocator&) const noexcept { return true; }
	bool operator!=(const PoolAllocator&) const noexcept { return false; }
};
template <class T> using PooledVector = std::vector<T, PoolAllocator<T>>;

// CPU-side decoded EXR frame: interleaved RGBA colour (half or float) plus an optional R32F
// depth channel, plus the header metadata. Holds no GPU objects, so DecodeExr may be called
// from any thread; the caller uploads with UploadColor / UploadDepth afterwards.
struct ExrFrame
{
	int Width = 0, Height = 0;
	nosFormat ColorFormat = NOS_FORMAT_NONE;   // RGBA16F (HALF source) or RGBA32F
	PooledVector<uint8_t> Color;               // Width*Height*4 texels, in ColorFormat
	bool HasDepth = false;
	PooledVector<float> Depth;                 // Width*Height R32F

	// Header metadata, mirrored onto the EXRMetadata pin by BuildExrMetadata.
	uint32_t PixelType = 3;                    // EXRPixelType value (default MIXED)
	std::string Compression;
	int32_t DataWindow[4] = { 0, 0, 0, 0 };
	int32_t DisplayWindow[4] = { 0, 0, 0, 0 };
	float PixelAspectRatio = 1.0f;
	std::vector<std::string> Channels;
	std::vector<std::string> Layers;           // selectable layer names in the file
	uint32_t NumParts = 1;
	std::string LoadedLayer;
};

// Decodes an EXR from an in-memory file image into out. layerArg is empty for the file's
// default channels, or a layer / multipart part name. Returns false and fills err on failure.
// Thread-safe; touches no disk.
bool DecodeExrFromMemory(const uint8_t* data, size_t size, const std::string& layerArg,
	ExrFrame& out, std::string& err);

// Convenience wrapper: reads the whole file at path, then DecodeExrFromMemory.
bool DecodeExr(const std::string& path, const std::string& layerArg,
	ExrFrame& out, std::string& err);

// GPU uploads. Safe to call off the execute thread (the Vulkan subsystem is thread-safe for
// these). Return an invalid ref on allocation failure. When outEvent is null the upload is
// waited on before returning; when non-null the call returns immediately after submission and
// stores the GPU event, which the caller must WaitGpuEvent exactly once before using or
// destroying the texture (this lets the wait overlap other work, e.g. the next graph execution).
TypedObjectRef<sys::vulkan::Texture> UploadColor(const ExrFrame& frame, nos::uuid nodeId,
	nosGPUEvent* outEvent = nullptr);
TypedObjectRef<sys::vulkan::Texture> UploadDepth(const ExrFrame& frame, nos::uuid nodeId,
	nosGPUEvent* outEvent = nullptr);

// Builds the nos.mediaio.EXRMetadata flatbuffer for the Metadata pin.
nos::Buffer BuildExrMetadata(const ExrFrame& frame);

} // namespace nos::mediaio
