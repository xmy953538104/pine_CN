// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace skyline::diagnostics {
struct DfxMessage {
    bool recognized{}, valid{};
    std::string reason, event, probe, file, line, function, message, traceback, phase;
};
inline bool DfxUtf8(std::string_view value) noexcept {
    for (size_t i{}; i < value.size();) {
        auto b = static_cast<unsigned char>(value[i]);
        size_t n = b < 0x80 ? 1 : b >= 0xc2 && b <= 0xdf ? 2 : b >= 0xe0 && b <= 0xef ? 3 : b >= 0xf0 && b <= 0xf4 ? 4 : 0;
        if (!n || i + n > value.size()) return false;
        for (size_t j = 1; j < n; ++j) if ((static_cast<unsigned char>(value[i + j]) & 0xc0) != 0x80) return false;
        if (n >= 3) {
            auto next = static_cast<unsigned char>(value[i + 1]);
            if ((b == 0xe0 && next < 0xa0) || (b == 0xed && next >= 0xa0) ||
                (b == 0xf0 && next < 0x90) || (b == 0xf4 && next >= 0x90)) return false;
        }
        i += n;
    }
    return true;
}
inline DfxMessage ParseDfx1(std::string_view wire) {
    DfxMessage result;
    if (!wire.starts_with("DFX1|")) return result;
    result.recognized = true;
    const auto fail = [&](std::string_view reason) { result.reason = reason; return result; };
    if (wire.size() > 2048) return fail("wire_limit_2048_bytes");
    // A logging transport may add one physical line terminator. No leading
    // prefix, embedded extra record or arbitrary substring is accepted.
    if (wire.ends_with('\n')) { wire.remove_suffix(1); if (wire.ends_with('\r')) wire.remove_suffix(1); }
    wire.remove_prefix(5);
    std::array<std::string_view, 8> seen{};
    size_t count{}, decodedBytes{};
    const auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    while (!wire.empty()) {
        const auto delimiter = wire.find('|');
        const auto field = wire.substr(0, delimiter);
        const auto equals = field.find('=');
        if (equals == std::string_view::npos || count == seen.size()) return fail("field_syntax_or_count");
        const auto key = field.substr(0, equals);
        for (size_t i{}; i < count; ++i) if (seen[i] == key) return fail("duplicate_field");
        seen[count++] = key;
        std::string *target{}; size_t limit{};
        if (key == "event") { target = &result.event; limit = 32; }
        else if (key == "probe_id") { target = &result.probe; limit = 64; }
        else if (key == "file") { target = &result.file; limit = 192; }
        else if (key == "line") { target = &result.line; limit = 10; }
        else if (key == "function") { target = &result.function; limit = 96; }
        else if (key == "message") { target = &result.message; limit = 384; }
        else if (key == "traceback") { target = &result.traceback; limit = 768; }
        else if (key == "phase") { target = &result.phase; limit = 64; }
        else return fail("unknown_field");
        const auto encoded = field.substr(equals + 1);
        for (size_t i{}; i < encoded.size(); ++i) {
            auto value = static_cast<unsigned char>(encoded[i]);
            if (value == '%') {
                if (i + 2 >= encoded.size() || hex(encoded[i + 1]) < 0 || hex(encoded[i + 2]) < 0) return fail("invalid_percent_escape");
                value = static_cast<unsigned char>((hex(encoded[i + 1]) << 4) | hex(encoded[i + 2])); i += 2;
            } else if (value < 32 || value == 127) return fail("unescaped_control");
            if (!value || (value < 32 && value != '\n' && value != '\r' && value != '\t') || value == 127) return fail("invalid_control");
            if ((value < 32) && key != "message" && key != "traceback") return fail("control_in_identifier");
            if (target->size() == limit || ++decodedBytes > 1024) return fail("decoded_field_limit");
            *target += static_cast<char>(value);
        }
        if (!DfxUtf8(*target)) return fail("invalid_utf8");
        if (delimiter == std::string_view::npos) break;
        wire.remove_prefix(delimiter + 1);
        if (wire.empty()) return fail("empty_trailing_field");
    }
    if (result.event != "DFX_PROBE" && result.event != "DFX_MAIN_ENTER" && result.event != "DFX_MODINDEX_READY" &&
        result.event != "DFX_MODMAIN_OK" && result.event != "DFX_HAMLET_ENTER" && result.event != "DFX_LUA_ERROR") return fail("unknown_event");
    if (result.probe.empty() || result.probe.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != std::string::npos) return fail("invalid_probe_id");
    if (!result.line.empty()) {
        uint32_t number{};
        auto parsed = std::from_chars(result.line.data(), result.line.data() + result.line.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != result.line.data() + result.line.size() || !number || number > 2147483647) return fail("invalid_script_line");
    }
    result.valid = true; return result;
}
// Source guest messages remain separately bounded in GUEST_* records. Derived
// protocol records are limited to 16 per second/session, including rejections.
bool AllowGuestProtocol(uint64_t &priorSuppressed) noexcept;
}
