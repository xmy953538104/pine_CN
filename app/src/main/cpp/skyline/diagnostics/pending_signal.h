// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <csignal>
#include "exception.h"

namespace skyline::diagnostics {
    // A guest signal is delivered on the owning KThread. The handler only stores
    // sig_atomic_t; serialization happens after its existing longjmp returns.
    struct PendingGuestSignal {
        volatile std::sig_atomic_t value{};
        void Capture(int number) noexcept {
            if (number != SIGINT)
                value = number;
        }
        void Drain() noexcept {
            const auto number{value};
            value = 0;
            if (!number || !Enabled())
                return;
            char text[16]{};
            const auto converted{std::to_chars(text, text + sizeof(text), number)};
            Emit("exception", "GUEST_SIGNAL", {{"signal", std::string_view(text, converted.ptr - text)},
                 {"location", "NCE::SignalHandler"}, {"signal_scope", "handled_guest_thread_signal_not_final_host_exit"},
                 {"event_time_scope", "normal_thread_drain_after_longjmp_not_fault_time"}}, "ERROR");
        }
    };
}
