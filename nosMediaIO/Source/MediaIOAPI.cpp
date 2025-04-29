// Copyright MediaZ Teknoloji A.S. All Rights Reserved.
#include <Nodos/PluginAPI.h>
#include <nosMediaIO/nosMediaIO.h>

namespace nos::mediaio
{
std::unordered_map<uint32_t, nosMediaIOAPI*> GExportedAPIVersions;

const char* NOSAPI_CALL GetFrameGeometryName(nosMediaIOFrameGeometry geometry)
{
	if (geometry < NOS_MEDIAIO_FRAME_GEOMETRY_MIN || geometry > NOS_MEDIAIO_FRAME_GEOMETRY_MAX)
		return NOS_MEDIAIO_FRAME_GEOMETRY_NAMES[NOS_MEDIAIO_FRAME_GEOMETRY_INVALID];
	return NOS_MEDIAIO_FRAME_GEOMETRY_NAMES[geometry];
}

const char* NOSAPI_CALL GetFrameRateName(nosMediaIOFrameRate frameRate)
{
	if (frameRate < NOS_MEDIAIO_FRAME_RATE_MIN || frameRate > NOS_MEDIAIO_FRAME_RATE_MAX)
		return NOS_MEDIAIO_FRAME_RATE_NAMES[NOS_MEDIAIO_FRAME_RATE_INVALID];
	return NOS_MEDIAIO_FRAME_RATE_NAMES[frameRate];
}

const char* NOSAPI_CALL GetPixelFormatName(nosMediaIOPixelFormat pixelFormat)
{
	if (pixelFormat < NOS_MEDIAIO_PIXEL_FORMAT_MIN || pixelFormat > NOS_MEDIAIO_PIXEL_FORMAT_MAX)
		return NOS_MEDIAIO_PIXEL_FORMAT_NAMES[NOS_MEDIAIO_PIXEL_FORMAT_INVALID];
	return NOS_MEDIAIO_PIXEL_FORMAT_NAMES[pixelFormat];
}

const char* NOSAPI_CALL GetVideoScanTypeName(nosMediaIOVideoScanType scanType)
{
	if (scanType < NOS_MEDIAIO_VIDEO_SCAN_TYPE_MIN || scanType > NOS_MEDIAIO_VIDEO_SCAN_TYPE_MAX)
		return NOS_MEDIAIO_VIDEO_SCAN_TYPE_NAMES[NOS_MEDIAIO_VIDEO_SCAN_TYPE_INVALID];
	return NOS_MEDIAIO_VIDEO_SCAN_TYPE_NAMES[scanType];
}

nosMediaIOFrameGeometry NOSAPI_CALL GetFrameGeometryFromString(const char* str)
{
	for (size_t i = NOS_MEDIAIO_FRAME_GEOMETRY_MIN; i <= NOS_MEDIAIO_FRAME_GEOMETRY_MAX; ++i)
	{
		if (strcmp(str, NOS_MEDIAIO_FRAME_GEOMETRY_NAMES[i]) == 0)
			return static_cast<nosMediaIOFrameGeometry>(i);
	}
	return NOS_MEDIAIO_FRAME_GEOMETRY_INVALID;
}

nosMediaIOFrameRate NOSAPI_CALL GetFrameRateFromString(const char* str)
{
	for (size_t i = NOS_MEDIAIO_FRAME_RATE_MIN; i <= NOS_MEDIAIO_FRAME_RATE_MAX; ++i)
	{
		if (strcmp(str, NOS_MEDIAIO_FRAME_RATE_NAMES[i]) == 0)
			return static_cast<nosMediaIOFrameRate>(i);
	}
	return NOS_MEDIAIO_FRAME_RATE_INVALID;
}

nosMediaIOPixelFormat NOSAPI_CALL GetPixelFormatFromString(const char* str)
{
	for (size_t i = NOS_MEDIAIO_PIXEL_FORMAT_MIN; i <= NOS_MEDIAIO_PIXEL_FORMAT_MAX; ++i)
	{
		if (strcmp(str, NOS_MEDIAIO_PIXEL_FORMAT_NAMES[i]) == 0)
			return static_cast<nosMediaIOPixelFormat>(i);
	}
	return NOS_MEDIAIO_PIXEL_FORMAT_INVALID;
}

nosMediaIOVideoScanType NOSAPI_CALL GetVideoScanTypeFromString(const char* str)
{
	for (size_t i = NOS_MEDIAIO_VIDEO_SCAN_TYPE_MIN; i <= NOS_MEDIAIO_VIDEO_SCAN_TYPE_MAX; ++i)
	{
		if (strcmp(str, NOS_MEDIAIO_VIDEO_SCAN_TYPE_NAMES[i]) == 0)
			return static_cast<nosMediaIOVideoScanType>(i);
	}
	return NOS_MEDIAIO_VIDEO_SCAN_TYPE_INVALID;
}

nosResult NOSAPI_CALL GetFrameRateDeltaSeconds(nosMediaIOFrameRate frameRate, nosVec2u* outDeltaSeconds)
{
	if (outDeltaSeconds == nullptr)
		return NOS_RESULT_INVALID_ARGUMENT;

	static const std::unordered_map<nosMediaIOFrameRate, nosVec2u> frameRateTable = {
		{NOS_MEDIAIO_FRAME_RATE_2398, {1001, 24000}},
		{NOS_MEDIAIO_FRAME_RATE_24, {1, 24}},
		{NOS_MEDIAIO_FRAME_RATE_25, {1, 25}},
		{NOS_MEDIAIO_FRAME_RATE_2997, {1001, 30000}},
		{NOS_MEDIAIO_FRAME_RATE_30, {1, 30}},
		{NOS_MEDIAIO_FRAME_RATE_4795, {1001, 20000}},
		{NOS_MEDIAIO_FRAME_RATE_48, {1, 48}},
		{NOS_MEDIAIO_FRAME_RATE_50, {1, 50}},
		{NOS_MEDIAIO_FRAME_RATE_5994, {1001, 60000}},
		{NOS_MEDIAIO_FRAME_RATE_60, {1, 60}},
		{NOS_MEDIAIO_FRAME_RATE_9590, {1001, 96000}},
		{NOS_MEDIAIO_FRAME_RATE_96, {1, 96}},
		{NOS_MEDIAIO_FRAME_RATE_100, {1, 100}},
		{NOS_MEDIAIO_FRAME_RATE_11988, {3001, 120000}},
		{NOS_MEDIAIO_FRAME_RATE_120, {1, 120}}
	};

	auto it = frameRateTable.find(frameRate);
	if (it != frameRateTable.end())
	{
		*outDeltaSeconds = it->second;
		return NOS_RESULT_SUCCESS;
	}

	return NOS_RESULT_INVALID_ARGUMENT;
}

nosResult NOSAPI_CALL Get2DFrameResolution(nosMediaIOFrameGeometry geom, nosVec2u* outResolution)
{
	if (outResolution == nullptr)
		return NOS_RESULT_INVALID_ARGUMENT;

	static const std::unordered_map<nosMediaIOFrameGeometry, nosVec2u> frameGeometryTable = {
		{NOS_MEDIAIO_FRAME_GEOMETRY_NTSC, {720, 486}},
		{NOS_MEDIAIO_FRAME_GEOMETRY_PAL, {720, 576}},
		{NOS_MEDIAIO_FRAME_GEOMETRY_HD720, {1280, 720}},
		{NOS_MEDIAIO_FRAME_GEOMETRY_HD1080, {1920, 1080}},
		{NOS_MEDIAIO_FRAME_GEOMETRY_2K, {2048, 1080}},
		{NOS_MEDIAIO_FRAME_GEOMETRY_2KDCI, {2048, 1080}},
		{NOS_MEDIAIO_FRAME_GEOMETRY_4K2160, {3840, 2160}},
		{NOS_MEDIAIO_FRAME_GEOMETRY_4KDCI, {4096, 2160}},
		{NOS_MEDIAIO_FRAME_GEOMETRY_8K4320, {7680, 4320}},
		{NOS_MEDIAIO_FRAME_GEOMETRY_8KDCI, {8192, 4320}},
		{NOS_MEDIAIO_FRAME_GEOMETRY_640x480, {640, 480}},
		{NOS_MEDIAIO_FRAME_GEOMETRY_800x600, {800, 600}},
		{NOS_MEDIAIO_FRAME_GEOMETRY_1440x900, {1440, 900}},
		{NOS_MEDIAIO_FRAME_GEOMETRY_1440x1080, {1440, 1080}},
		{NOS_MEDIAIO_FRAME_GEOMETRY_1600x1200, {1600, 1200}},
		{NOS_MEDIAIO_FRAME_GEOMETRY_1920x1200, {1920, 1200}},
		{NOS_MEDIAIO_FRAME_GEOMETRY_1920x1440, {1920, 1440}},
		{NOS_MEDIAIO_FRAME_GEOMETRY_2560x1440, {2560, 1440}},
		{NOS_MEDIAIO_FRAME_GEOMETRY_2560x1600, {2560, 1600}}
	};

	auto it = frameGeometryTable.find(geom);
	if (it != frameGeometryTable.end())
	{
		*outResolution = it->second;
		return NOS_RESULT_SUCCESS;
	}

	return NOS_RESULT_INVALID_ARGUMENT;
}

nosResult NOSAPI_CALL Export(uint32_t minorVersion, void** outAPI)
{
	auto it = GExportedAPIVersions.find(minorVersion);
	if (it != GExportedAPIVersions.end())
	{
		*outAPI = it->second;
		return NOS_RESULT_SUCCESS;
	}
	nosMediaIOAPI* api = new nosMediaIOAPI();
	api->GetFrameGeometryName = GetFrameGeometryName;
	api->GetFrameRateName = GetFrameRateName;
	api->GetPixelFormatName = GetPixelFormatName;
	api->GetFrameGeometryFromString = GetFrameGeometryFromString;
	api->GetFrameRateFromString = GetFrameRateFromString;
	api->GetPixelFormatFromString = GetPixelFormatFromString;
	api->GetFrameRateDeltaSeconds = GetFrameRateDeltaSeconds;
	api->Get2DFrameResolution = Get2DFrameResolution;
	api->GetVideoScanTypeName = GetVideoScanTypeName;
	api->GetVideoScanTypeFromString = GetVideoScanTypeFromString;
	*outAPI = api;
	GExportedAPIVersions[minorVersion] = api;
	return NOS_RESULT_SUCCESS;
}
} 