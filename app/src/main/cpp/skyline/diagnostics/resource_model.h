// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace skyline::diagnostics::resource {
inline constexpr size_t WindowCapacity = 60;
inline constexpr uint64_t SamplePeriodMs = 1000;
inline constexpr uint64_t RecentWindowMs = 60 * SamplePeriodMs;
inline constexpr uint64_t NoNewPresentMs = 30 * SamplePeriodMs;
inline constexpr uint64_t MaximumObservationGapMs = 2500;
inline constexpr uint64_t MinimumAcceptedPresents = 3;
inline constexpr size_t MaximumModelBytes = 16 * 1024;

enum MemoryAvailability : uint32_t {
    HostPss = 1U << 0, HostRss = 1U << 1, GuestHeap = 1U << 2,
    GuestMapped = 1U << 3, TrackedDevice = 1U << 4, HostVirtual = 1U << 5,
};
// Values are supplied observations. A zero without its availability bit means
// unknown, not measured zero. This model neither measures nor estimates memory.
struct MemoryValues {
    uint64_t hostPssBytes, hostRssBytes, guestHeapBytes, guestMappedBytes;
    uint64_t trackedDeviceBytes, hostVirtualBytes;
    uint32_t available;
};

struct Snapshot {
    uint64_t launch; // Caller-owned nonzero generation, not a title/hash/string inference.
    uint64_t monotonicMs;
    // Process-cumulative counters: the first reliable sample of every launch is
    // a baseline. No producer counter may be reset just to feed this model.
    uint64_t presentAccepted, presentReturns;
    MemoryValues memory;
    bool observerAvailable;
    bool pauseKnown, paused;
    bool stopKnown, stopRequested;
    bool normalExit;
};

enum class State : uint8_t {
    Unavailable, PauseUnknown, StopUnknown, Paused, StopRequested, NormalExit,
    AwaitingProgress, Observing, PostProgressNoNewPresent,
};
enum class Discontinuity : uint8_t { None, NewLaunch, ClockRollback, CounterRegression, SamplingGap };

struct Sample {
    Snapshot snapshot;
    State state;
    uint64_t quietMs;
};
struct Update {
    State state;
    Discontinuity discontinuity;
    bool stored, acceptedProgress, recovered;
    uint64_t acceptedDelta, observedAccepted, quietMs;
};

// Ordinary serialized caller only. No atomics, locks, threads, Guest access,
// allocation, I/O, formatting or clock calls. This is not a hang/OOM detector.
class Model {
    std::array<Sample, WindowCapacity> recent{};
    size_t first{}, count{};
    uint64_t launch{}, lastObservedMs{}, lastStoredMs{}, previousAccepted{}, previousReturns{};
    uint64_t observedAccepted{}, quietSinceMs{};
    bool initialized{}, haveStored{}, haveCounters{}, continuous{}, noPresentEpisode{};
    bool stopped{}, exited{};

    void Begin(const Snapshot &input, bool preserveTermination) noexcept {
        const bool priorStop = preserveTermination && stopped;
        const bool priorExit = preserveTermination && exited;
        Reset();
        launch = input.launch; lastObservedMs = input.monotonicMs;
        quietSinceMs = input.monotonicMs; initialized = true;
        stopped = priorStop; exited = priorExit;
    }
    void Expire(uint64_t now) noexcept {
        // Exact boundary age==60s is excluded. Missing samples are never filled.
        while (count && now - recent[first].snapshot.monotonicMs >= RecentWindowMs) {
            first = (first + 1) % WindowCapacity; --count;
        }
    }
    static uint64_t AddSaturating(uint64_t left, uint64_t right) noexcept {
        constexpr auto max = std::numeric_limits<uint64_t>::max();
        return right > max - left ? max : left + right;
    }

  public:
    Model() noexcept = default;
    void Reset() noexcept {
        for (auto &sample : recent) sample = {};
        first = count = 0;
        launch = lastObservedMs = lastStoredMs = previousAccepted = previousReturns = 0;
        observedAccepted = quietSinceMs = 0;
        initialized = haveStored = haveCounters = continuous = noPresentEpisode = stopped = exited = false;
    }

