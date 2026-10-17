// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <atomic>
#include <cstdint>
#include <string_view>
namespace skyline::diagnostics {
class LegacyErrorBudget {
    std::atomic_flag guard = ATOMIC_FLAG_INIT;
    std::atomic<uint64_t> suppressed{};
    uint64_t window{}, used{};
public:
    // Reset only after closing producers. Take never waits and does no formatting.
    void Reset() noexcept { window = used = 0; suppressed = 0; guard.clear(std::memory_order_release); }
    bool Take(uint64_t second, uint64_t &priorSuppressed) noexcept {
        if (guard.test_and_set(std::memory_order_acquire)) { ++suppressed; return false; }
        if (second != window) { window = second; used = 0; }
        const bool allowed = used < 4;
        if (allowed) { ++used; priorSuppressed = suppressed.exchange(0); } else ++suppressed;
        guard.clear(std::memory_order_release);
        return allowed;
    }
};
// Only the owned AudioCore/Shader error adapters call this, before their existing message move.
void EmitLegacyError(std::string_view component, std::string_view message) noexcept;
}
