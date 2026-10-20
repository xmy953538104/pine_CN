// SPDX-License-Identifier: MPL-2.0
// Actual production POSIX window writer -> JVM ZIP exporter. Synthetic rows,
// narrow IO faults; no Android, guest, ROM, keys or application signing.
#include <jni.h>
#include <algorithm>
#include <diagnostics/resource_window.h>

namespace {
struct WindowIo : skyline::diagnostics::resource_window_detail::PosixIo {
    static inline thread_local int fault{}, calls{};
    static ssize_t Write(int fd, const void *data, size_t size) noexcept {
        ++calls;
        if (fault == 1) { errno = ENOSPC; return -1; }
        if (fault == 3 && calls == 1) { errno = EINTR; return -1; }
        return ::write(fd, data, fault == 3 ? std::min<size_t>(7, size) : size);
    }
    static int Rename(int dir, const char *from, const char *to) noexcept {
        if (fault == 2) { errno = EACCES; return -1; }
        return ::renameat(dir, from, dir, to);
    }
};
}

extern "C" JNIEXPORT jobjectArray JNICALL Java_emu_skyline_diagnostics_ResourceWindowNativeBridge_persist(
    JNIEnv *env, jclass, jstring directory, jint marker, jint fault) {
    using namespace skyline::diagnostics;
    if (!directory) return nullptr;
    const char *text = env->GetStringUTFChars(directory, nullptr);
    if (!text) return nullptr;
    std::string path;
    try { path = text; } catch (...) { env->ReleaseStringUTFChars(directory, text); return nullptr; }
    env->ReleaseStringUTFChars(directory, text);
    try {
        WindowIo::fault = fault; WindowIo::calls = 0;
        BasicRecentResourceWindow<WindowIo> window;
        const auto row = marker == 0 ?
            "{\"schema\":1,\"session_id\":\"native-window\",\"sequence\":7,\"category\":\"resource\",\"event\":\"RESOURCE_GPU_SAMPLE\",\"fields\":{\"marker\":\"before\",\"present_accepted\":\"7\",\"path\":\"/data/user/0/CONTROLLED/private\",\"password\":\"CONTROLLED_OMIT\"}}" :
            "{\"schema\":1,\"session_id\":\"native-window\",\"sequence\":8,\"category\":\"resource\",\"event\":\"RESOURCE_GPU_SAMPLE\",\"fields\":{\"marker\":\"after\",\"present_accepted\":\"8\",\"path\":\"/data/user/0/CONTROLLED/private\",\"password\":\"CONTROLLED_OMIT\"}}";
        if (!window.Add(1, row)) return nullptr;
        const auto saved = window.Persist(path, "native-window", ResourceWindowReason::Periodic);
        const auto failure = window.LastFailure();
        const char *values[]{saved ? "true" : "false", ResourceWindowFailureStageName(failure.stage),
            ResourceWindowFailureReasonName(failure.reason), ResourceWindowErrnoName(failure.error)};
        auto clazz = env->FindClass("java/lang/String");
        if (!clazz) return nullptr;
        auto result = env->NewObjectArray(4, clazz, nullptr);
        env->DeleteLocalRef(clazz);
        if (!result) return nullptr;
        for (jsize i{}; i < 4; ++i) {
            auto value = env->NewStringUTF(values[i]);
            if (!value) return nullptr;
            env->SetObjectArrayElement(result, i, value); env->DeleteLocalRef(value);
            if (env->ExceptionCheck()) return nullptr;
        }
        return result;
    } catch (...) { return nullptr; }
}
