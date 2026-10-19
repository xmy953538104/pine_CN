// SPDX-License-Identifier: MPL-2.0
// Public synthetic inputs, independent OpenSSL encryption, production scan/JNI.
#include "fixtures.h"
#include <catalog/scan_report.h>
#include <common/exception.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <jni.h>

namespace skyline { std::vector<void *> exception::GetStackFrames() { return {}; } }

namespace {
using namespace fixture;
namespace observed = skyline::catalog::observed;
void Write(const std::filesystem::path &path, const std::string &value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary); stream << value; Check(bool(stream), "fixture write failed");
}
void Write(const std::filesystem::path &path, const Bytes &value) {
    Write(path, std::string(reinterpret_cast<const char *>(value.data()), value.size()));
}
Bytes Container(Bytes cnmt) {
    const auto meta = Nca(1, App, {{true, Pfs({{"PRIVATE_MEMBER_NAME.cnmt", std::move(cnmt)}})}});
    return Pfs({{"PRIVATE_MEMBER_NAME.cnmt.nca", meta}});
}
void Roots(const std::filesystem::path &root) {
    const auto header = HeaderKey(); const auto area = AreaKey();
    const auto headerLine = "header_key=" + Hex(header.data(), header.size()) + "\n";
    Write(root / "full/keys/prod.keys", headerLine + "key_area_key_application_00=" + Hex(area.data(), area.size()) + "\n");
    Write(root / "header/keys/prod.keys", headerLine);
    Write(root / "none/keys/prod.keys", "");
    Write(root / "bad/keys/prod.keys", "PRIVATE_KEY_CONTENT_WITHOUT_EQUALS\n");
    const auto cnmt = Cnmt(0x80, App, 0, {});
    Write(root / "valid.nsp", Container(cnmt));
    Write(root / "empty.nsp", Pfs({}));
    Write(root / "aoc.nsp", Container(Cnmt(0x82, Aoc1, App, {})));
    Write(root / "patch.nsp", Container(Cnmt(0x81, Patch, App, {})));
    auto bad = cnmt; bad.pop_back(); Write(root / "bad-cnmt.nsp", Container(bad));
    auto digest = cnmt; digest.back() ^= 1; Write(root / "digest.nsp", Container(digest));
    Write(root / "bad-pfs.nsp", Bytes(16, 0));
    auto overBudget = Pfs({}); Put(overBudget, 4, 4097, 4);
    Write(root / "entry-budget.nsp", overBudget);
}
std::string Scan(const Bytes &bytes, const std::shared_ptr<crypto::KeyStore> &keys,
                 const Options &options, bool changeBeforeSerialize = false) {
    observed::ScanReport report;
    for (auto stage : {observed::Stage::ProfileValidation, observed::Stage::KeyStoreLoad, observed::Stage::FdDuplicate}) {
        report.Begin(stage); report.Complete(stage);
    }
    auto source = std::make_shared<MutableSource>(bytes);
    std::optional<Catalog> catalog;
    std::string transient;
    try {
        report.Begin(observed::Stage::CatalogScan); catalog = ScanNative(source, keys, options);
        report.Complete(observed::Stage::CatalogScan);
        report.Begin(observed::Stage::CatalogSerialize);
        source->changed = changeBeforeSerialize;
        transient = ToJson(*catalog);
        report.Complete(observed::Stage::CatalogSerialize); report.Finish();
    } catch (const Error &error) { report.Fail(error.code); }
    // Exercise the actual envelope but never persist its raw Catalog part.
    const auto envelope = report.Envelope(transient, catalog ? &*catalog : nullptr);
    Check(envelope.find("\"selected_scan_schema\":1") != std::string::npos, "envelope schema");
    const auto safe = report.ReportJson(catalog ? &*catalog : nullptr);
    Check(safe.size() <= observed::MaxReportBytes && safe.find("PRIVATE") == std::string::npos, "safe report bounds/privacy");
    return safe;
}
void Has(const std::string &report, const char *token) { Check(report.find(token) != std::string::npos, token); }
}

