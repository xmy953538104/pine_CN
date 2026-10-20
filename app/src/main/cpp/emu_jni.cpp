// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <string>
#include <algorithm>
#include <array>
#include "skyline/diagnostics/diagnostics.h"
#include "skyline/diagnostics/utf16.h"
#include "skyline/diagnostics/initialization.h"
#include "skyline/diagnostics/identity_jni.h"
#include "skyline/diagnostics/resource_observer.h"
#include "selected_jni.h"
#include "skyline/nce.h"
#include <csignal>
#include <pthread.h>
#include <android/asset_manager_jni.h>
#include <sys/system_properties.h>
#include "skyline/common.h"
#include "skyline/common/language.h"
#include "skyline/common/signal.h"
#include "skyline/common/android_settings.h"
#include "skyline/common/trace.h"
#include "skyline/loader/loader.h"
#include "skyline/vfs/android_asset_filesystem.h"
#include "skyline/os.h"
#include "skyline/jvm.h"
#include "skyline/gpu.h"
#include "skyline/audio.h"
#include "skyline/input.h"
#include "skyline/kernel/types/KProcess.h"
#include "skyline/logger/logger.h"
#include "skyline/soc/gm20b/engines/engine.h"
#include <audio_core/sink/sink_details.h>

jint Fps; //!< An approximation of the amount of frames being submitted every second
jfloat AverageFrametimeMs; //!< The average time it takes for a frame to be rendered and presented in milliseconds
jfloat AverageFrametimeDeviationMs; //!< The average deviation of the average frametimes in milliseconds

std::weak_ptr<skyline::kernel::OS> OsWeak;
std::weak_ptr<skyline::gpu::GPU> GpuWeak;
std::weak_ptr<skyline::audio::Audio> AudioWeak;
std::weak_ptr<skyline::input::Input> InputWeak;
std::weak_ptr<skyline::Settings> SettingsWeak;

// https://cs.android.com/android/platform/superproject/+/master:bionic/libc/tzcode/bionic.cpp;l=43;drc=master;bpv=1;bpt=1
static std::string GetTimeZoneName() {
    const char *nameEnv = getenv("TZ");
    if (nameEnv)
        return std::string(nameEnv);

    char propBuf[PROP_VALUE_MAX];
    if (__system_property_get("persist.sys.timezone", propBuf)) {
        std::string nameProp(propBuf);

        // Flip -/+, see https://cs.android.com/android/platform/superproject/+/master:bionic/libc/tzcode/bionic.cpp;l=53;drc=master;bpv=1;bpt=1
        if (nameProp.size() > 2) {
            if (nameProp[2] == '-')
                nameProp[2] = '+';
            else if (nameProp[2] == '+')
                nameProp[2] = '-';
        }

        return nameProp;
    }

    // Fallback to GMT
    return "GMT";
}

std::string ConvertJStringToString(JNIEnv* env, jstring jStr) {
    if (!jStr) return "";

    const char* chars = env->GetStringUTFChars(jStr, nullptr);
    std::string str(chars);
    env->ReleaseStringUTFChars(jStr, chars);
    return str;
}

// Guest diagnostic text needs standard UTF-8, while existing non-diagnostic
// callers retain their original JNI string handling and limits.
static std::string ConvertDiagnosticJString(JNIEnv* env, jstring jStr) {
    if (!jStr || env->ExceptionCheck()) return {};
    const auto length{std::min<jsize>(env->GetStringLength(jStr), 4096)};
    std::vector<jchar> units(static_cast<size_t>(length));
    if (length) env->GetStringRegion(jStr, 0, length, units.data());
    if (env->ExceptionCheck()) return {};
    return skyline::diagnostics::Utf16ToUtf8(units);
}

