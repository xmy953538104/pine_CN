// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <catalog/catalog.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace skyline::loader {
    struct SelectedLaunchProgram {
        std::string groupId, contentId;
        uint64_t programId{};
        uint8_t programIndex{};
    };
    struct SelectedLaunchRequest {
        bool standalone{};
        catalog::ContentInfoProfile profile{catalog::ContentInfoProfile::Unknown};
        uint64_t applicationId{};
        SelectedLaunchProgram primary;
        std::optional<SelectedLaunchProgram> update;
        std::vector<std::string> addOnGroups;
    };

    // Pure bounded wire validation only: no source I/O, role/byte validation,
    // implicit content selection, or game/runtime capability claim.
    // Throws catalog::Error with fixed, input-free codes/messages.
    SelectedLaunchRequest ParseSelectedLaunchFields(const std::vector<std::string> &fields,
                                                   const std::vector<int32_t> &indices);
}
