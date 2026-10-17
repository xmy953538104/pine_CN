// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <jni.h>
#include <diagnostics/content.h>
namespace skyline::loader {
inline void MetadataString(JNIEnv *env, jobject target, jclass clazz, const char *name, const std::string &value) {
    if (env->ExceptionCheck()) return;
    const auto field = env->GetFieldID(clazz, name, "Ljava/lang/String;");
    if (!field || env->ExceptionCheck()) return;
    const auto string = env->NewStringUTF(value.c_str());
    if (!string || env->ExceptionCheck()) return;
    env->SetObjectField(target, field, string);
    env->DeleteLocalRef(string);
}
// This is the actual legacy+diagnostic JNI identity transfer used by populate().
// Controlled host JNI tests use the same getters/setters and the real Kotlin initializer.
template<class Loader> void PopulateLoaderTitleFields(JNIEnv *env, jobject target, jclass clazz, Loader &loader, bool diagnosticsEnabled) {
    if (loader.nacp) MetadataString(env, target, clazz, "applicationTitleId", loader.nacp->GetSaveDataOwnerId());
    if (env->ExceptionCheck()) return;
    if (loader.cnmt) {
        const auto type = static_cast<unsigned>(loader.cnmt->GetContentMetaType());
        const auto field = env->GetFieldID(clazz, "romTypeInt", "I");
        if (!field || env->ExceptionCheck()) return;
        env->SetIntField(target, field, static_cast<jint>(type));
        if (type != 0x80) MetadataString(env, target, clazz, "parentTitleId", loader.cnmt->GetParentTitleId());
    }
    if (!diagnosticsEnabled || env->ExceptionCheck()) return;
    try {
        const auto identity = diagnostics::DescribeContentIdentity(loader);
        MetadataString(env, target, clazz, "diagnosticApplicationId", identity.application);
        MetadataString(env, target, clazz, "diagnosticProgramId", identity.program);
        MetadataString(env, target, clazz, "diagnosticCnmtId", identity.cnmt);
        MetadataString(env, target, clazz, "diagnosticParentId", identity.parent);
        MetadataString(env, target, clazz, "diagnosticSaveOwnerId", identity.saveOwner);
        MetadataString(env, target, clazz, "diagnosticProgramContentId", identity.programContent);
        MetadataString(env, target, clazz, "diagnosticCnmtVersion", identity.cnmtVersion);
        MetadataString(env, target, clazz, "diagnosticIdentitySource", identity.source);
        // Only new diagnostic transfer failures are contained. A pre-existing
        // legacy/native pending exception was checked before entering this block.
        if (env->ExceptionCheck()) env->ExceptionClear();
    } catch (...) { if (env->ExceptionCheck()) env->ExceptionClear(); }
}
}
