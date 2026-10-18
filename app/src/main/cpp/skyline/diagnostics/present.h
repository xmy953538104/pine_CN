// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <utility>
#include "diagnostics.h"
namespace skyline::diagnostics {
// The pinned Vulkan-Hpp wrapper throws SystemError after receiving negative
// VkResult. Observe that original code and rethrow the same exception object.
template<class SystemError, class Call> auto ObservePresent(Call &&call) {
    try {
        auto result = std::forward<Call>(call)();
        PresentResult(static_cast<int32_t>(result)); return result;
    } catch (const SystemError &error) {
        PresentResult(static_cast<int32_t>(error.code().value()), true);
        throw;
    }
}
}
