// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <csignal>
#include <string>
#include "diagnostics/pending_signal.h"

static skyline::diagnostics::PendingGuestSignal pending;
static bool enabled = true;
static int events{};
static std::string event, number;
namespace skyline::diagnostics {
    bool Enabled() noexcept { return enabled; }
    void Emit(std::string_view, std::string_view name,
              std::initializer_list<std::pair<std::string_view, std::string_view>> fields,
              std::string_view level) noexcept {
        assert(level == "ERROR");
        ++events; event = name;
        for (const auto &[key, value] : fields)
            if (key == "signal") number = value;
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
    pending.Drain(); assert(events == 1);
    pending.Capture(SIGINT); pending.Drain(); assert(events == 1);
    enabled = false;
    pending.Capture(SIGSEGV); pending.Drain(); assert(events == 1 && pending.value == 0);
}
