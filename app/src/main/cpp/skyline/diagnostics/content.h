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
enum class ApplicationIdentityKind { Unknown, Application, Patch, AddOnContent };
inline std::string ResolveApplicationTitle(std::string_view /* legacyAssociation */, std::string_view program,
                                         std::string_view cnmt, std::string_view parent, ApplicationIdentityKind kind) {
    auto valid = [](std::string_view value) {
        return value.size() == 16 && value != "0000000000000000" && value.find_first_not_of("0123456789abcdefABCDEF") == std::string_view::npos;
    };
    // Only typed native metadata establishes this identity; an untyped UI/save-owner hint does not.
    if (kind == ApplicationIdentityKind::Application && valid(cnmt)) return std::string(cnmt);
    if ((kind == ApplicationIdentityKind::Patch || kind == ApplicationIdentityKind::AddOnContent) && valid(parent)) return std::string(parent);
    if (valid(program)) return std::string(program);
    return {};
}
struct ContentIdentity {
    std::string application, program, cnmt, parent, saveOwner, programContent, cnmtVersion, source;
};
template<class Loader> ContentIdentity DescribeContentIdentity(Loader &loader) {
    ContentIdentity identity;
    if (loader.programNca) identity.program = ContentTitleId(loader.programNca->header.titleId);
    if (loader.nacp) identity.saveOwner = loader.nacp->GetSaveDataOwnerId();
    identity.programContent = loader.diagnosticProgramContentId;
    auto kind = ApplicationIdentityKind::Unknown;
    if (loader.cnmt) {
        identity.cnmt = loader.cnmt->GetTitleId();
        identity.cnmtVersion = std::to_string(loader.cnmt->header.version);
        const auto type = static_cast<unsigned>(loader.cnmt->GetContentMetaType());
        if (type == 0x80) kind = ApplicationIdentityKind::Application;
        else if (type == 0x81 || type == 0x82) {
            kind = type == 0x81 ? ApplicationIdentityKind::Patch : ApplicationIdentityKind::AddOnContent;
            identity.parent = loader.cnmt->GetParentTitleId();
        }
    }
    identity.application = ResolveApplicationTitle({}, identity.program, identity.cnmt, identity.parent, kind);
    if (!identity.application.empty()) {
        if (kind == ApplicationIdentityKind::Application && identity.application == identity.cnmt) identity.source = "cnmt_application";
        else if (kind == ApplicationIdentityKind::Patch && identity.application == identity.parent) identity.source = "cnmt_patch_parent";
        else if (kind == ApplicationIdentityKind::AddOnContent && identity.application == identity.parent) identity.source = "cnmt_aoc_parent";
        else identity.source = "program_header";
    }
    return identity;
}
inline std::string NcaContentId(std::string_view name) {
    name = ContentBasename(name);
    if (name.size() != 36 || name.substr(32) != ".nca" || name.substr(0, 32).find_first_not_of("0123456789abcdefABCDEF") != std::string_view::npos) return {};
    std::string value(name.substr(0, 32));
    for (auto &c : value) if (c >= 'A' && c <= 'F') c += 'a' - 'A';
    return value;
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
