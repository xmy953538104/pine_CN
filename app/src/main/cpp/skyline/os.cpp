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
        auto loadContent = [&](int fd, std::string_view source, size_t index = 0) {
            diagnostics::ContentScope scope(source, index);
            PINE_DIAG_CONTENT("LOADER_BEGIN", {"source", source});
            try {
                auto loader{GetLoader(fd, keyStore, romType)};
                PINE_DIAG_CONTENT("LOADER_READY", {"source", source},
                    {"title_id", loader->cnmt ? loader->cnmt->GetTitleId() : "unknown"},
                    {"cnmt_version", loader->cnmt ? std::to_string(loader->cnmt->header.version) : "unknown"},
                    {"display_version", loader->nacp ? loader->nacp->GetApplicationVersion() : "unknown"},
                    {"cnmt_type", loader->cnmt ? std::to_string(static_cast<int>(loader->cnmt->header.contentMetaType)) : "unknown"},
                    {"program_id", loader->programNca ? diagnostics::ContentTitleId(loader->programNca->header.titleId) : "unknown"},
                    {"program_nca_id", loader->diagnosticProgramContentId}, {"full_content_hash_status", "not_computed"},
                    {"full_content_hash_algorithm", ""}, {"full_content_hash_scope", "input_document_bytes"});
                PINE_DIAG_CONTENT("CONTENT_ROLE_OBSERVED", {"slot_role", source},
                    {"cnmt_role", !loader->cnmt ? "unknown" : loader->cnmt->GetContentMetaType() == vfs::ContentMetaType::Application ? "application" :
                        loader->cnmt->GetContentMetaType() == vfs::ContentMetaType::Patch ? "patch" : loader->cnmt->GetContentMetaType() == vfs::ContentMetaType::AddOnContent ? "add_on_content" : "other"},
                    {"title_id", loader->cnmt ? loader->cnmt->GetTitleId() : "unknown"}, {"validation", "observation_only_no_policy_change"});
                return loader;
            } catch (const std::exception &e) {
                PINE_DIAG_CONTENT_ERROR("LOADER_FAILED", {"source", source}, {"reason", e.what()}, {"last_stage", diagnostics::LastStage()});
                throw;
            }
        };
        state.loader = loadContent(romFd, "base");
        // Remember the selected base even when logging is off so live enabling
        // can correlate the running game without loading its metadata again.
        try {
            const auto program = state.loader->programNca ? diagnostics::ContentTitleId(state.loader->programNca->header.titleId) : std::string{};
            const auto cnmt = state.loader->cnmt ? state.loader->cnmt->GetTitleId() : std::string{};
            auto kind = diagnostics::ApplicationIdentityKind::Unknown;
            std::string parent;
            if (state.loader->cnmt) {
                if (state.loader->cnmt->GetContentMetaType() == vfs::ContentMetaType::Application) kind = diagnostics::ApplicationIdentityKind::Application;
                if (state.loader->cnmt->GetContentMetaType() == vfs::ContentMetaType::Patch) {
                    kind = diagnostics::ApplicationIdentityKind::Patch;
                    parent = state.loader->cnmt->GetParentTitleId();
                }
            }
            diagnostics::SetContentIdentity(diagnostics::ResolveApplicationTitle(diagnostics::ApplicationTitleId(), program, cnmt, parent, kind),
                program, cnmt, state.loader->diagnosticProgramContentId);
        } catch (...) {}
        diagnostics::Stage("base_loader_ready");
        PINE_DIAG_CONTENT("BASE_SELECTED", {"source", "base"}, {"program_id", state.loader->programNca ? diagnostics::ContentTitleId(state.loader->programNca->header.titleId) : "unknown"},
            {"program_nca_id", state.loader->diagnosticProgramContentId}, {"title_id", state.loader->cnmt ? state.loader->cnmt->GetTitleId() : "unknown"},
            {"cnmt_version", state.loader->cnmt ? std::to_string(state.loader->cnmt->header.version) : "unknown"},
            {"display_version", state.loader->nacp ? state.loader->nacp->GetApplicationVersion() : "unknown"});

        if (updateFd > 0) {
            state.updateLoader = loadContent(updateFd, "update");
            diagnostics::Stage("update_loader_ready");
            PINE_DIAG_CONTENT("PATCH_SELECTED", {"source", "update"}, {"program_id", state.updateLoader->programNca ? diagnostics::ContentTitleId(state.updateLoader->programNca->header.titleId) : "unknown"},
                {"program_nca_id", state.updateLoader->diagnosticProgramContentId}, {"selection", "provided_fd_gt_zero"}, {"rom_format_source", "base"},
                {"title_id", state.updateLoader->cnmt ? state.updateLoader->cnmt->GetTitleId() : "unknown"},
                {"parent_title_id", state.updateLoader->cnmt && state.updateLoader->cnmt->GetContentMetaType() == vfs::ContentMetaType::Patch ? state.updateLoader->cnmt->GetParentTitleId() : "unknown"},
                {"cnmt_version", state.updateLoader->cnmt ? std::to_string(state.updateLoader->cnmt->header.version) : "unknown"},
                {"display_version", state.updateLoader->nacp ? state.updateLoader->nacp->GetApplicationVersion() : "unknown"});
        } else {
            PINE_DIAG_CONTENT("PATCH_SKIPPED", {"reason", "update_fd_not_positive"});
        }

        if (dlcFds.size() > 0)
            for (int fd : dlcFds) {
                auto index{state.dlcLoaders.size()};
                diagnostics::ContentScope scope("dlc", index);
                PINE_DIAG_CONTENT("DLC_DISCOVERED", {"source", "provided_fd"}, {"rom_format_source", "base"});
                state.dlcLoaders.push_back(loadContent(fd, "dlc", index));
                auto &dlc = state.dlcLoaders.back();
                PINE_DIAG_CONTENT("DLC_REGISTERED", {"source", "dlc"}, {"title_id", dlc->cnmt ? dlc->cnmt->GetTitleId() : "unknown"},
                    {"parent_title_id", dlc->cnmt && dlc->cnmt->GetContentMetaType() == vfs::ContentMetaType::AddOnContent ? dlc->cnmt->GetParentTitleId() : "unknown"},
                    {"cnmt_type", dlc->cnmt ? std::to_string(static_cast<int>(dlc->cnmt->header.contentMetaType)) : "unknown"},
                    {"cnmt_version", dlc->cnmt ? std::to_string(dlc->cnmt->header.version) : "unknown"});
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

            PINE_DIAG_CONTENT("APPLICATION_METADATA", {"source", "base_control"}, {"title_name", name},
                {"title_id", state.loader->cnmt ? state.loader->cnmt->GetTitleId() : "unknown"},
                {"save_data_owner_id", nacp->GetSaveDataOwnerId()},
                {"base_display_version", nacp->GetApplicationVersion()},
                {"update_display_version", state.updateLoader && state.updateLoader->nacp ? state.updateLoader->nacp->GetApplicationVersion() : "unknown"});

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