// Both Java entry points use the existing lifetime, signal classification and
// FD owner. A selected request never falls back to the ordinary NSP loader.
static void ExecuteApplication(
    JNIEnv *env,
    jobject instance,
    jstring romUriJstring,
    jint romType,
    jint romFd,
    jintArray dlcFds,
    jint updateFd,
    jobject settingsInstance,
    jstring publicAppFilesPathJstring,
    jstring privateAppFilesPathJstring,
    jstring nativeLibraryPathJstring,
    jobject assetManager,
    jstring diagnosticTitleId,
    jlong generationToken,
    bool selected = false,
    jobjectArray selectionFields = nullptr,
    jintArray programIndices = nullptr
) {
    skyline::signal::ScopedStackBlocker stackBlocker; // We do not want anything to unwind past JNI code as there are invalid stack frames which can lead to a segmentation fault
    skyline::diagnostics::ScopedJavaContentIdentityObserver identityObserver(env, generationToken);
    skyline::diagnostics::ClearTitleId();
    // UI identity is an association, available before GPU/OS construction.
    // OS::Execute still replaces it with the actually loaded content identity.
    try { skyline::diagnostics::SetTitleId(ConvertDiagnosticJString(env, diagnosticTitleId)); } catch (...) {}
    Fps = 0;
    AverageFrametimeMs = AverageFrametimeDeviationMs = 0.0f;

    pthread_setname_np(pthread_self(), "EmuMain");
    skyline::diagnostics::Stage("JNI_ENTERED");

    auto jvmManager{skyline::diagnostics::ObserveInitialization("JNI_JVM_MANAGER", [&] { return std::make_shared<skyline::JvmManager>(env, instance); })};

    jsize dlcArrSize = dlcFds != nullptr ? env->GetArrayLength(dlcFds) : 0;
    std::vector<int> dlcFdsVector(dlcArrSize);

    if (dlcArrSize > 0)
        env->GetIntArrayRegion(dlcFds, 0, dlcArrSize, &dlcFdsVector[0]);

    std::shared_ptr<skyline::Settings> settings{skyline::diagnostics::ObserveInitialization("JNI_ANDROID_SETTINGS", [&] { return std::make_shared<skyline::AndroidSettings>(env, settingsInstance); })};

    skyline::JniString publicAppFilesPath(env, publicAppFilesPathJstring);

    skyline::diagnostics::ObserveInitialization("JNI_TEXT_LOGGER", [&] { skyline::AsyncLogger::Initialize(*settings->logLevel, publicAppFilesPath + "logs/emulation.log"); });

    auto start{std::chrono::steady_clock::now()};

    // Initialize tracing
    perfetto::TracingInitArgs args;
    args.backends |= perfetto::kSystemBackend;
    args.shmem_size_hint_kb = 0x200000;
    skyline::diagnostics::ObserveInitialization("JNI_TRACING", [&] {
        perfetto::Tracing::Initialize(args);
        perfetto::TrackEvent::Register();
    });
    std::string_view observedEndReason{"execute_not_returned"};
    try {
        skyline::JniString nativeLibraryPath(env, nativeLibraryPathJstring);
        skyline::JniString privateAppFilesPath{env, privateAppFilesPathJstring};

        // Host signal handlers need to be set before NCE is initialized, this is the only place where we can do that
        skyline::signal::SetHostSignalHandler({SIGINT, SIGILL, SIGTRAP, SIGBUS, SIGFPE, SIGSEGV}, skyline::signal::ExceptionalSignalHandler);
        auto os{skyline::diagnostics::ObserveInitialization("OS_CONSTRUCT", [&] { return std::make_shared<skyline::kernel::OS>(
            jvmManager,
            settings,
            publicAppFilesPath,
            privateAppFilesPath,
            nativeLibraryPath,
            GetTimeZoneName(),
            std::make_shared<skyline::vfs::AndroidAssetFileSystem>(AAssetManager_fromJava(env, assetManager))
        ); })};
        skyline::diagnostics::Stage("OS_INITIALIZED");
        OsWeak = os;
        GpuWeak = os->state.gpu;
        AudioWeak = os->state.audio;
        InputWeak = os->state.input;
        SettingsWeak = settings;
        skyline::diagnostics::ObserveInitialization("CONTROLLERS_INITIALIZE", [&] { jvmManager->InitializeControllers(); });

        LOGDNF("Launching ROM {}", skyline::JniString(env, romUriJstring));

        skyline::diagnostics::Emit("lifecycle", "STAGE_STARTED", {{"stage", "OS_EXECUTE"}});
        if (selected)
            skyline::ExecuteSelectedFromJava(env, *os, romFd, selectionFields, programIndices);
        else
            os->Execute(romFd, dlcFdsVector, updateFd, static_cast<skyline::loader::RomFormat>(romType));
        observedEndReason = "execute_returned";
        skyline::diagnostics::Emit("lifecycle", "EMULATION_RETURNED");
    } catch (std::exception &e) {
        observedEndReason = dynamic_cast<const skyline::nce::NCE::ExitException *>(&e) ? "exit_exception" : "cpp_exception";
        if (!dynamic_cast<const skyline::nce::NCE::ExitException *>(&e))
            skyline::diagnostics::Emit("exception", "UNHANDLED_CPP_EXCEPTION", {{"message", e.what()}}, "ERROR");
        else
            skyline::diagnostics::Emit("lifecycle", "EMULATION_CANCELLED", {{"reason", "ExitException"}});
        skyline::diagnostics::Flush(250);
        LOGENF("An uncaught exception has occurred: {}", e.what());
    } catch (const skyline::signal::SignalException &e) {
        observedEndReason = e.signal == SIGINT ? "sigint_stop" : "native_signal_exception";
        if (e.signal != SIGINT)
            skyline::diagnostics::Emit("exception", "NATIVE_SIGNAL_EXCEPTION", {{"message", e.what()}}, "ERROR");
        else
            skyline::diagnostics::Emit("lifecycle", "EMULATION_CANCELLED", {{"reason", "SIGINT"}});
        skyline::diagnostics::Flush(250);
        LOGENF("An uncaught signal exception has occurred: {}", e.what());
    } catch (...) {
        observedEndReason = "unknown_exception";
        skyline::diagnostics::Emit("exception", "UNKNOWN_EXCEPTION", {}, "ERROR");
        skyline::diagnostics::Flush(250);
        LOGENF("An unknown uncaught exception has occurred");
    }

    skyline::diagnostics::EndLaunch(observedEndReason);
    perfetto::TrackEvent::Flush();

    InputWeak.reset();

    auto end{std::chrono::steady_clock::now()};
    LOGINF("Emulation has ended in {}ms", std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());

    skyline::diagnostics::Stage("EMULATION_ENDED");
    skyline::diagnostics::Stop();
    skyline::diagnostics::ClearTitleId();
    skyline::diagnostics::SetLaunchId({});
    skyline::AsyncLogger::Finalize(true);
    close(romFd);

    close(updateFd);

    if (dlcArrSize > 0)
        for (int i = 0; i < dlcArrSize; i++)
            close(env->GetIntArrayElements(dlcFds, nullptr)[i]);

}

