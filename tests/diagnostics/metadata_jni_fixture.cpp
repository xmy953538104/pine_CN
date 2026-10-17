// SPDX-License-Identifier: MPL-2.0
// Controlled metadata, production JNI field transfer. Not an NCA parser or a commercial ROM.
#include <loader/metadata_jni.h>
#include <diagnostics/identity_jni.h>
#include <cstdint>
#include <optional>
struct Program { struct Header { std::uint64_t titleId; } header; };
struct Control { std::string GetSaveDataOwnerId() { return "0100000000001111"; } };
struct Meta {
    struct Header { unsigned version{655360}; } header;
    unsigned type;
    unsigned GetContentMetaType() { return type; }
    std::string GetTitleId() { return type == 0x82 ? "0100751007ADB001" : "0100751007ADA800"; }
    std::string GetParentTitleId() { return "0100751007ADA000"; }
};
struct Loader {
    std::optional<Program> programNca;
    std::optional<Control> nacp{Control{}};
    std::optional<Meta> cnmt;
    std::string diagnosticProgramContentId;
};
static Loader ControlledLoader(jint mode) {
    Loader loader;
    if (mode != 2) { loader.programNca = Program{{0x0100751007ADA001}}; loader.diagnosticProgramContentId = std::string(32, 'a'); }
    if (mode == 0) loader.cnmt = Meta{{655360}, 0x81};
    if (mode == 3) { loader.cnmt = Meta{{655360}, 0x82}; loader.programNca.reset(); loader.diagnosticProgramContentId.clear(); }
    return loader;
}
extern "C" JNIEXPORT jint JNICALL Java_emu_skyline_loader_MetadataNativeBridge_populate(JNIEnv *env, jclass, jobject target, jint mode) {
    auto loader = ControlledLoader(mode);
    const auto clazz = env->GetObjectClass(target);
    skyline::loader::PopulateLoaderTitleFields(env, target, clazz, loader, true);
    env->DeleteLocalRef(clazz);
    return env->ExceptionCheck() ? 1 : 0;
}

extern "C" JNIEXPORT void JNICALL Java_emu_skyline_loader_MetadataNativeBridge_resolveForLaunch(JNIEnv *env, jclass, jclass diagnosticsClass, jlong owner, jint mode) {
    auto loader = ControlledLoader(mode);
    skyline::diagnostics::ScopedJavaContentIdentityObserver observer(env, owner, diagnosticsClass);
    const auto identity = skyline::diagnostics::DescribeContentIdentity(loader);
    skyline::diagnostics::NotifyContentIdentity(identity.application, identity.source);
}
