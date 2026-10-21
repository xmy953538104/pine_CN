// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <cstdint>
#include <string_view>
#include <span>
#include <atomic>
#include <utility>

namespace skyline::diagnostics {
// Normal loader/bridge threads only. No signal or Guest callback use.
struct ModuleOwner {
    std::array<char, 129> session{}, launch{}, process{};
    uint64_t sinkGeneration{}, launchEpoch{}, runtimeOwnerEpoch{};
    bool operator==(const ModuleOwner &) const = default;
};
// Try-lock, bounded, no allocation. A token authenticates logging ownership only;
// it does not pin a Guest mapping, identify a Guest thread or serialize rtld.
bool TryModuleOwner(ModuleOwner &output) noexcept;
// Normal consumer only; rejects a stale sink/process/launch generation.
bool EmitForModuleOwner(const ModuleOwner &owner, std::string_view category, std::string_view event,
    std::span<const std::pair<std::string_view,std::string_view>> fields,
    const std::atomic<uint64_t> *admissionGate = nullptr, uint64_t expectedAdmission = 0) noexcept;
inline std::string_view OwnerText(const std::array<char, 129> &text) noexcept {
    size_t size{};
    while (size < text.size() && text[size]) ++size;
    return {text.data(), size};
}
}
