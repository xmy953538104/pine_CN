// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <atomic>
#include <cstdint>

namespace skyline::diagnostics::runtime {
// Normal diagnostics worker only; no Guest callback ever invokes formatting.
using Drain = bool (*)() noexcept;
inline std::atomic<Drain> drainCallback{};
// Never reset/reused; Guest slots compare the owner generation seeded at Init.
inline std::atomic<uint64_t> ownerEpoch{};
inline std::atomic<uint64_t> drainRequested{},drainCompleted{};
inline bool DrainOnWriter() noexcept {
    if (auto callback = drainCallback.load(std::memory_order_acquire)) return callback();
    return true;
}
}
