// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <dlfcn.h>
#include <adrenotools/driver.h>
#include "diagnostics/initialization.h"
#include "common/settings.h"
#include "common.h"
#include "nce.h"
#include "soc.h"
#include "gpu.h"
#include "audio.h"
#include "input.h"
#include "os.h"
#include "kernel/types/KProcess.h"

namespace skyline {
    static std::pair<PFN_vkGetInstanceProcAddr, void *> LoadVulkanDriver(kernel::OS &os, Settings &settings) {
        void *libvulkanHandle{};
        void *importHandle{};

        // If the user has selected a custom driver, try to load it
        if (!(*settings.gpuDriver).empty()) {
            libvulkanHandle = adrenotools_open_libvulkan(
                RTLD_NOW,
                ADRENOTOOLS_DRIVER_FILE_REDIRECT | ADRENOTOOLS_DRIVER_CUSTOM | ADRENOTOOLS_DRIVER_GPU_MAPPING_IMPORT,
                nullptr, // We require Android 10 so don't need to supply
                os.nativeLibraryPath.c_str(),
                (os.privateAppFilesPath + "gpu_drivers/" + *settings.gpuDriver + "/").c_str(),
                (*settings.gpuDriverLibraryName).c_str(),
                (os.publicAppFilesPath + "gpu/vk_file_redirect/").c_str(),
                &importHandle
            );

            if (!libvulkanHandle) {
                char *error = dlerror();
                LOGW("Failed to load custom Vulkan driver {}/{}: {}", *settings.gpuDriver, *settings.gpuDriverLibraryName, error ? error : "");
            }
        }

        if (!libvulkanHandle) {
            libvulkanHandle = adrenotools_open_libvulkan(
                RTLD_NOW,
                ADRENOTOOLS_DRIVER_FILE_REDIRECT | ADRENOTOOLS_DRIVER_GPU_MAPPING_IMPORT,
                nullptr, // We require Android 10 so don't need to supply
                os.nativeLibraryPath.c_str(),
                nullptr,
                nullptr,
                (os.publicAppFilesPath + "gpu/vk_file_redirect/").c_str(),
                &importHandle
            );

            if (!libvulkanHandle) {
                char *error = dlerror();
                LOGW("Failed to load builtin Vulkan driver: {}", error ? error : "");
            }

            if (!libvulkanHandle)
                libvulkanHandle = dlopen("libvulkan.so", RTLD_NOW);
        }

        return {reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(libvulkanHandle, "vkGetInstanceProcAddr")), importHandle};
    }

    DeviceState::DeviceState(kernel::OS *tOs, std::shared_ptr<JvmManager> jvmManager, std::shared_ptr<Settings> tSettings)
        : os(tOs), jvm(std::move(jvmManager)), settings(std::move(tSettings)) {          
        auto [vkGetInstanceProcAddr, adrenotoolsImportHandle]{diagnostics::ObserveInitialization("VULKAN_DRIVER_LOAD", [&] { return LoadVulkanDriver(*os, *settings); })};
        // We assign these later as they use the state in their constructor and we don't want null pointers
        gpu = diagnostics::ObserveInitialization("GPU_CONSTRUCT", [&] { return std::make_shared<gpu::GPU>(*this, vkGetInstanceProcAddr, adrenotoolsImportHandle); });
        soc = diagnostics::ObserveInitialization("SOC_CONSTRUCT", [&] { return std::make_shared<soc::SOC>(*this); });
        audio = diagnostics::ObserveInitialization("AUDIO_CONSTRUCT", [&] { return std::make_shared<audio::Audio>(*this); });
        input = diagnostics::ObserveInitialization("INPUT_CONSTRUCT", [&] { return std::make_shared<input::Input>(*this); });
    }

    DeviceState::~DeviceState() {
        if (process)
            process->ClearHandleTable();
  }
}
