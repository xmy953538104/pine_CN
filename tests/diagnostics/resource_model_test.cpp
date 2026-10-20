// SPDX-License-Identifier: MPL-2.0
// Content-free, single-threaded numeric fixtures; no Guest or actual sampling.
#include "resource_model.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <type_traits>

static bool watchAllocations{};
static size_t observedAllocations{};
[[gnu::noinline]] void *operator new(size_t size) {
    if (watchAllocations) ++observedAllocations;
    if (auto *value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
[[gnu::noinline]] void *operator new[](size_t size) { return ::operator new(size); }
[[gnu::noinline]] void operator delete(void *value) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete[](void *value) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete(void *value, size_t) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete[](void *value, size_t) noexcept { std::free(value); }
[[gnu::noinline]] void *operator new(size_t size, std::align_val_t alignment) {
    if (watchAllocations) ++observedAllocations;
    void *value{};
    if (!posix_memalign(&value, static_cast<size_t>(alignment), size ? size : 1)) return value;
    throw std::bad_alloc();
}
[[gnu::noinline]] void *operator new[](size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
[[gnu::noinline]] void operator delete(void *value, std::align_val_t) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete[](void *value, std::align_val_t) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete(void *value, size_t, std::align_val_t) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete[](void *value, size_t, std::align_val_t) noexcept { std::free(value); }

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::abort(); } } while (false)
using namespace skyline::diagnostics::resource;

static Snapshot Running(uint64_t milliseconds, uint64_t accepted, uint64_t returns) {
    Snapshot value{};
    value.launch = 7; value.monotonicMs = milliseconds;
    value.presentAccepted = accepted; value.presentReturns = returns;
    value.observerAvailable = value.pauseKnown = value.stopKnown = true;
    return value;
}
static Snapshot Running(uint64_t milliseconds, uint64_t accepted) { return Running(milliseconds, accepted, accepted); }
static void EstablishThree(Model &model) {
    CHECK(model.Observe(Running(0, 1000)).observedAccepted == 0);
    for (uint64_t i = 1; i <= 3; ++i) {
        const auto update = model.Observe(Running(i * 1000, 1000 + i));
        CHECK(update.acceptedProgress && update.acceptedDelta == 1 && update.observedAccepted == i);
    }
}
static Update QuietUntil(Model &model, uint64_t from, uint64_t through, Snapshot input) {
    Update result{};
    for (uint64_t time = from; time <= through; time += 1000) {
        input.monotonicMs = time; result = model.Observe(input);
    }
    return result;
}

static void BaselineAndExactThreshold() {
    Model model;
    const auto initial = model.Observe(Running(0, 100000));
    CHECK(initial.discontinuity == Discontinuity::NewLaunch && !initial.acceptedProgress && initial.observedAccepted == 0);
    CHECK(QuietUntil(model, 1000, 60000, Running(0, 100000)).state == State::AwaitingProgress);
    model.Reset(); EstablishThree(model);
    CHECK(QuietUntil(model, 4000, 32000, Running(0, 1003)).state == State::Observing);
    auto result = model.Observe(Running(32999, 1003));
    CHECK(result.state == State::Observing && result.quietMs == 29999 && !result.acceptedProgress);
    result = model.Observe(Running(33000, 1003));
    CHECK(result.state == State::PostProgressNoNewPresent && result.quietMs == 30000);
    result = model.Observe(Running(33000, 1003));
    CHECK(!result.stored && !result.acceptedProgress && !result.recovered);
}

static void ThreeAcceptedNotThreeSamples() {
    Model model;
    model.Observe(Running(0, 55));
    model.Observe(Running(1000, 57));
    CHECK(QuietUntil(model, 2000, 100000, Running(0, 57)).state == State::AwaitingProgress);
    CHECK(model.Observe(Running(101000, 58)).observedAccepted == 3);
    CHECK(QuietUntil(model, 102000, 130000, Running(0, 58)).state == State::Observing);
    CHECK(model.Observe(Running(131000, 58)).state == State::PostProgressNoNewPresent);
    model.Reset(); model.Observe(Running(0, 900));
    const auto batch = model.Observe(Running(1000, 903));
    CHECK(batch.acceptedDelta == 3 && batch.observedAccepted == 3);
    CHECK(QuietUntil(model, 2000, 31000, Running(0, 903)).state == State::PostProgressNoNewPresent);
}

static void RealCounterProgressOnly() {
    Model model;
    model.Observe(Running(0, 0));
    for (uint64_t i = 1; i <= 120; ++i) {
        auto input = Running(i * 1000, 0, i);
        input.memory.available = HostRss | TrackedDevice;
        input.memory.hostRssBytes = i * 1000000;
        input.memory.trackedDeviceBytes = std::numeric_limits<uint64_t>::max();
        const auto update = model.Observe(input);
        CHECK(!update.acceptedProgress && !update.recovered && update.observedAccepted == 0);
        CHECK(update.state == State::AwaitingProgress);
    }
}

static void RecoveryIsActualAcceptedProgress() {
    Model model; EstablishThree(model);
    CHECK(QuietUntil(model, 4000, 33000, Running(0, 1003)).state == State::PostProgressNoNewPresent);
    auto recovered = model.Observe(Running(34000, 1004));
    CHECK(recovered.state == State::Observing && recovered.recovered && recovered.acceptedProgress && recovered.quietMs == 0);
    CHECK(!model.Observe(Running(34000, 1004)).recovered);
    CHECK(QuietUntil(model, 35000, 64000, Running(0, 1004)).state == State::PostProgressNoNewPresent);
    CHECK(model.Observe(Running(65000, 1006)).recovered);
}

static void PauseAndResumeAreNotProgress() {
    Model model; EstablishThree(model);
    auto paused = Running(0, 1003); paused.paused = true;
    CHECK(QuietUntil(model, 4000, 70000, paused).state == State::Paused);
    const auto resume = model.Observe(Running(71000, 1003));
    CHECK(resume.state == State::Observing && resume.quietMs == 0 && !resume.acceptedProgress && !resume.recovered);
    CHECK(QuietUntil(model, 72000, 100000, Running(0, 1003)).state == State::Observing);
    CHECK(model.Observe(Running(101000, 1003)).state == State::PostProgressNoNewPresent);
}

static void UnknownPauseAndStopCannotArm() {
    Model model; EstablishThree(model);
    auto unknown = Running(0, 1003); unknown.pauseKnown = false;
    CHECK(QuietUntil(model, 4000, 70000, unknown).state == State::PauseUnknown);
    unknown.pauseKnown = true; unknown.stopKnown = false;
    CHECK(QuietUntil(model, 71000, 140000, unknown).state == State::StopUnknown);
    const auto restored = model.Observe(Running(141000, 1003));
    CHECK(restored.state == State::Observing && restored.quietMs == 0 && !restored.acceptedProgress);
    CHECK(QuietUntil(model, 142000, 170000, Running(0, 1003)).state == State::Observing);
    CHECK(model.Observe(Running(171000, 1003)).state == State::PostProgressNoNewPresent);
}

static void UnavailableAndUnknownLaunchCannotArm() {
    Model model; EstablishThree(model);
    auto unavailable = Running(0, 0); unavailable.observerAvailable = false;
    CHECK(QuietUntil(model, 4000, 70000, unavailable).state == State::Unavailable);
    auto restored = model.Observe(Running(71000, 1003));
    CHECK(restored.observedAccepted == 3 && !restored.acceptedProgress && restored.quietMs == 0);
    auto unbound = Running(72000, 1003); unbound.launch = 0;
    CHECK(model.Observe(unbound).state == State::Unavailable);
    CHECK(QuietUntil(model, 73000, 140000, unbound).state == State::Unavailable);
}

static void StopAndNormalExitAreLatched() {
    Model model; EstablishThree(model);
    auto stopped = Running(4000, 1003); stopped.stopRequested = true;
    CHECK(model.Observe(stopped).state == State::StopRequested);
    CHECK(QuietUntil(model, 5000, 70000, Running(0, 1003)).state == State::StopRequested);
    auto exit = Running(71000, 1003); exit.normalExit = true;
    CHECK(model.Observe(exit).state == State::NormalExit);
    CHECK(model.Observe(Running(1, 0)).state == State::NormalExit); // Clock rollback cannot revive an ended generation.
    CHECK(QuietUntil(model, 1001, 70001, Running(0, 3)).state == State::NormalExit);
    auto next = Running(71001, 4000); next.launch = 8;
    const auto update = model.Observe(next);
    CHECK(update.state == State::AwaitingProgress && update.observedAccepted == 0 && model.Size() == 1);
}

static void NewLaunchUsesCumulativeCounterBaseline() {
    Model model; EstablishThree(model);
    QuietUntil(model, 4000, 33000, Running(0, 1003));
    auto next = Running(34000, 500000); next.launch = 9;
    auto update = model.Observe(next);
    CHECK(update.discontinuity == Discontinuity::NewLaunch && !update.acceptedProgress && !update.recovered);
    CHECK(update.state == State::AwaitingProgress && update.observedAccepted == 0 && model.Size() == 1);
    update = model.Observe(next);
    CHECK(!update.stored && update.observedAccepted == 0);
}

static void ClockRollbackAndCounterRegression() {
    Model model; EstablishThree(model);
    QuietUntil(model, 4000, 33000, Running(0, 1003));
    auto update = model.Observe(Running(200, 1003));
    CHECK(update.discontinuity == Discontinuity::ClockRollback && model.Size() == 1 && update.observedAccepted == 0);
    CHECK(!update.acceptedProgress && !update.recovered);
    update = model.Observe(Running(1200, 1002));
    CHECK(update.discontinuity == Discontinuity::CounterRegression && update.observedAccepted == 0 && model.Size() == 1);
    update = model.Observe(Running(2200, 1002, 1001));
    CHECK(update.discontinuity == Discontinuity::CounterRegression && update.observedAccepted == 0);
    // Independent atomic snapshots may have accepted>returns; no invented cross-counter relation.
    model.Reset(); model.Observe(Running(0, 900, 800));
    CHECK(model.Observe(Running(1000, 903, 801)).acceptedDelta == 3);
}

static void CadenceAndRealSamplesOnly() {
    Model model;
    CHECK(model.Observe(Running(0, 0)).stored);
    auto subsecond = model.Observe(Running(999, 3));
    CHECK(!subsecond.stored && subsecond.acceptedProgress && subsecond.observedAccepted == 3);
    CHECK(model.Observe(Running(1000, 3)).stored);
    CHECK(!model.Observe(Running(1999, 3)).stored);
    CHECK(model.Observe(Running(2000, 3)).stored && model.Size() == 3);
    Sample sample{};
    for (size_t i = 0; i < 3; ++i) { CHECK(model.RecentAt(i, sample)); CHECK(sample.snapshot.monotonicMs == i * 1000); }
    const auto sparse = model.Observe(Running(30000, 3));
    CHECK(sparse.discontinuity == Discontinuity::SamplingGap && sparse.quietMs == 0 && !sparse.acceptedProgress);
    CHECK(model.Size() == 4); // No synthetic rows for the missing 28 seconds.
}

static void SixtySecondWindowAndRingOrder() {
    Model model;
    for (uint64_t i = 0; i <= 60; ++i) model.Observe(Running(i * 1000, i));
    CHECK(model.Size() == WindowCapacity);
    Sample value{};
    for (size_t i = 0; i < WindowCapacity; ++i) {
        CHECK(model.RecentAt(i, value)); CHECK(value.snapshot.monotonicMs == (i + 1) * 1000);
    }
    value.snapshot.monotonicMs = 77;
    CHECK(!model.RecentAt(WindowCapacity, value) && value.snapshot.monotonicMs == 77);
    CHECK(!model.RecentAt(std::numeric_limits<size_t>::max(), value));
    CHECK(model.Observe(Running(120000, 60)).stored && model.Size() == 1);
    CHECK(model.RecentAt(0, value) && value.snapshot.monotonicMs == 120000);
}

static void GapAndUint64Boundaries() {
    Model model; EstablishThree(model);
    CHECK(model.Observe(Running(5500, 1003)).discontinuity == Discontinuity::None);
    CHECK(model.Observe(Running(8001, 1003)).discontinuity == Discontinuity::SamplingGap);
    CHECK(QuietUntil(model, 9001, 37001, Running(0, 1003)).state == State::Observing);
    CHECK(model.Observe(Running(38001, 1003)).state == State::PostProgressNoNewPresent);
    constexpr auto max = std::numeric_limits<uint64_t>::max();
    model.Reset(); model.Observe(Running(max - 40000, max - 10));
    for (uint64_t i = 1; i <= 33; ++i) {
        const auto update = model.Observe(Running(max - 40000 + i * 1000, max - 10 + (i < 3 ? i : 3)));
        if (i == 33) CHECK(update.state == State::PostProgressNoNewPresent && update.quietMs == 30000);
    }
    CHECK(model.Observe(Running(0, 0)).discontinuity == Discontinuity::ClockRollback);
}

static void FixedBudgetAndNoDynamicAllocation() {
    static_assert(sizeof(Model) <= MaximumModelBytes);
    static_assert(std::is_trivial_v<Sample> && std::is_standard_layout_v<Sample>);
    watchAllocations = true;
    void *probe = ::operator new(16);
    ::operator delete(probe);
    watchAllocations = false;
    CHECK(observedAllocations == 1); observedAllocations = 0;
    watchAllocations = true;
    {
        Model model;
        for (uint64_t i = 0; i < 100000; ++i) {
            auto input = Running(i * 1000, i);
            input.memory.available = HostPss | HostRss | GuestHeap | GuestMapped | TrackedDevice | HostVirtual;
            input.memory.hostRssBytes = std::numeric_limits<uint64_t>::max();
            model.Observe(input);
            Sample output{}; model.RecentAt(0, output);
            if (i % 100 == 0) model.Reset();
        }
    }
    watchAllocations = false;
    CHECK(observedAllocations == 0);
    std::printf("fixed-budget model_bytes=%zu sample_bytes=%zu capacity=%zu max_model_bytes=%zu\n",
        sizeof(Model), sizeof(Sample), WindowCapacity, MaximumModelBytes);
}

int main() {
    struct Case { const char *name; void (*run)(); };
    constexpr Case cases[]{
        {"baseline/exact 3-accepted+30s threshold", BaselineAndExactThreshold},
        {"accepted deltas, not observation counts", ThreeAcceptedNotThreeSamples},
        {"memory/other counters are not accepted progress", RealCounterProgressOnly},
        {"observable accepted-counter recovery", RecoveryIsActualAcceptedProgress},
        {"paused/resume quiet period", PauseAndResumeAreNotProgress},
        {"unknown pause/stop cannot arm", UnknownPauseAndStopCannotArm},
        {"unavailable/unbound generation", UnavailableAndUnknownLaunchCannotArm},
        {"stop/normal-exit termination latches", StopAndNormalExitAreLatched},
        {"new-launch cumulative counter baseline", NewLaunchUsesCumulativeCounterBaseline},
        {"clock/counter reset and independent counters", ClockRollbackAndCounterRegression},
        {"1s cadence and no fabricated samples", CadenceAndRealSamplesOnly},
        {"60s expiry and chronological ring bounds", SixtySecondWindowAndRingOrder},
        {"sampling gaps and uint64 boundaries", GapAndUint64Boundaries},
        {"fixed memory budget and no dynamic allocation", FixedBudgetAndNoDynamicAllocation},
    };
    for (const auto &test : cases) { test.run(); std::printf("PASS: %s\n", test.name); }
    std::printf("PASS: %zu synthetic resource model cases; no production hook, Guest, thread or memory measurement\n", std::size(cases));
}
