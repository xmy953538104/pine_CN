// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string_view>
#include "diagnostics.h"

namespace skyline::diagnostics {
    // A marker must be the first token of a real guest message, never a substring
    // inferred from emulator activity. This deliberately accepts no arbitrary event names.
    inline std::string_view GuestMarker(std::string_view message) noexcept {
        constexpr std::string_view markers[]{"DFX_MAIN_ENTER", "DFX_MODINDEX_READY", "DFX_MODMAIN_OK", "DFX_HAMLET_ENTER", "DFX_LUA_ERROR"};
        for (auto marker : markers) {
            if (message.substr(0, marker.size()) != marker)
                continue;
            if (message.size() == marker.size())
                return marker;
            const char delimiter{message[marker.size()]};
            if (delimiter == ' ' || delimiter == ':' || delimiter == '\t' || delimiter == '\r' || delimiter == '\n')
                return marker;
        }
        return {};
    }

    inline void GuestMessage(std::string_view message, std::string_view source,
                             std::string_view level = "INFO", std::string_view filename = {},
                             std::string_view line = {}, std::string_view function = {},
                             std::string_view thread = {}) noexcept {
        if (!Enabled())
            return;
        Emit("guest", level == "ERROR" ? "GUEST_RUNTIME_ERROR" : "GUEST_DEBUG_MESSAGE",
             {{"source", source}, {"message", message}, {"guest_file", filename},
              {"guest_line", line}, {"guest_function", function}, {"guest_thread", thread}}, level);
        const auto marker{GuestMarker(message)};
        if (!marker.empty())
            Emit("guest", marker, {{"source", source}, {"message", message}, {"guest_file", filename},
                 {"guest_line", line}, {"guest_function", function}, {"guest_thread", thread}},
                 marker == "DFX_LUA_ERROR" ? "ERROR" : level);
    }
}
