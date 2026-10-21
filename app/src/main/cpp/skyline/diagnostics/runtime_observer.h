// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "nso_image.h"
#include <atomic>
#include <memory>

namespace skyline::diagnostics::runtime {
enum class Domain { Guest, ControlledSynthetic };
enum class StopReason : uint64_t { None, Disabled, IdentityMismatch, LateLoad, MappingChanged, ProcessExit, OwnerChanged, Capacity, SafetyGateClosed };
struct Profile {
    image::ExpectedPreimage preimage;
    std::array<image::WindowRequest, image::MaxWindows> requests{};
    std::array<std::pair<uint32_t,uint32_t>,3> segments{};
    uint32_t callVa{}, pltVa{}, noCallbackVa{}, insufficientVa{};
};
const Profile &SdkProfile() noexcept;
// Explicit module_runtime category plus verbose diagnostics; never enabled by
// an empty/all category filter. This enables readonly identity, NOT interception.
void ConfigureIdentity(bool enabled) noexcept;
bool IdentityRequested() noexcept;
struct Module;
class ProcessObservation {
    friend class ScopedLoad;
    std::shared_ptr<Module> storage; // owns one immutable launch allocation; never reused
    std::atomic<Module *> published{};
    std::atomic<bool> loadsSealed{}, closed{};
    const Profile *profile;
    Domain domain{Domain::Guest};
public:
    ProcessObservation() noexcept;
#ifdef PINE_DIAG_RUNTIME_TEST
    ProcessObservation(const Profile &profile, Domain domain) noexcept;
#endif
    ~ProcessObservation();
    ProcessObservation(const ProcessObservation &) = delete;
    void SealLoads() noexcept { loadsSealed.store(true, std::memory_order_release); }
    void Invalidate(StopReason reason) noexcept;
    void BeforeMappingMutation(uint64_t start, uint64_t size) noexcept;
    bool Validated() const noexcept;
    // No runtime address read: owned metadata only.
    std::shared_ptr<Module> Binding() const noexcept { return storage; }
};
// Only exists on the synchronous NsoLoader -> LoadExecutable call stack. It
// carries no symbols, URI, FD or borrowed bytes past that stack's lifetime.
class ScopedLoad {
    ProcessObservation *process{};
    const image::Record *input{};
    ScopedLoad *previous{};
    bool observed{};
public:
    ScopedLoad(ProcessObservation &process, const image::Record *input) noexcept;
    ~ScopedLoad();
    static bool Active() noexcept;
    static void Mapped(std::span<const uint8_t> text, std::span<const uint8_t> ro,
                       uint64_t moduleBase, uint64_t reservationBase, size_t reservationSize,
                       bool instructionCachePublished) noexcept;
};
struct Module {
    image::Record input;
    std::array<image::Window,image::MaxWindows> loadedWindows;
    uint64_t moduleBase{}, textSize{}, roBase{}, roSize{}, optionEpoch{};
    Domain domain{};
    std::atomic<uint64_t> active{}, stopReason{}, publishedReady{};
    bool cachePublished{}, identityExported{};
    // Post-NCE/pre-observer loaded copies, never a retained host mapping.
};
}
