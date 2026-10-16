// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace skyline::diagnostics {
    // JNI GetStringUTFChars uses Modified UTF-8 (including encoded surrogate
    // pairs). Convert UTF-16 explicitly so emoji/NUL remain valid JSON text.
    inline std::string Utf16ToUtf8(std::span<const uint16_t> units) {
        std::string result;
        result.reserve(units.size());
        for (size_t i{}; i < units.size(); ++i) {
            uint32_t cp{units[i]};
            if (cp >= 0xd800 && cp <= 0xdbff) {
                if (i + 1 < units.size() && units[i + 1] >= 0xdc00 && units[i + 1] <= 0xdfff) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (units[++i] - 0xdc00);
                } else cp = 0xfffd;
            } else if (cp >= 0xdc00 && cp <= 0xdfff) cp = 0xfffd;
            if (cp < 0x80) result += static_cast<char>(cp);
            else if (cp < 0x800) {
                result += static_cast<char>(0xc0 | (cp >> 6));
                result += static_cast<char>(0x80 | (cp & 0x3f));
            } else if (cp < 0x10000) {
                result += static_cast<char>(0xe0 | (cp >> 12));
                result += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
                result += static_cast<char>(0x80 | (cp & 0x3f));
            } else {
                result += static_cast<char>(0xf0 | (cp >> 18));
                result += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
                result += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
                result += static_cast<char>(0x80 | (cp & 0x3f));
            }
        }
        return result;
    }
}
