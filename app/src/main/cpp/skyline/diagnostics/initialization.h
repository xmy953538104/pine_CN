// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <exception>
#include <utility>
#include "diagnostics.h"

namespace skyline::diagnostics {
// Ordinary startup threads only. The guard observes the original construction
// and rethrow path, including non-movable Vulkan/member objects. It never runs
// in a raw signal handler and cannot turn a fatal signal into a caught error.
class InitializationStage {
    std::string_view name;
    bool enabled;
    int exceptions{};
public:
    explicit InitializationStage(std::string_view stage) noexcept : name(stage), enabled(Enabled()) {
        if (enabled) {
            exceptions = std::uncaught_exceptions();
            Emit("lifecycle", "STAGE_STARTED", {{"stage", name}});
        }
    }
    ~InitializationStage() noexcept {
        if (!enabled) return;
        if (std::uncaught_exceptions() > exceptions) {
            // Unwinding alone is not a crash classification (ExitException and
            // SIGINT can be normal cancellation). The existing catch classifies.
            Emit("lifecycle", "INITIALIZATION_UNWOUND", {{"stage", name}, {"reason", "unclassified_exception_unwinding"}});
            // Only the rare ordinary-thread error path waits for a bounded
            // snapshot. No stage success, guest read, or raw signal waits.
            Flush(250);
        } else {
            Stage(name);
        }
    }
};

template<typename F> decltype(auto) ObserveInitialization(std::string_view stage, F &&operation) {
    InitializationStage observation(stage);
    return std::forward<F>(operation)();
}
}