#ifndef PINE_OBSERVED_JNI_LIBRARY
int main(int argc, char **argv) {
    try {
        Check(argc == 3, "roots and evidence arguments required"); Roots(argv[1]);
        const auto full = std::make_shared<crypto::KeyStore>(std::string(argv[1]) + "/full/keys/");
        const auto header = std::make_shared<crypto::KeyStore>(std::string(argv[1]) + "/header/keys/");
        const auto none = std::make_shared<crypto::KeyStore>(std::string(argv[1]) + "/none/keys/");
        auto options = OptionsForTest(); options.verifyHashes = false;
        auto save = [&](const char *name, const std::string &value) { Write(std::filesystem::path(argv[2]) / (std::string(name) + ".json"), value); std::cout << "PASS native " << name << '\n'; };
        const auto cnmt = Cnmt(0x80, App, 0, {}), valid = Container(cnmt);
        auto report = Scan(valid, full, options); Has(report, "\"parsing\":\"parsed\""); Has(report, "\"status\":\"completed\""); save("valid-meta", report);
        report = Scan(valid, header, options); Has(report, "missing_key_area"); Has(report, "\"all_metadata_enumerated\":false"); save("missing-meta-area-key", report);
        report = Scan(valid, none, options); Has(report, "missing_header_key"); save("missing-header-key", report);
        auto malformed = cnmt; malformed.pop_back(); report = Scan(Container(malformed), full, options);
        Has(report, "\"parsing\":\"invalid\""); save("malformed-cnmt", report);
        auto mismatch = cnmt; mismatch.back() ^= 1; report = Scan(Container(mismatch), full, options);
        Has(report, "cnmt_digest_mismatch"); Has(report, "\"digest_observation\":\"mismatch\""); Has(report, "\"parsing\":\"parsed\""); save("digest-observation", report);
        auto limited = options; limited.limits.maxMetadataBytes = 0x1000; report = Scan(valid, full, limited);
        Has(report, "scan_budget"); Has(report, "\"bounded_scan_complete\":false"); save("native-budget", report);
        report = Scan(valid, full, options, true); Has(report, "\"error_code\":\"input_changed\"");
        Has(report, "\"failed_stage\":\"catalog_serialize\""); save("source-changed-serialize", report);
        report = Scan(Pfs({}), full, options); Has(report, "\"group_count\":0"); save("empty-container", report);
        return 0;
    } catch (const std::exception &error) { std::cerr << "synthetic assertion failed: " << error.what() << '\n'; return 1; }
}
#endif

extern "C" jstring Java_emu_skyline_loader_SelectedContentBridge_inspectObserved(JNIEnv *, jclass, jint, jstring, jstring);
extern "C" JNIEXPORT void JNICALL Java_emu_skyline_loader_SelectedScanObservedFixture_prepare(JNIEnv *env, jclass, jstring value) {
    const auto text = env->GetStringUTFChars(value, nullptr); if (!text) return;
    const std::string root(text); env->ReleaseStringUTFChars(value, text); Roots(root);
}
extern "C" JNIEXPORT jint JNICALL Java_emu_skyline_loader_SelectedScanObservedFixture_open(JNIEnv *env, jclass, jstring value) {
    const auto text = env->GetStringUTFChars(value, nullptr); if (!text) return -1;
    const auto fd = ::open(text, O_RDONLY | O_CLOEXEC); env->ReleaseStringUTFChars(value, text); return fd;
}
extern "C" JNIEXPORT void JNICALL Java_emu_skyline_loader_SelectedScanObservedFixture_close(JNIEnv *, jclass, jint fd) { ::close(fd); }
extern "C" JNIEXPORT jboolean JNICALL Java_emu_skyline_loader_SelectedScanObservedFixture_isOpen(JNIEnv *, jclass, jint fd) {
    return ::fcntl(fd, F_GETFD) != -1;
}
extern "C" JNIEXPORT void JNICALL Java_emu_skyline_loader_SelectedScanObservedFixture_pending(JNIEnv *env, jclass) {
    const auto clazz = env->FindClass("java/lang/IllegalStateException");
    if (!clazz) return;
    env->ThrowNew(clazz, "synthetic_pending_exception"); env->DeleteLocalRef(clazz);
    (void)Java_emu_skyline_loader_SelectedContentBridge_inspectObserved(env, nullptr, -1, nullptr, nullptr);
}
