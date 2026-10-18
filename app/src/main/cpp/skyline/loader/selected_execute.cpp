// SPDX-License-Identifier: MPL-2.0
#include "selected_content.h"
#include <os.h>
#include <diagnostics/content.h>

namespace skyline::kernel {
    void OS::ExecuteSelectedContent(const catalog::Catalog &catalog, const loader::SelectionRequest &request,
                                    std::shared_ptr<crypto::KeyStore> providedKeys) {
        auto selected = loader::PrepareSelectedContent(catalog, request, std::move(providedKeys));
        selected->Install(state);
        try {
            const auto identity = diagnostics::DescribeContentIdentity(*state.loader);
            diagnostics::SetContentIdentity(identity.application, identity.program, identity.cnmt, identity.programContent, identity.source);
        } catch (...) {}
        diagnostics::Stage("selected_primary_loader_ready");
        auto describe = [&](const std::shared_ptr<loader::Loader> &content, const char *slot) {
            PINE_DIAG_CONTENT("SELECTED_CONTENT_INSTALLED", {"slot_role", slot},
                {"cnmt_role", content->cnmt->GetContentMetaType() == vfs::ContentMetaType::Application ? "application" :
                              content->cnmt->GetContentMetaType() == vfs::ContentMetaType::Patch ? "patch" : "add_on_content"},
                {"cnmt_id", content->cnmt->GetTitleId()}, {"program_id", content->programNca ? diagnostics::ContentTitleId(content->programNca->header.titleId) : ""},
                {"program_content_id", content->diagnosticProgramContentId}, {"validation", "selected_bounded_structural_v1"});
        };
        describe(state.loader, request.mode == loader::SelectedMode::StandaloneRebuiltPatch ? "standalone_patch" : "primary_application");
        if (state.updateLoader) { describe(state.updateLoader, "update"); diagnostics::Stage("selected_update_loader_ready"); }
        for (const auto &aoc : state.dlcLoaders) {
            describe(aoc, "add_on_content");
            PINE_DIAG_CONTENT("DLC_REGISTERED", {"source", "explicit_selected_group"}, {"title_id", aoc->cnmt->GetTitleId()},
                              {"parent_title_id", aoc->cnmt->GetParentTitleId()}, {"cnmt_type", "130"}, {"validation", "selected_bounded_structural_v1"});
        }
        ExecuteLoadedContent(request.mode == loader::SelectedMode::StandaloneRebuiltPatch ? "selected_standalone_patch_control" : "selected_primary_control");
    }
}
