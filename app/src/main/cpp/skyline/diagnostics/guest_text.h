// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <string_view>
namespace skyline::diagnostics {
// Matches the existing SVC text/logger behavior; shared so protocol tests cross
// the actual production normalization boundary rather than a parallel fixture.
inline std::string_view StripGuestLogLineFeed(std::string_view text) noexcept {
    if (text.ends_with('\n')) text.remove_suffix(1);
    return text;
}
inline std::string_view GuestProtocolLine(std::string_view text) noexcept {
    text = StripGuestLogLineFeed(text);
    if (text.ends_with('\r')) text.remove_suffix(1);
    return text;
}
}
