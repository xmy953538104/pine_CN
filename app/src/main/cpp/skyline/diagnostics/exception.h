// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <charconv>
#include "diagnostics.h"

namespace skyline::diagnostics {
    // For ordinary C++ catch sites only; never for an asynchronous signal handler.
    inline void SignalException(int signal, std::string_view location, std::string_view call = {}) noexcept {
        if (!Enabled())
            return;
        char number[16]{};
        const auto converted{std::to_chars(number, number + sizeof(number), signal)};
        Emit("exception", "NATIVE_SIGNAL_EXCEPTION",
             {{"signal", std::string_view(number, converted.ptr - number)}, {"location", location}, {"call", call}}, "ERROR");
    }
}
