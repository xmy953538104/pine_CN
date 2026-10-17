// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <jni.h>
#include <string>
#include "identity_observer.h"

namespace skyline::diagnostics {
inline void NotifyJavaContentIdentity(JNIEnv *env, jclass clazz, jlong owner, std::string_view application, std::string_view source) noexcept {
    if (!clazz || env->ExceptionCheck()) return; // Preserve an existing Java exception.
    jstring app{}, origin{};
    try {
        const auto method = env->GetStaticMethodID(clazz, "onNativeContentIdentity", "(JLjava/lang/String;Ljava/lang/String;)V");
        if (method && !env->ExceptionCheck()) app = env->NewStringUTF(std::string(application).c_str());
        if (app && !env->ExceptionCheck()) origin = env->NewStringUTF(std::string(source).c_str());
        if (origin && !env->ExceptionCheck()) env->CallStaticVoidMethod(clazz, method, owner, app, origin);
    } catch (...) {}
    if (app) env->DeleteLocalRef(app);
    if (origin) env->DeleteLocalRef(origin);
    // Only failures of this optional diagnostic callback are contained.
    if (env->ExceptionCheck()) env->ExceptionClear();
}
class ScopedJavaContentIdentityObserver {
    JNIEnv *env;
    jlong owner;
    jclass clazz{};
    bool ownsClass{};
    static void Callback(void *opaque, std::string_view app, std::string_view source) noexcept {
        const auto &self = *static_cast<ScopedJavaContentIdentityObserver *>(opaque);
        NotifyJavaContentIdentity(self.env, self.clazz, self.owner, app, source);
    }
    ScopedContentIdentityObserver observer;
public:
    ScopedJavaContentIdentityObserver(JNIEnv *env, jlong owner, jclass suppliedClass = nullptr) noexcept
        : env(env), owner(owner), clazz(suppliedClass), observer({Callback, this}) {
        if (!clazz && !env->ExceptionCheck()) {
            clazz = env->FindClass("emu/skyline/diagnostics/Diagnostics");
            ownsClass = clazz != nullptr;
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }
    ~ScopedJavaContentIdentityObserver() { if (ownsClass) env->DeleteLocalRef(clazz); }
};
}
