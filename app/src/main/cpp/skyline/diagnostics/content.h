// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "diagnostics.h"

// Argument formatting is diagnostic work too: allocation failures must never alter emulation.
#define PINE_DIAG_CONTENT_LEVEL(level, event, ...) do { \
    if (::skyline::diagnostics::Enabled()) { \
        try { ::skyline::diagnostics::Emit("content", event, {__VA_ARGS__}, level); } catch (...) {} \
    } \
} while (false)

#define PINE_DIAG_CONTENT(event, ...) PINE_DIAG_CONTENT_LEVEL("INFO", event, __VA_ARGS__)
#define PINE_DIAG_CONTENT_ERROR(event, ...) PINE_DIAG_CONTENT_LEVEL("ERROR", event, __VA_ARGS__)
