// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <exception>
#include <diagnostics/content.h>

namespace skyline::loader {
// One iteration of the NSP NCA scan. Keep the existing eligibility, overwrite and
// exception policy together so diagnostics cannot introduce a second selection path.
template<class LoaderException, class MakeCandidate, class NcaSlot>
void LoadNspContent(std::string_view name, MakeCandidate &&makeCandidate,
                    NcaSlot &program, NcaSlot &control, NcaSlot &meta, NcaSlot &publicData) {
    try {
        auto nca{makeCandidate()};
        using ContentType = decltype(nca.contentType);
        PINE_DIAG_CONTENT("NCA_CANDIDATE", {"content_name", diagnostics::ContentBasename(name)},
            {"nca_type", std::to_string(static_cast<int>(nca.contentType))}, {"title_id", diagnostics::ContentTitleId(nca.header.titleId)});
        auto select = [&](auto &slot) {
            const bool replaced{slot.has_value()};
            const auto previousTitleId{slot ? slot->header.titleId : 0};
            slot = std::move(nca);
            PINE_DIAG_CONTENT(replaced ? "NCA_OVERWRITTEN" : "NCA_SELECTED", {"content_name", diagnostics::ContentBasename(name)},
                {"nca_type", std::to_string(static_cast<int>(slot->contentType))}, {"title_id", diagnostics::ContentTitleId(slot->header.titleId)},
                {"replaced_title_id", replaced ? diagnostics::ContentTitleId(previousTitleId) : "unknown"}, {"policy", "last_eligible_of_type"});
        };
        if (nca.contentType == ContentType::Program && nca.romFs != nullptr && nca.exeFs != nullptr)
            select(program);
        else if (nca.contentType == ContentType::Control && nca.romFs != nullptr)
            select(control);
        else if (nca.contentType == ContentType::Meta)
            select(meta);
        else if (nca.contentType == ContentType::PublicData)
            select(publicData);
        else
            PINE_DIAG_CONTENT("NCA_SKIPPED", {"content_name", diagnostics::ContentBasename(name)}, {"reason", "ineligible_type_or_sections"});
    } catch (const LoaderException &e) {
        PINE_DIAG_CONTENT_ERROR("NCA_LOAD_FAILED", {"content_name", diagnostics::ContentBasename(name)}, {"exception", "loader_exception"},
            {"error_code", std::to_string(static_cast<int>(e.error))}, {"action", "propagate"});
        throw LoaderException(e.error);
    } catch (const std::exception &e) {
        PINE_DIAG_CONTENT_ERROR("NCA_LOAD_FAILED", {"content_name", diagnostics::ContentBasename(name)}, {"exception", "std_exception"},
            {"reason", e.what()}, {"action", "continue"});
    }
}
}
