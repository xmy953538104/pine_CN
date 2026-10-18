// SPDX-License-Identifier: MPL-2.0
#pragma once
// Host fault-injection builds only; no hook calls or storage in the Android APK.
#ifdef PINE_DIAG_TEST_HOOKS
#include <atomic>
#include <string_view>
namespace skyline::diagnostics::testing {
enum class Point { ProducerStateRead, PublishedBeforeCount, BeforeRecordWrite };
inline std::atomic<void (*)(Point, std::string_view)> hook{};
inline void Notify(Point point, std::string_view event = {}) {
    if (auto callback = hook.load()) callback(point, event);
}
}
#endif