extern "C" JNIEXPORT void Java_emu_skyline_EmulationActivity_executeApplication(
    JNIEnv *env, jobject instance, jstring romUri, jint romType, jint romFd,
    jintArray dlcFds, jint updateFd, jobject settings, jstring publicPath,
    jstring privatePath, jstring libraryPath, jobject assets,
    jstring diagnosticTitleId, jlong generationToken) {
    ExecuteApplication(env, instance, romUri, romType, romFd, dlcFds, updateFd,
                       settings, publicPath, privatePath, libraryPath, assets,
                       diagnosticTitleId, generationToken);
}

extern "C" JNIEXPORT void Java_emu_skyline_EmulationActivity_executeSelectedApplication(
    JNIEnv *env, jobject instance, jstring romUri, jint romFd,
    jobjectArray selectionFields, jintArray programIndices, jobject settings,
    jstring publicPath, jstring privatePath, jstring libraryPath, jobject assets,
    jstring diagnosticTitleId, jlong generationToken) {
    ExecuteApplication(env, instance, romUri, static_cast<jint>(skyline::loader::RomFormat::NSP),
                       romFd, nullptr, -1, settings, publicPath, privatePath, libraryPath,
                       assets, diagnosticTitleId, generationToken, true, selectionFields, programIndices);
}

