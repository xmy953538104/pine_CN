// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <cstddef>
#include <string_view>

namespace skyline::diagnostics {
inline bool CriticalEvent(std::string_view category, std::string_view event, std::string_view level) noexcept {
    if (category == "resource") return event == "RESOURCE_POST_PROGRESS_NO_NEW_PRESENT" || event == "RESOURCE_CAPABILITIES";
    if (category == "exception") return true;
    if (category == "guest") return event == "DFX_PROBE" || event == "DFX_MAIN_ENTER" || event == "DFX_MODINDEX_READY" ||
        event == "DFX_MODMAIN_OK" || event == "DFX_HAMLET_ENTER" || event == "DFX_LUA_ERROR";
    if (category == "lifecycle") return event == "STAGE_COMPLETED" || event == "STAGE_STARTED" ||
        event == "LAUNCH_ENDED" || event == "LAUNCH_STOP_REQUESTED" || event == "LAUNCH_PROGRESS" || event == "FIRST_PRESENT_OBSERVED" ||
        event == "LAUNCH_HOST_EXIT_REQUESTED" || event == "HOST_EXIT_REQUEST_ACCEPTED" ||
        event == "SESSION_METADATA" || event == "LAUNCH_BEGIN" || event == "LAUNCH_CONTEXT_OBSERVED" ||
        event == "PREVIOUS_PROCESS_CRASH" || event == "PREVIOUS_PROCESS_EXIT" || event == "PREVIOUS_NATIVE_TRACE";
    if ((category == "content" || category == "storage" || category == "audio" || category == "gpu") && level == "ERROR") return true;
    return event == "CONTENT_ROLE_OBSERVED" || event == "BASE_SELECTED" || event == "PATCH_SELECTED" || event == "NCA_FINAL_SELECTION" ||
        event == "DLC_REGISTERED" || event == "DLC_STORAGE_OPENED" || event == "PATCH_EXEFS_SELECTED";
}
inline size_t AdmissionLimit(size_t capacity, bool critical) noexcept {
    const auto reserved = capacity < 8 ? 0 : std::min<size_t>(64, capacity / 8);
    return critical ? capacity : capacity - reserved;
}
}
