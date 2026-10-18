// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string_view>
#include <charconv>
#include "diagnostics.h"
#include "dfx_protocol.h"

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
        char size[32]{};
        const auto end = std::to_chars(size, size + sizeof(size), message.size()).ptr;
        Emit("guest", level == "ERROR" ? "GUEST_RUNTIME_ERROR" : "GUEST_DEBUG_MESSAGE",
             {{"source", source}, {"message", message}, {"guest_file", filename},
              {"guest_line", line}, {"guest_function", function}, {"guest_thread", thread},
              {"guest_message_original_bytes", std::string_view(size, end - size)},
              {"guest_message_truncated", message.size() > 2048 ? "true" : "false"}}, level);
        const auto marker{GuestMarker(message)};
        uint64_t priorSuppressed{};
        if (!marker.empty() && AllowGuestProtocol(priorSuppressed))
            Emit("guest", marker, {{"source", source}, {"message", message}, {"guest_file", filename},
                 {"guest_line", line}, {"guest_function", function}, {"guest_thread", thread}, {"bridge_protocol", "legacy_strict_first_token"}},
                 marker == "DFX_LUA_ERROR" ? "ERROR" : level);
        if (!message.starts_with("DFX1|") || !AllowGuestProtocol(priorSuppressed)) return;
        try {
            const auto parsed = ParseDfx1(message);
            const auto suppressed = std::to_string(priorSuppressed);
            if (!parsed.valid) {
                Emit("guest", "DFX_PROTOCOL_REJECTED", {{"source", source}, {"message", message}, {"bridge_protocol", "DFX1"},
                    {"reason", parsed.reason}, {"protocol_prior_rate_suppressed", suppressed}}, "WARN");
                return;
            }
            Emit("guest", parsed.event, {{"source", source}, {"message", message},
                {"bridge_protocol", "DFX1"}, {"probe_id", parsed.probe}, {"protocol_validation", "ok"},
                {"script_fields_source", "guest_DFX1_payload"}, {"protocol_prior_rate_suppressed", suppressed},
                {"protocol_rate_policy", "first_16_messages_per_second_per_session"},
                {"game_script_file", parsed.file}, {"game_script_line", parsed.line}, {"game_script_function", parsed.function},
                {"game_message", parsed.message}, {"game_traceback", parsed.traceback}, {"game_phase", parsed.phase},
                {"guest_file", filename.substr(0, 192)}, {"guest_line", line.substr(0, 16)},
                {"guest_function", function.substr(0, 96)}, {"guest_thread", thread.substr(0, 64)}},
                parsed.event == "DFX_LUA_ERROR" ? "ERROR" : level);
        } catch (...) {} // Diagnostic parsing never changes the guest call/error path.
    }
}
