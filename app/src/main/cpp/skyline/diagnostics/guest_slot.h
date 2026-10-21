// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
namespace skyline::diagnostics::runtime {
// Written only by the owning Guest thread. Never reset/reused, even after exit.
// A normal worker may read payload only after acquire state==2. Interrupted
// Writing is not a snapshot. Both gate atomics and module storage outlive join.
struct alignas(64) GuestSlot {
    std::atomic<uint64_t> state{}; // 0 empty,1 writing,2 release-ready,3 consumed
    const std::atomic<uint64_t> *optionGate{};
    uint64_t expectedOption{};
    const std::atomic<uint64_t> *ownerGate{};
    uint64_t expectedOwner{};
    const std::atomic<uint64_t> *moduleActive{};
    uint64_t x0{},lr{},sp{},observerPc{};
    uint64_t guestThread{},threadGeneration{};
    std::atomic<bool> retired{};
};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(offsetof(GuestSlot,optionGate)==8&&offsetof(GuestSlot,expectedOption)==16);
static_assert(offsetof(GuestSlot,ownerGate)==24&&offsetof(GuestSlot,expectedOwner)==32);
static_assert(offsetof(GuestSlot,moduleActive)==40&&offsetof(GuestSlot,x0)==48);
static_assert(offsetof(GuestSlot,lr)==56&&offsetof(GuestSlot,sp)==64&&offsetof(GuestSlot,observerPc)==72);
}
