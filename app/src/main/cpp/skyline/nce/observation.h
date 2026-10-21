// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstdint>
#include <span>
#include <array>
namespace skyline::nce {
struct ObservationPatch {
    uint32_t original{}, installed{};
    uint64_t callPc{}, stubPc{}, pltPc{};
    std::array<uint8_t,32> stubHash{};
};
// Only a synchronous load owner may supply these mutable spans. No live patch
// API; disarm closes gates and retains code/storage until producers quiesce.
bool InstallCallsiteObservation(std::span<uint8_t> text, uint64_t moduleBase, uint32_t callVa,
    uint32_t pltVa, uint32_t expectedBl, std::span<uint8_t> reservedStub, uint64_t stubPc,
    bool loadQuiescent, ObservationPatch &installed) noexcept;
bool RollbackCallsiteObservation(std::span<uint8_t> text, uint32_t callVa,
    std::span<const uint8_t> stub, bool loadQuiescent, const ObservationPatch &installed) noexcept;
}
