// SPDX-License-Identifier: MPL-2.0
// Separate observed entry: the existing inspect JNI implementation is unchanged.
#include <jni.h>
#ifdef PINE_ENABLE_SELECTED_CONTENT
#include <algorithm>
#include "skyline/loader/selected_launch_jni.h"
#include "skyline/catalog/native_adapter.h"
#include "skyline/catalog/scan_report.h"
#include "skyline/crypto/key_store.h"
#include "skyline/diagnostics/utf16.h"

namespace {
    skyline::catalog::Options InspectionOptions(skyline::catalog::ContentInfoProfile profile) {
        skyline::catalog::Options options;
        options.profile = profile;
        // Same metadata-preview policy as the compatibility inspect entry.
        options.verifyHashes = false;
        options.limits.maxPfsEntries = 4096;
        options.limits.maxCnmtFiles = 256;
        options.limits.maxContentReferences = 16384;
        options.limits.maxMetaReferences = 4096;
        options.limits.maxCnmtBytes = 4 * 1024 * 1024;
        options.limits.maxMetadataBytes = 32ULL * 1024 * 1024;
        return options;
    }
    skyline::catalog::ContentInfoProfile Profile(const std::string &value) {
        if (value == "modern-40") return skyline::catalog::ContentInfoProfile::Modern40;
        if (value == "legacy-48") return skyline::catalog::ContentInfoProfile::Legacy48;
        throw skyline::catalog::Error("selected_profile_required", "explicit content-info profile required");
    }
    std::string PrivatePath(JNIEnv *env, jstring value) {
        skyline::loader::selected_jni::Check(env);
        if (!value) throw skyline::catalog::Error("selected_private_path_required", "app private path required");
        const auto length = env->GetStringLength(value);
        skyline::loader::selected_jni::Check(env);
        if (length < 1 || length > 4096)
            throw skyline::catalog::Error("selected_private_path_invalid", "invalid app private path");
        std::vector<jchar> units(static_cast<size_t>(length));
        env->GetStringRegion(value, 0, length, units.data());
        skyline::loader::selected_jni::Check(env);
        if (units.front() != '/' || std::find(units.begin(), units.end(), 0) != units.end())
            throw skyline::catalog::Error("selected_private_path_invalid", "invalid app private path");
        auto path = skyline::diagnostics::Utf16ToUtf8(units);
        if (path.back() != '/') path += '/';
        return path;
    }
}
#endif
namespace {
    void ThrowObserved(JNIEnv *env, const char *code) noexcept {
        if (env->ExceptionCheck()) return;
        const auto clazz = env->FindClass("java/lang/IllegalStateException");
        if (clazz) { env->ThrowNew(clazz, code); env->DeleteLocalRef(clazz); }
    }
}
extern "C" JNIEXPORT jstring JNICALL Java_emu_skyline_loader_SelectedContentBridge_inspectObserved(
    JNIEnv *env, jclass, jint borrowedFd, jstring privatePath, jstring profile) {
    // No JNI allocation or helper call is allowed to replace a pending VM error.
    if (env->ExceptionCheck()) return nullptr;
#ifdef PINE_ENABLE_SELECTED_CONTENT
    using namespace skyline::catalog;
    observed::ScanReport report;
    std::optional<Catalog> catalog;
    std::string transientJson;
    try {
        report.Begin(observed::Stage::ProfileValidation);
        skyline::loader::selected_jni::LocalRef stringClass(env, env->FindClass("java/lang/String"));
        skyline::loader::selected_jni::Check(env);
        if (!stringClass.Get()) throw Error("other_native_issue", "JNI class unavailable");
        const auto selectedProfile = Profile(skyline::loader::selected_jni::Ascii(env, profile, static_cast<jclass>(stringClass.Get())));
        report.Complete(observed::Stage::ProfileValidation);
        report.Begin(observed::Stage::KeyStoreLoad);
        // Existing app-private key directory only; no new policy, search,
        // ticket import, key generation, logging or authorization decision.
        auto keys = std::make_shared<skyline::crypto::KeyStore>(PrivatePath(env, privatePath) + "keys/");
        report.Complete(observed::Stage::KeyStoreLoad);
        report.Begin(observed::Stage::FdDuplicate);
        auto source = DuplicateFd(borrowedFd);
        report.Complete(observed::Stage::FdDuplicate);
        report.Begin(observed::Stage::CatalogScan);
        catalog = ScanNative(source, keys, InspectionOptions(selectedProfile));
        report.Complete(observed::Stage::CatalogScan);
        report.Begin(observed::Stage::CatalogSerialize);
        transientJson = ToJson(*catalog);
        if (transientJson.size() > 4 * 1024 * 1024)
            throw Error("selected_preview_limit", "catalog preview exceeds limit");
        report.Complete(observed::Stage::CatalogSerialize);
        report.Finish();
    } catch (const Error &error) {
        if (env->ExceptionCheck()) return nullptr;
        report.Fail(error.code);
    } catch (...) {
        if (env->ExceptionCheck()) return nullptr;
        report.Fail("other_native_issue");
    }
    try {
        const auto envelope = report.Envelope(transientJson, catalog ? &*catalog : nullptr);
        if (env->ExceptionCheck()) return nullptr;
        // ASCII only. NewStringUTF failures leave the actual pending VM error.
        return env->NewStringUTF(envelope.c_str());
    } catch (...) {
        ThrowObserved(env, "other_native_issue");
    }
#else
    ThrowObserved(env, "selected_content_unavailable");
#endif
    return nullptr;
}
