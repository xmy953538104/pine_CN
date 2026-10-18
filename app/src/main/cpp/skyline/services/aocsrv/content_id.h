// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstdint>

namespace skyline::service::aocsrv::constant {
    // Shared with the existing AOC ListAddOnContent projection. This does not
    // establish a parent association; selected CNMT must prove that separately.
    constexpr std::uint64_t AOCTitleIdMask{0x7FF};
}
