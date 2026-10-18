// SPDX-License-Identifier: MPL-2.0
// Pause the production writer and count actual allocations on the producer.
#include "diagnostics.h"
#include "test_hooks.h"
#include <cassert>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <new>

namespace {
thread_local bool countAllocations{};
thread_local size_t allocations{};
void *Allocate(size_t bytes) {
    if (countAllocations) ++allocations;
    if (auto *value = std::malloc(bytes ? bytes : 1)) return value;
    throw std::bad_alloc();
}
void *AllocateAligned(size_t bytes, std::align_val_t alignment) {
    if (countAllocations) ++allocations;
    void *value{};
    if (!posix_memalign(&value, static_cast<size_t>(alignment), bytes ? bytes : 1)) return value;
    throw std::bad_alloc();
}
std::mutex mutex;
std::condition_variable condition;
bool writerPaused{}, releaseWriter{};
void Hook(skyline::diagnostics::testing::Point point, std::string_view event) {
    if (point != skyline::diagnostics::testing::Point::BeforeRecordWrite || event != "WRITER_PAUSE") return;
    std::unique_lock lock(mutex);
    writerPaused = true; condition.notify_all();
    condition.wait(lock, [] { return releaseWriter; });
}
template<class Action> void WithoutAllocations(Action action) {
    allocations = 0; countAllocations = true;
    action();
    countAllocations = false;
    assert(allocations == 0);
}
}
void *operator new(size_t bytes) { return Allocate(bytes); }
void *operator new[](size_t bytes) { return Allocate(bytes); }
void *operator new(size_t bytes, std::align_val_t alignment) { return AllocateAligned(bytes, alignment); }
void *operator new[](size_t bytes, std::align_val_t alignment) { return AllocateAligned(bytes, alignment); }
void operator delete(void *value) noexcept { std::free(value); }
void operator delete[](void *value) noexcept { std::free(value); }
void operator delete(void *value, size_t) noexcept { std::free(value); }
void operator delete[](void *value, size_t) noexcept { std::free(value); }
void operator delete(void *value, std::align_val_t) noexcept { std::free(value); }
void operator delete[](void *value, std::align_val_t) noexcept { std::free(value); }
void operator delete(void *value, size_t, std::align_val_t) noexcept { std::free(value); }
void operator delete[](void *value, size_t, std::align_val_t) noexcept { std::free(value); }

int main(int argc, char **argv) {
    using namespace skyline::diagnostics;
    assert(argc == 2);
    // Context values deliberately exceed SSO. All inputs are owned before the
    // measured calls; copying context or the payload would allocate and fail.
    const std::string identity(32, 'a'), content(64, 'b'), origin(64, 'c');
    const std::string launch(96, 'l'), stale(96, 's'), payload(2048, 'p');
    const std::pair<std::string_view, std::string_view> fields[]{{"payload", payload}};
    SetContentIdentity(identity, identity, identity, content, origin);
    SetEffectiveExeFs(identity, content, origin);
    SetLaunchId(launch);
    Config config{argv[1], "saturated-allocation", 64, 65536, 3, false, "content"};
    assert(Start(config)); testing::hook = Hook;
    assert(EmitFields("content", "WRITER_PAUSE", {}));
    {
        std::unique_lock lock(mutex);
        assert(condition.wait_for(lock, std::chrono::seconds(5), [] { return writerPaused; }));
    }
    for (size_t i{}; i < 56; ++i) assert(EmitFields("content", "ORDINARY", fields));
    assert(Stats().accepted == 57 && Stats().dropped == 0);
    WithoutAllocations([&] {
        for (size_t i{}; i < 1000; ++i) assert(!EmitFields("content", "ORDINARY", fields));
    });
    assert(Stats().accepted == 57 && Stats().dropped == 1000 && Stats().droppedFull == 1000 && Stats().criticalDropped == 0);
    // All eight critical reservations remain usable after ordinary saturation.
    for (size_t i{}; i < 8; ++i) assert(EmitFields("content", "DLC_REGISTERED", fields));
    assert(Stats().accepted == 65);
    WithoutAllocations([&] {
        for (size_t i{}; i < 1000; ++i) assert(!EmitFields("content", "DLC_REGISTERED", fields));
    });
    assert(Stats().dropped == 2000 && Stats().droppedFull == 2000 && Stats().criticalDropped == 1000);
    // Category/stale-launch filtering must not become counted Full drops.
    WithoutAllocations([&] {
        for (size_t i{}; i < 1000; ++i) {
            assert(!EmitFields("guest", "DFX_PROBE", fields));
            assert(!testing::EmitForLaunch(stale, "lifecycle", "LAUNCH_PROGRESS", fields));
        }
    });
    assert(Stats().dropped == 2000 && Stats().criticalDropped == 1000);
    WithoutAllocations([&] {
        assert(!testing::EmitForLaunch(launch, "lifecycle", "LAUNCH_PROGRESS", fields));
    });
    assert(Stats().droppedFull == 2001 && Stats().criticalDropped == 1001);
    { std::lock_guard lock(mutex); releaseWriter = true; condition.notify_all(); }
    assert(Flush(2000)); Stop(); testing::hook = nullptr;
    const auto result = Stats();
    assert(result.accepted == 65 && result.written == 65 && result.dropped == 2001);
    assert(result.droppedFull == 2001 && result.criticalDropped == 1001);
    assert(result.droppedAllocation == 0 && result.droppedContention == 0 && result.diskErrors == 0);
    std::cout << "PASS: paused production writer, 56 ordinary + 8 critical slots; 2001 counted Full calls and 2000 filtered calls allocate zero; 65 accepted/written\n";
}