extern "C" JNIEXPORT jboolean Java_emu_skyline_EmulationActivity_stopEmulation(JNIEnv *, jobject, jboolean join) {
    auto os{OsWeak.lock()};
    if (!os)
        return false;
    auto process{os->state.process};
    if (!process)
        return false;
    skyline::diagnostics::RequestLaunchStop();
    process->Kill(join, false, true);
    return true;
}

extern "C" JNIEXPORT jboolean Java_emu_skyline_EmulationActivity_setSurface(JNIEnv *, jobject, jobject surface) {
    auto gpu{GpuWeak.lock()};
    if (!gpu)
        return false;
    gpu->presentation.UpdateSurface(surface);
    return true;
}

extern "C" JNIEXPORT void Java_emu_skyline_EmulationActivity_pauseEmulation(JNIEnv *, jobject, jboolean pause) {
    auto gpu = GpuWeak.lock();
    if (!gpu) return;
    if (pause) {
        gpu->presentation.Pause();
    } else gpu->presentation.Resume();
    skyline::diagnostics::ResourcePresentationPaused(pause);
}

extern "C" JNIEXPORT void Java_emu_skyline_EmulationActivity_changeAudioStatus(JNIEnv *, jobject, jboolean play) {
    auto audio{AudioWeak.lock()};
    if (audio)
        if (play)
            audio->Resume();
        else
            audio->Pause();
}

extern "C" JNIEXPORT void Java_emu_skyline_EmulationActivity_updatePerformanceStatistics(JNIEnv *env, jobject thiz) {
    static jclass clazz{};
    if (!clazz)
        clazz = env->GetObjectClass(thiz);

    static jfieldID fpsField{};
    if (!fpsField)
        fpsField = env->GetFieldID(clazz, "fps", "I");
    env->SetIntField(thiz, fpsField, Fps);

    static jfieldID averageFrametimeField{};
    if (!averageFrametimeField)
        averageFrametimeField = env->GetFieldID(clazz, "averageFrametime", "F");
    env->SetFloatField(thiz, averageFrametimeField, AverageFrametimeMs);

    static jfieldID averageFrametimeDeviationField{};
    if (!averageFrametimeDeviationField)
        averageFrametimeDeviationField = env->GetFieldID(clazz, "averageFrametimeDeviation", "F");
    env->SetFloatField(thiz, averageFrametimeDeviationField, AverageFrametimeDeviationMs);
}

extern "C" JNIEXPORT void JNICALL Java_emu_skyline_input_InputHandler_00024Companion_setController(JNIEnv *, jobject, jint index, jint type, jint partnerIndex) {
    auto input{InputWeak.lock()};
    std::lock_guard guard(input->npad.mutex);
    input->npad.controllers[static_cast<size_t>(index)] = skyline::input::GuestController{static_cast<skyline::input::NpadControllerType>(type), static_cast<skyline::i8>(partnerIndex)};
}

extern "C" JNIEXPORT void JNICALL Java_emu_skyline_input_InputHandler_00024Companion_updateControllers(JNIEnv *, jobject) {
    InputWeak.lock()->npad.Update();
}

extern "C" JNIEXPORT void JNICALL Java_emu_skyline_input_InputHandler_00024Companion_setButtonState(JNIEnv *, jobject, jint index, jlong mask, jboolean pressed) {
    auto input{InputWeak.lock()};
    if (!input)
        return; // We don't mind if we miss button updates while input hasn't been initialized
    auto device{input->npad.controllers[static_cast<size_t>(index)].device};
    if (device)
        device->SetButtonState(skyline::input::NpadButton{.raw = static_cast<skyline::u64>(mask)}, pressed);
}

