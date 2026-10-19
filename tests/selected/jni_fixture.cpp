// SPDX-License-Identifier: MPL-2.0
// Host JVM transfer fixture. It exercises the production JNI field decoder and
// pure request validator, never an Android OS, loader tail or guest process.
#include <loader/selected_launch_jni.h>

extern "C" jstring Java_emu_skyline_loader_SelectedContentBridge_inspectObserved(JNIEnv *, jclass, jint, jstring, jstring);

extern "C" JNIEXPORT jstring JNICALL Java_emu_skyline_loader_SelectedJniFixture_parse(
    JNIEnv *env, jclass, jobjectArray fields, jintArray indices) {
    try {
        const auto value = skyline::loader::ParseSelectedLaunchJni(env, fields, indices);
        const auto description = std::string(value.standalone ? "standalone_patch" : "application") + ":" +
            skyline::catalog::Hex64(value.applicationId) + ":" + value.primary.contentId + ":" +
            std::to_string(value.primary.programIndex) + ":" + std::to_string(value.addOnGroups.size());
        return env->NewStringUTF(description.c_str());
    } catch (const skyline::catalog::Error &error) {
        if (!env->ExceptionCheck()) {
            const auto clazz = env->FindClass("java/lang/IllegalArgumentException");
            if (clazz) { env->ThrowNew(clazz, error.code.c_str()); env->DeleteLocalRef(clazz); }
        }
        return nullptr;
    }
}

extern "C" JNIEXPORT void JNICALL Java_emu_skyline_loader_SelectedJniFixture_pending(JNIEnv *env, jclass) {
    const auto clazz = env->FindClass("java/lang/IllegalStateException");
    env->ThrowNew(clazz, "synthetic_pending_exception");
    env->DeleteLocalRef(clazz);
    try { (void)skyline::loader::ParseSelectedLaunchJni(env, nullptr, nullptr); }
    catch (const skyline::catalog::Error &) { /* Pending VM exception must survive. */ }
}

extern "C" JNIEXPORT void JNICALL Java_emu_skyline_loader_SelectedJniFixture_pendingObserved(JNIEnv *env, jclass) {
    const auto clazz = env->FindClass("java/lang/IllegalStateException");
    if (!clazz) return;
    env->ThrowNew(clazz, "synthetic_pending_exception"); env->DeleteLocalRef(clazz);
    (void)Java_emu_skyline_loader_SelectedContentBridge_inspectObserved(env, nullptr, -1, nullptr, nullptr);
}
