// SPDX-License-Identifier: MPL-2.0
#include "selected_jni.h"
#include <stdexcept>

#ifdef PINE_ENABLE_SELECTED_CONTENT
#include <algorithm>
#include "skyline/loader/selected_launch_jni.h"
#include "skyline/loader/selected_content.h"
#include "skyline/catalog/native_adapter.h"
#include "skyline/diagnostics/utf16.h"
#include "skyline/diagnostics/content.h"
#include "skyline/os.h"

namespace {
    skyline::catalog::Options InspectionOptions(skyline::catalog::ContentInfoProfile profile) {
        skyline::catalog::Options options;
        options.profile = profile;
        // Metadata preview only. Runtime preparation independently re-scans and
        // hashes the same owned source before mutating the loader state.
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
    void ThrowInspection(JNIEnv *env, const char *code) noexcept {
        // Neither keys, exception text nor private file/ROM paths cross this UI
        // error boundary. Preserve any pending VM exception (including OOM).
        if (env->ExceptionCheck()) return;
        const auto clazz = env->FindClass("java/lang/IllegalStateException");
        if (clazz) { env->ThrowNew(clazz, code); env->DeleteLocalRef(clazz); }
    }
}
#endif

extern "C" JNIEXPORT jboolean JNICALL Java_emu_skyline_loader_SelectedContentBridge_isAvailable(JNIEnv *, jclass) {
#ifdef PINE_ENABLE_SELECTED_CONTENT
    return JNI_TRUE;
#else
    return JNI_FALSE;
#endif
}

extern "C" JNIEXPORT jstring JNICALL Java_emu_skyline_loader_SelectedContentBridge_inspect(
    JNIEnv *env, jclass, jint borrowedFd, jstring privatePath, jstring profile) {
#ifdef PINE_ENABLE_SELECTED_CONTENT
    try {
        skyline::loader::selected_jni::LocalRef stringClass(env, env->FindClass("java/lang/String"));
        skyline::loader::selected_jni::Check(env);
        if (!stringClass.Get()) throw std::runtime_error("JNI class unavailable");
        const auto selectedProfile = Profile(skyline::loader::selected_jni::Ascii(env, profile, static_cast<jclass>(stringClass.Get())));
        // Uses only the app's existing private key directory, as ordinary OS::Execute
        // does. No filesystem search, ticket import, key generation or logging.
        auto keys = std::make_shared<skyline::crypto::KeyStore>(PrivatePath(env, privatePath) + "keys/");
        auto source = skyline::catalog::DuplicateFd(borrowedFd);
        auto catalog = skyline::catalog::ScanNative(source, keys, InspectionOptions(selectedProfile));
        const auto json = skyline::catalog::ToJson(catalog);
        if (json.size() > 4 * 1024 * 1024)
            throw skyline::catalog::Error("selected_preview_limit", "catalog preview exceeds limit");
        // Catalog JSON escapes every non-ASCII input byte; NewStringUTF receives
        // only ASCII JSON, not guest UTF-8 or a private path.
        return env->NewStringUTF(json.c_str());
    } catch (const skyline::catalog::Error &error) {
        ThrowInspection(env, error.code.c_str());
    } catch (...) {
        ThrowInspection(env, "selected_inspection_failed");
    }
#else
    const auto clazz = env->FindClass("java/lang/IllegalStateException");
    if (clazz) { env->ThrowNew(clazz, "selected_content_unavailable"); env->DeleteLocalRef(clazz); }
#endif
    return nullptr;
}

namespace skyline {
    void ExecuteSelectedFromJava(JNIEnv *env, kernel::OS &os, int borrowedFd, jobjectArray fields, jintArray indices) {
#ifdef PINE_ENABLE_SELECTED_CONTENT
        try {
            const auto wire = loader::ParseSelectedLaunchJni(env, fields, indices);
            loader::SelectionRequest request;
            request.mode = wire.standalone ? loader::SelectedMode::StandaloneRebuiltPatch : loader::SelectedMode::Application;
            request.profile = wire.profile;
            request.applicationId = wire.applicationId;
            const auto program = [](const loader::SelectedLaunchProgram &value) {
                return loader::ProgramChoice{value.groupId, value.contentId, value.programIndex, value.programId};
            };
            request.primary = program(wire.primary);
            if (wire.update) request.update = program(*wire.update);
            request.addOnGroups = wire.addOnGroups;
            auto source = catalog::DuplicateFd(borrowedFd);
            auto keys = std::make_shared<crypto::KeyStore>(os.privateAppFilesPath + "keys/");
            diagnostics::Stage("SELECTED_CATALOG_SCAN");
            auto catalog = catalog::ScanNative(source, keys, InspectionOptions(request.profile));
            PINE_DIAG_CONTENT("SELECTED_LAUNCH_REQUESTED", {"mode", wire.standalone ? "standalone_patch" : "application"},
                {"application_id", catalog::Hex64(wire.applicationId)}, {"primary_group", wire.primary.groupId},
                {"update_group", wire.update ? wire.update->groupId : ""}, {"aoc_count", std::to_string(wire.addOnGroups.size())},
                {"status", "validation_pending"});
            os.ExecuteSelectedContent(catalog, request, std::move(keys));
        } catch (const catalog::Error &error) {
            PINE_DIAG_CONTENT_ERROR("SELECTED_LAUNCH_FAILED", {"error_code", error.code},
                {"last_stage", diagnostics::LastStage()}, {"status", "rejected_or_failed_no_legacy_fallback"});
            throw;
        }
#else
        throw std::runtime_error("selected_content_unavailable");
#endif
    }
}