extern "C" JNIEXPORT void JNICALL Java_emu_skyline_input_InputHandler_00024Companion_setAxisValue(JNIEnv *, jobject, jint index, jint axis, jint value) {
    auto input{InputWeak.lock()};
    if (!input)
        return; // We don't mind if we miss axis updates while input hasn't been initialized
    auto device{input->npad.controllers[static_cast<size_t>(index)].device};
    if (device)
        device->SetAxisValue(static_cast<skyline::input::NpadAxisId>(axis), value);
}

extern "C" JNIEXPORT void JNICALL Java_emu_skyline_input_InputHandler_00024Companion_setMotionState(JNIEnv *env, jobject, jint index, jint motionId, jobject value) {
    auto input{InputWeak.lock()};
    if (!input)
        return; // We don't mind if we miss motion updates while input hasn't been initialized

    const auto motionValue = reinterpret_cast<skyline::input::MotionSensorState*>(env->GetDirectBufferAddress(value));

    auto device{input->npad.controllers[static_cast<size_t>(index)].device};
    if (device)
        device->SetMotionValue(static_cast<skyline::input::MotionId>(motionId), motionValue);
}

extern "C" JNIEXPORT void JNICALL Java_emu_skyline_input_InputHandler_00024Companion_setTouchState(JNIEnv *env, jobject, jintArray pointsJni) {
    using Point = skyline::input::TouchScreenPoint;

    auto input{InputWeak.lock()};
    if (!input)
        return; // We don't mind if we miss touch updates while input hasn't been initialized
    jboolean isCopy{false};

    skyline::span<Point> points(reinterpret_cast<Point *>(env->GetIntArrayElements(pointsJni, &isCopy)),
                                static_cast<size_t>(env->GetArrayLength(pointsJni)) / (sizeof(Point) / sizeof(jint)));
    input->touch.SetState(points);
    env->ReleaseIntArrayElements(pointsJni, reinterpret_cast<jint *>(points.data()), JNI_ABORT);
}

extern "C" JNIEXPORT void JNICALL Java_emu_skyline_settings_NativeSettings_updateNative(JNIEnv *env, jobject) {
    auto settings{SettingsWeak.lock()};
    if (!settings)
        return; // We don't mind if we miss settings updates while settings haven't been initialized
    settings->Update();
}

extern "C" JNIEXPORT void JNICALL Java_emu_skyline_EmulationActivity_enableDynamicResolution(JNIEnv *env, jobject obj, jboolean enable) {
    skyline::soc::gm20b::engine::enableDynamicResolution(enable);
}

extern "C" JNIEXPORT void JNICALL Java_emu_skyline_EmulationActivity_enableJit(JNIEnv *env, jobject obj, jboolean enable) {
    skyline::kernel::isJitEnabled = enable;
}

extern "C" JNIEXPORT void JNICALL Java_emu_skyline_EmulationActivity_setAudioSink(JNIEnv *env, jobject obj, jstring jSink) {
    std::string sink = ConvertJStringToString(env, jSink);
    AudioCore::Sink::AudioSink = sink;
}

extern "C" JNIEXPORT void JNICALL Java_emu_skyline_EmulationActivity_setAnisotropicFilter(JNIEnv *env, jobject obj, jfloat jAnisotropy) {
    skyline::gpu::interconnect::preferredAnisotropy = static_cast<float>(jAnisotropy);
}

