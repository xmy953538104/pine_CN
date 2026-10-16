// SPDX-License-Identifier: MPL-2.0
#include <array>
#include <cassert>
#include "diagnostics/utf16.h"
int main() {
    using skyline::diagnostics::Utf16ToUtf8;
    constexpr std::array<uint16_t, 6> text{'A', 0, 0x4e2d, 0xd83d, 0xde00, 'Z'};
    assert(Utf16ToUtf8(text) == std::string("A\0\xe4\xb8\xad\xf0\x9f\x98\x80Z", 10));
    constexpr std::array<uint16_t, 3> invalid{0xd83d, '!', 0xde00};
    assert(Utf16ToUtf8(invalid) == "\xef\xbf\xbd!\xef\xbf\xbd");
    assert(Utf16ToUtf8({}).empty());
}
