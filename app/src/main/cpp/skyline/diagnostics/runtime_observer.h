// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "nso_image.h"
#include "guest_slot.h"
#include "../nce/observation.h"
#include <mutex>
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
    uint32_t originalBl{0x94044cc0};
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
    std::atomic<uint64_t> admission{}; // 0 loading,1 admitted,2 irreversibly closed
    std::mutex loadMutex;
    const Profile *profile;
    Domain domain{Domain::Guest};
    bool controlledExecution{};
public:
    ProcessObservation() noexcept;
#ifdef PINE_DIAG_RUNTIME_TEST
    ProcessObservation(const Profile &profile, Domain domain, bool controlledExecution = false) noexcept;
#endif
    ~ProcessObservation();
    ProcessObservation(const ProcessObservation &) = delete;
    void SealLoads() noexcept { loadsSealed.store(true, std::memory_order_release); }
    void Invalidate(StopReason reason) noexcept;
    void BeforeMappingMutation(uint64_t start, uint64_t size) noexcept;
    bool Validated() const noexcept;
    // No runtime address read: owned metadata only.
    std::shared_ptr<Module> Binding() const noexcept { return storage; }
    std::shared_ptr<GuestSlot> AttachThread(uint64_t guestThread) noexcept;
};
// Only exists on the synchronous NsoLoader -> LoadExecutable call stack. It
// carries no symbols, URI, FD or borrowed bytes past that stack's lifetime.
class ScopedLoad {
    ProcessObservation *process{};
    const image::Record *input{};
    ScopedLoad *previous{};
    bool observed{};
    std::unique_lock<std::mutex> loadLock;
    size_t patchUsage{},reservation{};
    uint64_t configurationEpoch{};
public:
    ScopedLoad(ProcessObservation &process, const image::Record *input, std::string_view moduleName = {}) noexcept;
    ~ScopedLoad();
    static bool Active() noexcept;
    static size_t ReservationBytes() noexcept;
    static void RecordPatchUsage(size_t bytes) noexcept;
    static void Mapped(std::span<uint8_t> text, std::span<const uint8_t> ro,
                       uint64_t moduleBase, uint64_t reservationBase, size_t reservationSize,
                       bool instructionCachePublished, std::span<uint8_t> patch = {}) noexcept;
};
#ifdef PINE_DIAG_RUNTIME_TEST
inline thread_local void (*beforeActivation)(ProcessObservation &){};
inline thread_local void (*beforePublication)(Module &) noexcept{};
std::array<std::shared_ptr<Module>,16> FillRegistryForTest();
void HoldRegistryForTest(std::atomic<bool> &entered,const std::atomic<bool> &release);
#endif
struct Module {
    image::Record input;
    std::array<image::Window,image::MaxWindows> loadedWindows;
    uint64_t moduleBase{}, textSize{}, roBase{}, roSize{}, optionEpoch{};
    Domain domain{};
    std::atomic<uint64_t> active{2}, stopReason{}, publishedReady{}; // 2 pending,1 active,0 closed
    bool cachePublished{}, identityExported{};
    bool observerInstalled{}, eventPublished{};
    uint64_t callPc{},pltPc{},noCallbackPc{},insufficientPc{};
    std::mutex threadMutex;
    std::array<std::shared_ptr<GuestSlot>,256> threads;
    nce::ObservationPatch observerPatch;
    // Post-NCE/pre-observer loaded copies, never a retained host mapping.
};
}