// Diagnostics lifecycle calls run only on normal Java threads, never signal handlers.
extern "C" JNIEXPORT jboolean JNICALL Java_emu_skyline_diagnostics_Diagnostics_configureNative(
    JNIEnv *env, jclass, jstring path, jboolean enabled, jboolean verbose, jint maxFiles, jstring session, jstring categories, jstring processRunId) {
    try {
    skyline::diagnostics::ConfigureResourceObservation(false, "unspecified");
    skyline::diagnostics::Stop();
    if (!enabled) return false;
    skyline::diagnostics::Config config;
    config.directory = ConvertDiagnosticJString(env, path);
    config.sessionId = ConvertDiagnosticJString(env, session);
    config.verbose = verbose;
    config.categories = ConvertDiagnosticJString(env, categories);
    config.processRunId = ConvertDiagnosticJString(env, processRunId);
    config.maxFiles = static_cast<size_t>(std::clamp(maxFiles, 2, 8));
    if (env->ExceptionCheck()) return false;
    return skyline::diagnostics::Start(config);
    } catch (...) { return false; }
}

extern "C" JNIEXPORT void JNICALL Java_emu_skyline_diagnostics_Diagnostics_setLaunchNative(
    JNIEnv *env, jclass, jstring launchId, jstring applicationTitleId, jboolean newLaunch) {
    try {
        const auto launch = ConvertDiagnosticJString(env, launchId);
        const auto title = ConvertDiagnosticJString(env, applicationTitleId);
        if (env->ExceptionCheck()) return;
        if (newLaunch) skyline::diagnostics::ClearTitleId();
        skyline::diagnostics::SetLaunchId(launch, newLaunch);
        if (newLaunch) skyline::diagnostics::SetTitleId(title);
    } catch (...) {}
}

extern "C" JNIEXPORT void JNICALL Java_emu_skyline_diagnostics_Diagnostics_emitNative(
    JNIEnv *env, jclass, jstring category, jstring event, jstring detail, jstring titleId) {
    try {
    if (!skyline::diagnostics::Enabled()) return;
    const auto categoryText{ConvertDiagnosticJString(env, category)};
    const auto eventText{ConvertDiagnosticJString(env, event)};
    const auto detailText{ConvertDiagnosticJString(env, detail)};
    const auto titleText{ConvertDiagnosticJString(env, titleId)};
    if (!env->ExceptionCheck())
        skyline::diagnostics::Emit(categoryText, eventText, {{"detail", detailText}, {"title_id", titleText}},
                                   eventText == "ANDROID_UNCAUGHT_EXCEPTION" || eventText == "PREVIOUS_PROCESS_CRASH" || eventText == "CONTENT_OPEN_FAILED" || eventText == "CONTENT_METADATA_FAILED" ? "ERROR" : "INFO");
    } catch (...) { /* Diagnostics must never change guest error propagation. */ }
}

extern "C" JNIEXPORT jboolean JNICALL Java_emu_skyline_diagnostics_Diagnostics_emitFieldsNative(
    JNIEnv *env, jclass, jstring category, jstring event, jstring level, jobjectArray keys, jobjectArray values) {
    try {
        if (!skyline::diagnostics::Enabled() || !keys || !values) return false;
        const auto count = env->GetArrayLength(keys);
        if (env->ExceptionCheck() || count > 24 || env->GetArrayLength(values) != count || env->ExceptionCheck()) return false;
        const auto categoryText = ConvertDiagnosticJString(env, category);
        const auto eventText = ConvertDiagnosticJString(env, event);
        const auto levelText = ConvertDiagnosticJString(env, level);
        std::array<std::string, 24> ownedKeys, ownedValues;
        std::array<std::pair<std::string_view, std::string_view>, 24> fields;
        for (jsize i = 0; i < count; ++i) {
            const auto index = static_cast<size_t>(i);
            auto key = static_cast<jstring>(env->GetObjectArrayElement(keys, i));
            if (env->ExceptionCheck()) return false;
            auto value = static_cast<jstring>(env->GetObjectArrayElement(values, i));
            if (env->ExceptionCheck()) { if (key) env->DeleteLocalRef(key); return false; }
            ownedKeys[index] = ConvertDiagnosticJString(env, key);
            ownedValues[index] = ConvertDiagnosticJString(env, value);
            if (key) env->DeleteLocalRef(key);
            if (value) env->DeleteLocalRef(value);
            if (env->ExceptionCheck()) return false;
            fields[index] = {ownedKeys[index], ownedValues[index]};
        }
        if (env->ExceptionCheck()) return false;
        return skyline::diagnostics::EmitFields(categoryText, eventText, {fields.data(), static_cast<size_t>(count)}, levelText);
    } catch (...) { return false; }
}

