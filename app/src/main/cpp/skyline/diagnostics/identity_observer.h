// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <string_view>

namespace skyline::diagnostics {
// Only the ordinary synchronous JNI caller owns this observer. No JNIEnv or
// Java reference is retained by the sink, a worker, or a signal handler.
struct ContentIdentityObserver {
    using Callback = void (*)(void *, std::string_view, std::string_view) noexcept;
    Callback callback{};
    void *context{};
};
inline thread_local ContentIdentityObserver contentIdentityObserver;
class ScopedContentIdentityObserver {
    ContentIdentityObserver previous;
public:
    explicit ScopedContentIdentityObserver(ContentIdentityObserver next) noexcept : previous(contentIdentityObserver) { contentIdentityObserver = next; }
    ~ScopedContentIdentityObserver() { contentIdentityObserver = previous; }
    ScopedContentIdentityObserver(const ScopedContentIdentityObserver &) = delete;
    ScopedContentIdentityObserver &operator=(const ScopedContentIdentityObserver &) = delete;
};
inline void NotifyContentIdentity(std::string_view application, std::string_view source) noexcept {
    if (contentIdentityObserver.callback) contentIdentityObserver.callback(contentIdentityObserver.context, application, source);
}
}
