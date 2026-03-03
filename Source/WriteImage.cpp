// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>
#include "Builtins_generated.h"
#include <AppService_generated.h>
#define STBIW_WINDOWS_UTF8
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <nosSysVulkan/Helpers.hpp>

#include <mutex>

#include "Names.h"

namespace nos::mediaio
{
NOS_REGISTER_NAME(IncludeAlpha);
NOS_REGISTER_NAME_SPACED(Nos_Utilities_WriteImage, "nos.mediaio.WriteImage")

struct WriteImage : NodeContext
{
	std::filesystem::path Path;
	std::optional<TypedObjectRef<sys::vulkan::Texture>> TempSrgbCopy;
	bool IncludeAlpha = false;
	nosGPUEvent Event = 0;
	std::atomic_bool WriteRequested = false;
	std::condition_variable CV;
	std::mutex Mutex;
	std::thread Worker;
	std::atomic_bool Write = false;
	std::atomic_bool ShouldStop = false;

	nosResult OnCreate(nosFbNodePtr node) override
	{
		Worker = std::thread([this] {
			while (!ShouldStop)
			{
				std::unique_lock<std::mutex> lock(Mutex);
				CV.wait(lock, [this] { return Write || ShouldStop; });
				if (ShouldStop)
					break;
				if (Event)
					nosVulkan->WaitGpuEvent(&Event, UINT64_MAX);
				if (this->Write)
				{
					this->Write = false;
					this->WriteImageToFile();
				}
			}
		});
		if (node->pins())
		{
			for (auto* pin : *node->pins())
			{
				auto* pinData = pin->data();
				nosBuffer value = {.Data = (void*)pinData->data(), .Size = pinData->size()};
				OnPinValueChanged(nos::Name(pin->name()->c_str()), *pin->id(), value);
			}
		}
		return NOS_RESULT_SUCCESS;
	}

	~WriteImage()
	{
		ShouldStop = true;
		CV.notify_all();
		Worker.join();
	}

	void SendWriteRequest(nosFunctionExecuteParams* params)
	{
		nosEngine.LogI("WriteImage: Write requested");

		nos::NodeExecuteParams execParams(params->FunctionNodeExecuteParams);

		std::unique_lock<std::mutex> lock(Mutex);
		Path = nos::Utf8ToPath(std::string(execParams.GetPinValue<const char*>(NSN_Path)));
		IncludeAlpha = *execParams.GetPinValue<bool>(NSN_IncludeAlpha);
		assert(Event == 0);
		nosCmd cmd = sys::vulkan::BeginCmd(NOS_NAME("Write Image Copy To"), NodeId);
		auto inputTex = execParams.GetPinObject<sys::vulkan::Texture>(NSN_In);
		TempSrgbCopy = {};

		auto texInfo = *sys::vulkan::GetResourceInfo(inputTex);

		texInfo.Format = NOS_FORMAT_R8G8B8A8_SRGB;
		texInfo.Usage = nosImageUsage(NOS_IMAGE_USAGE_TRANSFER_SRC | NOS_IMAGE_USAGE_TRANSFER_DST);
		TempSrgbCopy = sys::vulkan::CreateTexture(texInfo, "TempSrgbCopy");

		nosVulkan->Copy(cmd, inputTex, *TempSrgbCopy, 0);
		nosCmdEndParams endParams{.ForceSubmit = true, .OutGPUEventHandle = &Event};
		nosVulkan->End(cmd, &endParams);
		Write = true;
		CV.notify_all();
	}

	void WriteImageToFile()
	{
		assert(TempSrgbCopy.has_value());
		std::string utf8Path = nos::PathToUtf8(this->Path);
		try
		{
			if (!std::filesystem::exists(this->Path.parent_path()))
				std::filesystem::create_directories(this->Path.parent_path());
		}
		catch (std::filesystem::filesystem_error& e)
		{
			nosEngine.LogE("WriteImage - %s: %s", utf8Path.c_str(), e.what());
			return;
		}
		nosEngine.LogI("WriteImage: Writing frame to file %s", utf8Path.c_str());

		auto tempSrgbInfo = *sys::vulkan::GetResourceInfo(*TempSrgbCopy);
		TypedObjectRef<sys::vulkan::Buffer> downloadBuf{};
		nosVulkan->Download(0, *TempSrgbCopy, &downloadBuf.GetStorage(), "TempSrgbCopy Download");
		if (!downloadBuf.IsValid())
		{
			nosEngine.LogE("WriteImage: Unable to download frame");
			return;
		}
		auto bufInfo = *sys::vulkan::GetResourceInfo(downloadBuf);

		if (auto buf2write = nosVulkan->Map(downloadBuf))
		{
			if (!IncludeAlpha)
				for (size_t i = 3; i < bufInfo.Size; i += 4)
					buf2write[i] = 0xff;

			if (!stbi_write_png(
					utf8Path.c_str(), tempSrgbInfo.Width, tempSrgbInfo.Height, 4, buf2write, tempSrgbInfo.Width * 4))
				nosEngine.LogE("WriteImage: Unable to write frame to file", "");
			else
				nosEngine.LogI("WriteImage: Wrote frame to file %s", utf8Path.c_str());
		}
		TempSrgbCopy = {};
	}

	static nosResult GetFunctions(size_t* count, nosName* names, nosPfnNodeFunctionExecute* fns)
	{
		*count = 1;
		if (!names || !fns)
			return NOS_RESULT_SUCCESS;

		*names = NOS_NAME_STATIC("WriteImage_Save");
		*fns = [](void* ctx, nosFunctionExecuteParams* params) {
			auto writeImage = (WriteImage*)ctx;
			writeImage->SendWriteRequest(params);
			return NOS_RESULT_SUCCESS;
		};

		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterWriteImage(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NSN_Nos_Utilities_WriteImage, WriteImage, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::mediaio