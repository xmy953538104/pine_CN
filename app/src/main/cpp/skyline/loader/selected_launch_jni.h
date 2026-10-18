// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "selected_launch.h"
#include <jni.h>

namespace skyline::loader {
    namespace selected_jni {
        inline void Check(JNIEnv *env) {
            if (env->ExceptionCheck()) throw catalog::Error("selected_jni_exception", "selected JNI transfer failed");
        }
        class LocalRef {
            JNIEnv *env;
            jobject value;
          public:
            LocalRef(JNIEnv *env, jobject value) : env(env), value(value) {}
            ~LocalRef() { if (value) env->DeleteLocalRef(value); }
            LocalRef(const LocalRef &) = delete;
            LocalRef &operator=(const LocalRef &) = delete;
            jobject Get() const { return value; }
        };
        inline std::string Ascii(JNIEnv *env, jstring value, jclass stringClass) {
            if (!value || !env->IsInstanceOf(value, stringClass))
                throw catalog::Error("selected_wire_invalid", "invalid selected launch field");
            Check(env);
            const auto length = env->GetStringLength(value);
            Check(env);
            // The longest legitimate field is a 76-byte CNMT identity. Reject
            // oversized Java strings before allocating/copying their contents.
            if (length > 128) throw catalog::Error("selected_wire_limit", "selected launch field exceeds limit");
            jchar units[128]{};
            if (length) env->GetStringRegion(value, 0, length, units);
            Check(env);
            std::string result;
            result.reserve(static_cast<size_t>(length));
            for (jsize i{}; i < length; ++i) {
                if (!units[i] || units[i] > 0x7f)
                    throw catalog::Error("selected_wire_invalid", "invalid selected launch field");
                result.push_back(static_cast<char>(units[i]));
            }
            return result;
        }
    }
    inline SelectedLaunchRequest ParseSelectedLaunchJni(JNIEnv *env, jobjectArray fields, jintArray indices) {
        selected_jni::Check(env);
        if (!fields || !indices) throw catalog::Error("selected_wire_invalid", "selected launch arrays required");
        const auto count = env->GetArrayLength(fields), indexCount = env->GetArrayLength(indices);
        selected_jni::Check(env);
        if (count < 10 || count > 42 || indexCount != 2)
            throw catalog::Error("selected_wire_limit", "selected launch array limit");
        selected_jni::LocalRef stringClass(env, env->FindClass("java/lang/String"));
        selected_jni::Check(env);
        if (!stringClass.Get()) throw catalog::Error("selected_jni_exception", "selected JNI transfer failed");
        std::vector<std::string> copied;
        copied.reserve(static_cast<size_t>(count));
        for (jsize i{}; i < count; ++i) {
            selected_jni::LocalRef field(env, env->GetObjectArrayElement(fields, i));
            selected_jni::Check(env);
            copied.push_back(selected_jni::Ascii(env, static_cast<jstring>(field.Get()), static_cast<jclass>(stringClass.Get())));
        }
        jint rawIndices[2]{};
        env->GetIntArrayRegion(indices, 0, 2, rawIndices);
        selected_jni::Check(env);
        return ParseSelectedLaunchFields(copied, {static_cast<int32_t>(rawIndices[0]), static_cast<int32_t>(rawIndices[1])});
    }
}
