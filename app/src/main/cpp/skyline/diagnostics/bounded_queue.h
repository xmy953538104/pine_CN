// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <type_traits>

namespace skyline::diagnostics {
enum class Admission { Accepted, Full, Contended };

// Bounded multi-producer, single-consumer ring. A producer never waits for a
// consumer or a mutex. A preempted reservation can delay the writer, not another
// producer's return. Admission has a finite CAS budget and reports its limit.
template<class T> class BoundedQueue {
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    static_assert(std::is_nothrow_move_assignable_v<T>);
    struct Slot { std::atomic<uint64_t> sequence{}; T value; };
    std::unique_ptr<Slot[]> slots;
    size_t capacity{};
    alignas(64) std::atomic<uint64_t> tail{}, head{};
public:
    void Reset(size_t count) {
        slots = std::make_unique<Slot[]>(count); capacity = count;
        tail = 0; head = 0;
        for (size_t i{}; i < count; ++i) slots[i].sequence = i;
    }
    Admission Push(T &&value, size_t limit) noexcept {
        auto position = tail.load(std::memory_order_relaxed);
        for (unsigned attempt{}; attempt < 64; ++attempt) {
            if (position - head.load(std::memory_order_acquire) >= limit) return Admission::Full;
            auto &slot = slots[position % capacity];
            auto sequence = slot.sequence.load(std::memory_order_acquire);
            if (sequence == position && tail.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
                slot.value = std::move(value);
                slot.sequence.store(position + 1, std::memory_order_release);
                return Admission::Accepted;
            }
            position = tail.load(std::memory_order_relaxed);
        }
        return Admission::Contended;
    }
    bool Pop(T &value, uint64_t &sequence) noexcept {
        auto position = head.load(std::memory_order_relaxed);
        auto &slot = slots[position % capacity];
        if (slot.sequence.load(std::memory_order_acquire) != position + 1) return false;
        value = std::move(slot.value); sequence = position + 1;
        slot.sequence.store(position + capacity, std::memory_order_release);
        head.store(position + 1, std::memory_order_release);
        return true;
    }
    bool Empty() const noexcept { return head.load(std::memory_order_relaxed) == tail.load(std::memory_order_acquire); }
};
}