    Update Observe(const Snapshot &input) noexcept {
        Update result{};
        if (!initialized || input.launch != launch) {
            Begin(input, false); result.discontinuity = Discontinuity::NewLaunch;
        } else if (input.monotonicMs < lastObservedMs) {
            Begin(input, true); result.discontinuity = Discontinuity::ClockRollback;
        } else if (input.monotonicMs - lastObservedMs > MaximumObservationGapMs) {
            continuous = false; result.discontinuity = Discontinuity::SamplingGap;
        }
        lastObservedMs = input.monotonicMs;
        Expire(input.monotonicMs);

        if (input.launch && input.observerAvailable) {
            if (haveCounters && (input.presentAccepted < previousAccepted || input.presentReturns < previousReturns)) {
                Begin(input, true); result.discontinuity = Discontinuity::CounterRegression;
            }
            if (haveCounters) {
                result.acceptedDelta = input.presentAccepted - previousAccepted;
                result.acceptedProgress = result.acceptedDelta != 0;
                observedAccepted = AddSaturating(observedAccepted, result.acceptedDelta);
                if (result.acceptedProgress) {
                    result.recovered = noPresentEpisode;
                    noPresentEpisode = false;
                }
            }
            previousAccepted = input.presentAccepted; previousReturns = input.presentReturns; haveCounters = true;
        }
        if (input.stopKnown && input.stopRequested) stopped = true;
        if (input.normalExit) exited = true;

        // Unknown pause/stop state must not silently become false. Termination
        // is latched for this generation and only a new launch/explicit Reset
        // can clear it, including across clock/counter discontinuities.
        if (exited) result.state = State::NormalExit;
        else if (stopped) result.state = State::StopRequested;
        else if (!input.launch || !input.observerAvailable) result.state = State::Unavailable;
        else if (!input.pauseKnown) result.state = State::PauseUnknown;
        else if (!input.stopKnown) result.state = State::StopUnknown;
        else if (input.paused) result.state = State::Paused;
        else {
            if (!continuous || result.acceptedProgress) quietSinceMs = input.monotonicMs;
            continuous = true;
            result.quietMs = input.monotonicMs - quietSinceMs;
            result.state = observedAccepted < MinimumAcceptedPresents ? State::AwaitingProgress : State::Observing;
            if (observedAccepted >= MinimumAcceptedPresents && result.quietMs >= NoNewPresentMs) {
                result.state = State::PostProgressNoNewPresent;
                noPresentEpisode = true;
            }
        }
        if (result.state != State::AwaitingProgress && result.state != State::Observing && result.state != State::PostProgressNoNewPresent) {
            continuous = false;
        }
        result.observedAccepted = observedAccepted;

        if (!haveStored || input.monotonicMs - lastStoredMs >= SamplePeriodMs) {
            if (count == WindowCapacity) { first = (first + 1) % WindowCapacity; --count; }
            recent[(first + count) % WindowCapacity] = {input, result.state, result.quietMs};
            ++count; lastStoredMs = input.monotonicMs; haveStored = true; result.stored = true;
        }
        return result;
    }

    size_t Size() const noexcept { return count; }
    // Chronological order, oldest first. The output is untouched on failure.
    bool RecentAt(size_t index, Sample &output) const noexcept {
        if (index >= count) return false;
        output = recent[(first + index) % WindowCapacity]; return true;
    }
};

static_assert(std::is_trivial_v<MemoryValues> && std::is_standard_layout_v<MemoryValues>);
static_assert(std::is_trivial_v<Snapshot> && std::is_standard_layout_v<Snapshot>);
static_assert(std::is_trivial_v<Sample> && std::is_standard_layout_v<Sample>);
static_assert(std::is_trivial_v<Update> && std::is_standard_layout_v<Update>);
static_assert(sizeof(Model) <= MaximumModelBytes, "resource history exceeded its fixed memory budget");
}
