// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <charconv>
#include <limits>
#include "diagnostics.h"

namespace skyline::diagnostics {
struct ContentContext {
    std::string_view source;
    std::string_view index;
};

inline thread_local ContentContext contentContext;

// The synchronous loader call owns this context; no FD or user-selected URI is logged.
class ContentScope {
    ContentContext previous;
    std::array<char, std::numeric_limits<size_t>::digits10 + 1> indexBuffer;
public:
    ContentScope(std::string_view source, size_t index = 0) noexcept : previous(contentContext) {
        const auto end = std::to_chars(indexBuffer.data(), indexBuffer.data() + indexBuffer.size(), index).ptr;
        contentContext = {source, {indexBuffer.data(), static_cast<size_t>(end - indexBuffer.data())}};
    }
    ~ContentScope() { contentContext = previous; }
    ContentScope(const ContentScope &) = delete;
    ContentScope &operator=(const ContentScope &) = delete;
};

inline std::string_view ContentBasename(std::string_view name) noexcept {
    const auto separator = name.find_last_of("/\\");
    return separator == std::string_view::npos ? name : name.substr(separator + 1);
}

inline std::string ContentTitleId(std::uint64_t id) {
    constexpr char digits[]{"0123456789ABCDEF"};
    std::string result(16, '0');
    for (size_t i = result.size(); i > 0; --i, id >>= 4)
        result[i - 1] = digits[id & 0xF];
    return result;
}
}

// Argument formatting is diagnostic work too: allocation failures must never alter emulation.
#define PINE_DIAG_CONTENT_CATEGORY_LEVEL(category, level, event, ...) do { \
    if (::skyline::diagnostics::Enabled()) { \
        try { ::skyline::diagnostics::Emit(category, event, {__VA_ARGS__ __VA_OPT__(,) \
            {"loader_source", ::skyline::diagnostics::contentContext.source}, \
            {"loader_index", ::skyline::diagnostics::contentContext.index}}, level); } catch (...) {} \
    } \
} while (false)

#define PINE_DIAG_CONTENT_LEVEL(level, event, ...) PINE_DIAG_CONTENT_CATEGORY_LEVEL("content", level, event, __VA_ARGS__)
#define PINE_DIAG_CONTENT(event, ...) PINE_DIAG_CONTENT_LEVEL("INFO", event, __VA_ARGS__)
#define PINE_DIAG_CONTENT_ERROR(event, ...) PINE_DIAG_CONTENT_LEVEL("ERROR", event, __VA_ARGS__)
#define PINE_DIAG_STORAGE_LEVEL(level, event, ...) PINE_DIAG_CONTENT_CATEGORY_LEVEL("storage", level, event, __VA_ARGS__)
#define PINE_DIAG_STORAGE(event, ...) PINE_DIAG_STORAGE_LEVEL("INFO", event, __VA_ARGS__)
#define PINE_DIAG_STORAGE_ERROR(event, ...) PINE_DIAG_STORAGE_LEVEL("ERROR", event, __VA_ARGS__)
