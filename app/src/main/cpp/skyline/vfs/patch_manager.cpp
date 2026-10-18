// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)

#include <diagnostics/content.h>
#include <os.h>
#include <vfs/nca.h>
#include "patch_manager.h"
#include "region_backing.h"

namespace skyline::vfs {
    PatchManager::PatchManager() {}

    std::shared_ptr<FileSystem> PatchManager::PatchExeFS(const DeviceState &state, std::shared_ptr<FileSystem> exefs) {
        auto updateProgramNCA{state.updateLoader->programNca};
        try { diagnostics::SetEffectiveExeFs(updateProgramNCA ? diagnostics::ContentTitleId(updateProgramNCA->header.titleId) : "",
            state.updateLoader->diagnosticProgramContentId, updateProgramNCA && updateProgramNCA->exeFs ? "update" : "update_missing_exefs"); } catch (...) {}
        PINE_DIAG_CONTENT_LEVEL(updateProgramNCA && updateProgramNCA->exeFs ? "INFO" : "ERROR",
            updateProgramNCA && updateProgramNCA->exeFs ? "PATCH_EXEFS_SELECTED" : "PATCH_EXEFS_MISSING",
            {"source", "update"}, {"title_id", updateProgramNCA ? diagnostics::ContentTitleId(updateProgramNCA->header.titleId) : "unknown"},
            {"effective_exefs_program_nca", state.updateLoader->diagnosticProgramContentId},
            {"base_program_nca", state.loader->diagnosticProgramContentId}, {"observation_layer", "patch_manager_returned_exefs"});
        return updateProgramNCA->exeFs;
    }

    std::shared_ptr<vfs::Backing> PatchManager::PatchRomFS(const DeviceState &state, std::optional<vfs::NCA> nca, u64 ivfcOffset) {
        PINE_DIAG_CONTENT("PATCH_ROMFS_BEGIN", {"title_id", nca ? diagnostics::ContentTitleId(nca->header.titleId) : "unknown"});
        try {
            auto newNca{std::make_shared<vfs::NCA>(nca, state.os->keyStore, state.loader->programNca->romFs, ivfcOffset)};
            PINE_DIAG_CONTENT_LEVEL(newNca->romFs ? "INFO" : "ERROR", newNca->romFs ? "PATCH_ROMFS_OPENED" : "PATCH_ROMFS_FAILED",
                {"title_id", diagnostics::ContentTitleId(newNca->header.titleId)}, {"status", newNca->romFs ? "ok" : "missing"},
                {"path_event", "ROMFS_BACKING_OPENED"});
            return newNca->romFs;
        } catch (const std::exception &e) {
            PINE_DIAG_CONTENT_ERROR("PATCH_ROMFS_FAILED", {"title_id", nca ? diagnostics::ContentTitleId(nca->header.titleId) : "unknown"}, {"reason", e.what()}, {"status", "exception"});
            throw;
        }
    }
}
