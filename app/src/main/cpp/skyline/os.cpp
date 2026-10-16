// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <diagnostics/content.h>
#include "gpu.h"
#include "common/settings.h"
#include "nce.h"
#include <jit/jit32.h>
#include "nce/guest.h"
#include "kernel/types/KProcess.h"
#include "vfs/os_backing.h"
#include "loader/nro.h"
#include "loader/nso.h"
#include "loader/nca.h"
#include "loader/nsp.h"
#include "loader/xci.h"
#include "os.h"

namespace skyline::kernel {
    OS::OS(
        std::shared_ptr<JvmManager> &jvmManager,
        std::shared_ptr<Settings> &settings,
        std::string publicAppFilesPath,
        std::string privateAppFilesPath,
        std::string nativeLibraryPath,
        std::string deviceTimeZone,
        std::shared_ptr<vfs::FileSystem> assetFileSystem)
        : nativeLibraryPath(std::move(nativeLibraryPath)),
          publicAppFilesPath(std::move(publicAppFilesPath)),
          privateAppFilesPath(std::move(privateAppFilesPath)),
          deviceTimeZone(std::move(deviceTimeZone)),
          assetFileSystem(std::move(assetFileSystem)),
          state(this, jvmManager, settings),
          serviceManager(state) {}
    
    bool isJitEnabled = false;
    
    void OS::Execute(int romFd, std::vector<int> dlcFds, int updateFd, loader::RomFormat romType) {
        auto romFile{std::make_shared<vfs::OsBacking>(romFd)};
        keyStore = std::make_shared<crypto::KeyStore>(privateAppFilesPath + "keys/");

        PINE_DIAG_CONTENT("CONTENT_LOAD_BEGIN", {"rom_format", fmt::format("{}", static_cast<int>(romType))}, {"update_fd_positive", updateFd > 0 ? "true" : "false"}, {"dlc_count", fmt::format("{}", dlcFds.size())});
        auto loadContent = [&](int fd, std::string_view source) {
            PINE_DIAG_CONTENT("LOADER_BEGIN", {"source", source});
            try {
                return GetLoader(fd, keyStore, romType);
            } catch (const std::exception &e) {
                PINE_DIAG_CONTENT_ERROR("LOADER_FAILED", {"source", source}, {"reason", e.what()}, {"last_stage", diagnostics::LastStage()});
                throw;
            }
        };
        state.loader = loadContent(romFd, "base");
        diagnostics::Stage("base_loader_ready");
        PINE_DIAG_CONTENT("BASE_SELECTED", {"source", "base"}, {"title_id", state.loader->cnmt ? state.loader->cnmt->GetTitleId() : "unknown"});

        if (updateFd > 0) {
            state.updateLoader = loadContent(updateFd, "update");
            diagnostics::Stage("update_loader_ready");
            PINE_DIAG_CONTENT("PATCH_SELECTED", {"source", "update"}, {"selection", "provided_fd_gt_zero"}, {"rom_format_source", "base"}, {"title_id", state.updateLoader->cnmt ? state.updateLoader->cnmt->GetTitleId() : "unknown"}, {"version", state.updateLoader->cnmt ? fmt::format("{}", state.updateLoader->cnmt->header.version) : "unknown"});
        } else {
            PINE_DIAG_CONTENT("PATCH_SKIPPED", {"reason", "update_fd_not_positive"});
        }

        if (dlcFds.size() > 0)
            for (int fd : dlcFds) {
                PINE_DIAG_CONTENT("DLC_DISCOVERED", {"source", "provided_fd"}, {"rom_format_source", "base"});
                state.dlcLoaders.push_back(loadContent(fd, "dlc"));
                auto &dlc = state.dlcLoaders.back();
                PINE_DIAG_CONTENT("DLC_REGISTERED", {"title_id", dlc->cnmt ? dlc->cnmt->GetTitleId() : "unknown"}, {"parent_title_id", dlc->cnmt ? dlc->cnmt->GetParentTitleId() : "unknown"}, {"cnmt_type", dlc->cnmt ? fmt::format("{}", static_cast<int>(dlc->cnmt->header.contentMetaType)) : "unknown"});
            }

        state.gpu->Initialise();
        diagnostics::Stage("gpu_initialized");

        auto &process{state.process};
        process = std::make_shared<kernel::type::KProcess>(state);

        auto entry{state.loader->LoadProcessData(process, state)};
        diagnostics::Stage("process_data_loaded");
        auto &nacp{state.loader->nacp};
        if (nacp) {
            std::string name{nacp->GetApplicationName(language::ApplicationLanguage::AmericanEnglish)}, publisher{nacp->GetApplicationPublisher(language::ApplicationLanguage::AmericanEnglish)};
            if (name.empty())
                name = nacp->GetApplicationName(nacp->GetFirstSupportedTitleLanguage());
            if (publisher.empty())
                publisher = nacp->GetApplicationPublisher(nacp->GetFirstSupportedTitleLanguage());

            if (state.updateLoader)
                LOGINF("Applied update v{}", state.updateLoader->nacp->GetApplicationVersion());

            if (state.dlcLoaders.size() > 0)
                for (auto &loader : state.dlcLoaders)
                    LOGINF("Applied DLC {}", loader->cnmt->GetTitleId());

            LOGINF(R"(Starting "{}" ({}) v{} by "{}")", name, nacp->GetSaveDataOwnerId(), state.updateLoader ? state.updateLoader->nacp->GetApplicationVersion() : nacp->GetApplicationVersion(), publisher);
        }

        // Scheduler retrieves information from the NPDM of the process so it needs to be initialized after the process is created
        state.scheduler = std::make_shared<kernel::Scheduler>(state);
        
        skyline::gpu::isSampleShadingEnabled = *state.settings->enableSampleShading;
        if (!isJitEnabled) {
            state.nce = std::make_shared<nce::NCE>(state);
        } else { // 32-bit
            state.jit32 = std::make_shared<jit::Jit32>(state);
        }

        process->InitializeHeapTls();
        auto thread{process->CreateThread(entry)};
        if (thread) {
            LOGI("Starting main HOS thread");
            thread->Start(true);
            process->Kill(true, true, true);
        }
    }

    std::shared_ptr<loader::Loader> OS::GetLoader(int fd, std::shared_ptr<crypto::KeyStore> keyStore, loader::RomFormat romType) {
        auto file{std::make_shared<vfs::OsBacking>(fd)};
        switch (romType) {
            case loader::RomFormat::NRO:
                return std::make_shared<loader::NroLoader>(std::move(file));
            case loader::RomFormat::NSO:
                return std::make_shared<loader::NsoLoader>(std::move(file));
            case loader::RomFormat::NCA:
                return std::make_shared<loader::NcaLoader>(std::move(file), std::move(keyStore));
            case loader::RomFormat::NSP:
                return std::make_shared<loader::NspLoader>(file, keyStore);
            case loader::RomFormat::XCI:
                return std::make_shared<loader::XciLoader>(file, keyStore);
            default:
                throw exception("Unsupported ROM extension.");
        }
    }
}
