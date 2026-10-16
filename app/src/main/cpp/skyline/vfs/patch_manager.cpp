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
        PINE_DIAG_CONTENT("PATCH_EXEFS_SELECTED", {"source", "update"}, {"title_id", fmt::format("{:016X}", updateProgramNCA->header.titleId)});
        return updateProgramNCA->exeFs;
    }

    std::shared_ptr<vfs::Backing> PatchManager::PatchRomFS(const DeviceState &state, std::optional<vfs::NCA> nca, u64 ivfcOffset) {
        PINE_DIAG_CONTENT("PATCH_ROMFS_BEGIN", {"title_id", nca ? fmt::format("{:016X}", nca->header.titleId) : "unknown"});
        try {
            auto newNca{std::make_shared<vfs::NCA>(nca, state.os->keyStore, state.loader->programNca->romFs, ivfcOffset)};
            PINE_DIAG_CONTENT("PATCH_ROMFS_OPENED", {"title_id", fmt::format("{:016X}", newNca->header.titleId)}, {"status", newNca->romFs ? "ok" : "missing"}, {"path_evidence", "ROMFS_BACKING_OPENED"});
            return newNca->romFs;
        } catch (const std::exception &e) {
            PINE_DIAG_CONTENT_ERROR("PATCH_ROMFS_FAILED", {"reason", e.what()});
            throw;
        }
    }
}
