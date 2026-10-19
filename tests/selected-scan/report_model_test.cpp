// SPDX-License-Identifier: MPL-2.0
#include <catalog/scan_report.h>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace skyline::catalog;
using namespace skyline::catalog::observed;
namespace {
uint64_t now{};
uint64_t Clock() noexcept { return now; }
void Check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
ScanReport CompletedReport() {
    ScanReport report(Clock);
    for (unsigned i{}; i < 5; ++i) {
        report.Begin(static_cast<Stage>(i));
        Check(report.Phases().back().status == PhaseStatus::Begun, "phase completed before operation returned");
        now += 7;
        report.Complete(static_cast<Stage>(i));
        Check(report.Phases().back().durationUs == 7, "phase monotonic duration differs");
    }
    report.Finish(); return report;
}
void Write(const std::filesystem::path &root, const char *name, const std::string &json) { std::ofstream(root / name) << json << '\n'; }
}
int main(int argc, char **argv) {
    try {
        Check(argc == 2, "evidence directory required"); const std::filesystem::path root(argv[1]); std::filesystem::create_directories(root);
        auto completed = CompletedReport(); Catalog empty;
        Write(root, "model-empty.json", completed.ReportJson(&empty));
        ScanReport failed(Clock); failed.Begin(Stage::ProfileValidation);
        { std::string temporary = "scan_budget"; failed.Fail(temporary); }
        std::string overwrite(10000, 'x');
        Write(root, "model-failed.json", failed.ReportJson());
        Check(failed.ReportJson().find("scan_budget") != std::string::npos, "known error token lifetime lost");
        std::cout << "PASS model phases success-after-return and known-code lifetime\n";
        Catalog canary;
        canary.containerSnapshot = "/PRIVATE_URI_KEY_RIGHTS_RAW_HEADER";
        canary.otherEntries.push_back(canary.containerSnapshot);
        NcaDescriptor nca; nca.entry = canary.containerSnapshot; nca.readability = canary.containerSnapshot; nca.support = canary.containerSnapshot;
        nca.issues.push_back({canary.containerSnapshot, canary.containerSnapshot}); canary.ncas.push_back(nca);
        Group group; group.cnmtEntry = canary.containerSnapshot; group.rawHeader = canary.containerSnapshot; group.id = canary.containerSnapshot;
        group.parsing = canary.containerSnapshot; group.digest = canary.containerSnapshot; group.issues = nca.issues; group.titleId = 1; canary.groups.push_back(group);
        const auto safe = completed.ReportJson(&canary);
        Check(safe.find("PRIVATE") == std::string::npos && safe.find("other_native_issue") != std::string::npos, "private/unknown token crossed report");
        Check(safe.find("title_id") == std::string::npos, "unparsed public ID exported");
        Write(root, "model-private-canary.json", safe);
        std::cout << "PASS model private canaries and unknown statuses/codes\n";
        Catalog large; group.parsing = "parsed"; group.digest = "mismatch"; group.id = "cnmt-sha256:" + std::string(64, 'a'); group.applicationId = 2;
        group.issues.clear(); nca.issues.clear();
        for (const auto code : AllowedCodes()) { group.issues.push_back({std::string(code), "PRIVATE"}); nca.issues.push_back({std::string(code), "PRIVATE"}); }
        large.groups.assign(4096, group); large.ncas.assign(4096, nca);
        const auto bounded = completed.ReportJson(&large);
        Check(bounded.size() <= MaxReportBytes && bounded.find("PRIVATE") == std::string::npos, "report byte bound/privacy failed");
        Write(root, "model-bounded.json", bounded);
        std::cout << "PASS model combined row and byte budget\n";
        std::string codes{"["}; bool first = true;
        for (const auto code : AllowedCodes()) { if (!first) codes += ','; first = false; codes += Json(std::string(code)); }
        Write(root, "native-code-allowlist.json", codes + ']');
        ScanReport unknown(Clock); unknown.Begin(Stage::ProfileValidation); unknown.Fail("PRIVATE_ERROR_CODE");
        Write(root, "model-unknown-error.json", unknown.ReportJson());
        Check(unknown.ReportJson().find("PRIVATE") == std::string::npos, "unknown error leaked");
        std::cout << "PASS model unknown exception code fixed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
