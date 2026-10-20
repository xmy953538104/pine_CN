// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <csignal>
#include <string>
#include "diagnostics/pending_signal.h"

static skyline::diagnostics::PendingGuestSignal pending;
static bool enabled = true;
static int events{};
static std::string event, number;
static std::string signalScope, eventTimeScope;
namespace skyline::diagnostics {
    bool Enabled() noexcept { return enabled; }
    void Emit(std::string_view, std::string_view name,
              std::initializer_list<std::pair<std::string_view, std::string_view>> fields,
              std::string_view level) noexcept {
        assert(level == "ERROR");
        ++events; event = name;
        for (const auto &[key, value] : fields) {
            if (key == "signal") number = value;
            if (key == "signal_scope") signalScope = value;
            if (key == "event_time_scope") eventTimeScope = value;
        }
    }
}
static void Handler(int value) { pending.Capture(value); }
int main() {
    // Real POSIX signal delivery exercises only the signal-safe capture operation.
    std::signal(SIGUSR1, Handler);
    std::raise(SIGUSR1);
    assert(events == 0 && pending.value == SIGUSR1);
    pending.Drain();
    assert(events == 1 && event == "GUEST_SIGNAL" && number == std::to_string(SIGUSR1));
    assert(signalScope == "handled_guest_thread_signal_not_final_host_exit");
    assert(eventTimeScope == "normal_thread_drain_after_longjmp_not_fault_time");
    pending.Drain(); assert(events == 1);
    pending.Capture(SIGINT); pending.Drain(); assert(events == 1);
    enabled = false;
    pending.Capture(SIGSEGV); pending.Drain(); assert(events == 1 && pending.value == 0);
}
