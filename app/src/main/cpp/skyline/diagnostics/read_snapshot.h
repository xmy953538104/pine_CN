// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <atomic>
#include <thread>

namespace skyline::diagnostics {
// Ordinary metadata setters are serialized by their caller. Emitters acquire a
// stable slot using only atomics, and never wait for a setter's mutex. Sequential
// consistency closes the late-reader/publication race; copying non-atomic T is
// safe because a published or reader-owned slot is never modified.
template<class T> class ReadSnapshot {
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    struct Slot { mutable std::atomic<unsigned> readers{}; T value; };
    std::array<Slot, 8> slots;
    std::atomic<unsigned> active{};
public:
    // Inspect immutable metadata without constructing/copying an owned T.
    // The visitor must not retain references beyond this call.
    template<class Visitor> bool Inspect(Visitor &&visitor) const {
        for (unsigned attempt{}; attempt < 64; ++attempt) {
            auto index = active.load();
            auto &slot = slots[index];
            slot.readers.fetch_add(1);
            if (active.load() == index) {
                try { visitor(static_cast<const T &>(slot.value)); }
                catch (...) { slot.readers.fetch_sub(1); throw; }
                slot.readers.fetch_sub(1); return true;
            }
            slot.readers.fetch_sub(1);
        }
        return false;
    }
    bool Read(T &result) const {
        return Inspect([&](const T &value) { result = value; });
    }
    void Publish(const T &value) {
        // Only infrequent ordinary-thread metadata setters may wait. Not Emit,
        // the present observer, a guest thread's queue admission, or a signal.
        for (;;) {
            auto current = active.load();
            for (unsigned i{}; i < slots.size(); ++i) {
                if (i != current && !slots[i].readers.load()) {
                    slots[i].value = value; active.store(i); return;
                }
            }
            std::this_thread::yield();
        }
    }
};
}