extern "C" JNIEXPORT void JNICALL Java_emu_skyline_diagnostics_Diagnostics_configureResourceNative(
    JNIEnv *env, jclass, jboolean enabled, jstring cohort) {
    try {
        const auto label = ConvertDiagnosticJString(env, cohort);
        if (!env->ExceptionCheck()) skyline::diagnostics::ConfigureResourceObservation(enabled, label.c_str());
    } catch (...) {}
}

extern "C" JNIEXPORT jboolean JNICALL Java_emu_skyline_diagnostics_Diagnostics_emitHostResourcesNative(
    JNIEnv *env, jclass, jstring session, jstring launch, jobjectArray keys, jobjectArray values) {
    try {
        if (!skyline::diagnostics::ResourceObservationEnabled() || !keys || !values) return false;
        const auto count = env->GetArrayLength(keys);
        if (env->ExceptionCheck() || count > 16 || env->GetArrayLength(values) != count || env->ExceptionCheck()) return false;
        const auto sessionText = ConvertDiagnosticJString(env, session);
        const auto launchText = ConvertDiagnosticJString(env, launch);
        std::array<std::string, 16> ownedKeys, ownedValues;
        std::array<std::pair<std::string_view, std::string_view>, 16> fields;
        for (jsize i = 0; i < count; ++i) {
            auto key = static_cast<jstring>(env->GetObjectArrayElement(keys, i));
            if (env->ExceptionCheck()) return false;
            auto value = static_cast<jstring>(env->GetObjectArrayElement(values, i));
            if (env->ExceptionCheck()) { if (key) env->DeleteLocalRef(key); return false; }
            ownedKeys[i] = ConvertDiagnosticJString(env, key);
            ownedValues[i] = ConvertDiagnosticJString(env, value);
            if (key) env->DeleteLocalRef(key);
            if (value) env->DeleteLocalRef(value);
            if (env->ExceptionCheck()) return false;
            fields[i] = {ownedKeys[i], ownedValues[i]};
        }
        if (env->ExceptionCheck()) return false;
        return skyline::diagnostics::EmitHostResourceFields(sessionText, launchText, {fields.data(), static_cast<size_t>(count)});
    } catch (...) { return false; }
}

extern "C" JNIEXPORT jlongArray JNICALL Java_emu_skyline_diagnostics_Diagnostics_statusNative(JNIEnv *env, jclass) {
    const auto stats = skyline::diagnostics::Stats();
    const jlong values[] = {skyline::diagnostics::Enabled() ? 1 : 0, static_cast<jlong>(stats.written), static_cast<jlong>(stats.dropped), static_cast<jlong>(stats.diskErrors)};
    auto result = env->NewLongArray(4);
    if (result) env->SetLongArrayRegion(result, 0, 4, values);
    return result;
}

extern "C" JNIEXPORT jboolean JNICALL Java_emu_skyline_diagnostics_Diagnostics_flushNative(JNIEnv *, jclass) {
    return skyline::diagnostics::Flush(250);
}

extern "C" JNIEXPORT void JNICALL Java_emu_skyline_diagnostics_Diagnostics_stopNative(JNIEnv *, jclass) {
    skyline::diagnostics::Stop();
}

extern "C" JNIEXPORT void JNICALL Java_emu_skyline_diagnostics_Diagnostics_observeHostExitNative(JNIEnv *env, jclass, jstring source) {
    try { skyline::diagnostics::ObserveHostExitRequest(ConvertDiagnosticJString(env, source)); } catch (...) {}
}
