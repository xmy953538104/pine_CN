// SPDX-License-Identifier: MPL-2.0
#include "scan_report.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <sstream>

namespace skyline::catalog::observed {
namespace {
constexpr std::string_view Codes[]{
#include "scan_report_codes.inc"
};
constexpr std::string_view Stages[]{"profile_validation", "keystore_load", "fd_duplicate", "catalog_scan", "catalog_serialize"};
constexpr uint64_t MaxTime = 0x7fffffffffffffffULL;
uint64_t Monotonic() noexcept {
    const auto value = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return value > 0 ? std::min(static_cast<uint64_t>(value), MaxTime) : 0;
}
uint64_t Difference(uint64_t end, uint64_t start) noexcept { return end >= start ? std::min(end - start, MaxTime) : 0; }
uint64_t Count(size_t value) { return std::min<uint64_t>(value, 0x7fffffff); }
struct Object {
    std::string data{"{"}; bool first{true};
    void Raw(const char *key, const std::string &value) { if (!first) data += ','; first = false; data += catalog::Json(key) + ':' + value; }
    void Token(const char *key, std::string_view value) { Raw(key, catalog::Json(std::string(value))); }
    void Number(const char *key, uint64_t value) { Raw(key, std::to_string(value)); }
    void Bool(const char *key, bool value) { Raw(key, value ? "true" : "false"); }
    std::string End() { return std::move(data) + '}'; }
};
std::string_view Enum(std::string_view value, std::initializer_list<std::string_view> allowed) {
    const auto found = std::find(allowed.begin(), allowed.end(), value);
    return found != allowed.end() ? *found : "unknown";
}
std::string_view Role(const Group &group) {
    if (group.type == 0x80) return "application";
    if (group.type == 0x81) return "patch";
    if (group.type == 0x82) return "aoc";
    return "other";
}
struct IssueCodes {
    std::array<std::string_view, MaxIssueCodes> values{}; size_t size{};
    void Add(const std::vector<Issue> &issues) {
        for (const auto &issue : issues) {
            const auto safe = SafeCode(issue.code);
            if (std::find(values.begin(), values.begin() + size, safe) == values.begin() + size && size < values.size())
                values[size++] = safe;
            if (size == values.size()) break;
        }
    }
    std::string Json() const {
        std::string out{"["};
        for (size_t i{}; i < size; ++i) { if (i) out += ','; out += catalog::Json(std::string(values[i])); }
        return out + ']';
    }
};
std::string IssueArray(const std::vector<Issue> &issues) { IssueCodes result; result.Add(issues); return result.Json(); }
bool GroupId(const std::string &value) {
    constexpr std::string_view prefix{"cnmt-sha256:"};
    return value.size() == 76 && value.compare(0, prefix.size(), prefix) == 0 &&
        std::all_of(value.begin() + prefix.size(), value.end(), [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
std::string GroupRow(const Group &group, size_t ordinal) {
    Object out; out.Number("ordinal", Count(ordinal)); out.Token("role", Role(group));
    out.Token("parsing", Enum(group.parsing, {"unknown", "parsed", "unsupported", "invalid", "not_inspected"}));
    out.Token("digest_observation", Enum(group.digest, {"unknown", "unverified", "verified", "mismatch"}));
    out.Raw("issue_codes", IssueArray(group.issues));
    if (group.parsing == "parsed") {
        if (group.titleId) out.Token("title_id", Hex64(*group.titleId));
        if (group.applicationId) out.Token("application_id", Hex64(*group.applicationId));
        if (GroupId(group.id)) out.Token("cnmt_group_id", group.id);
    }
    return out.End();
}
std::string MemberRow(const NcaDescriptor &nca, size_t ordinal) {
    Object out; out.Number("ordinal", Count(ordinal));
    out.Raw("nca_type", nca.contentType ? std::to_string(*nca.contentType) : "null");
    out.Token("readability", Enum(nca.readability, {"unknown", "plaintext_header", "decrypted_header", "encrypted_or_unknown", "unreadable", "not_inspected"}));
    out.Token("support", Enum(nca.support, {"unknown", "metadata_readable", "unsupported", "invalid"}));
    out.Raw("issue_codes", IssueArray(nca.issues)); return out.End();
}
std::string Summary(const Catalog &catalog) {
    Object out;
    out.Bool("bounded_scan_complete", catalog.complete); out.Bool("all_metadata_enumerated", catalog.AllMetadataEnumerated());
    out.Number("nca_count", Count(catalog.ncas.size())); out.Number("group_count", Count(catalog.groups.size()));
    out.Number("other_entry_count", Count(catalog.otherEntries.size()));
    size_t meta{}, unknown{}, unreadable{};
    IssueCodes codes; codes.Add(catalog.issues);
    for (const auto &nca : catalog.ncas) {
        meta += nca.contentType == 1;
        unknown += !nca.contentType || *nca.contentType > 5;
        unreadable += (nca.readability != "plaintext_header" && nca.readability != "decrypted_header") || nca.support != "metadata_readable";
        codes.Add(nca.issues);
    }
    out.Number("meta_nca_count", Count(meta)); out.Number("unknown_nca_count", Count(unknown)); out.Number("unreadable_nca_count", Count(unreadable));
    std::array<size_t, 4> roles{};
    for (const auto &group : catalog.groups) {
        const auto role = Role(group);
        ++roles[role == "application" ? 0 : role == "patch" ? 1 : role == "aoc" ? 2 : 3];
        codes.Add(group.issues);
    }
    Object roleCounts;
    roleCounts.Number("application", Count(roles[0])); roleCounts.Number("patch", Count(roles[1]));
    roleCounts.Number("aoc", Count(roles[2])); roleCounts.Number("other", Count(roles[3]));
    out.Raw("role_counts", roleCounts.End()); out.Raw("issue_codes", codes.Json());
    // Reserve at least 4 KiB for all fixed fields, five phases and capped codes.
    // Whole rows are omitted; JSON or UTF-8 is never cut at a byte boundary.
    size_t rows{}, bytes{}, groupRows{}, memberRows{};
    std::string groups{"["}, members{"["};
    for (size_t i{}; i < catalog.groups.size() && rows < MaxRows; ++i) {
        auto row = GroupRow(catalog.groups[i], i);
        if (bytes + row.size() + 1 > 12 * 1024) break;
        if (groupRows) groups += ',';
        groups += row; bytes += row.size() + 1; ++groupRows; ++rows;
    }
    for (size_t i{}; i < catalog.ncas.size() && rows < MaxRows; ++i) {
        auto row = MemberRow(catalog.ncas[i], i);
        if (bytes + row.size() + 1 > 12 * 1024) break;
        if (memberRows) members += ',';
        members += row; bytes += row.size() + 1; ++memberRows; ++rows;
    }
    out.Raw("groups", groups + ']'); out.Raw("members", members + ']');
    out.Number("groups_omitted", Count(catalog.groups.size() - groupRows)); out.Number("members_omitted", Count(catalog.ncas.size() - memberRows));
    return out.End();
}
}
std::string_view StageName(Stage stage) noexcept {
    const auto index = static_cast<size_t>(stage);
    return index < std::size(Stages) ? Stages[index] : "catalog_serialize";
}
std::span<const std::string_view> AllowedCodes() noexcept { return Codes; }
std::string_view SafeCode(std::string_view code) noexcept {
    const auto found = std::find(std::begin(Codes), std::end(Codes), code);
    return found == std::end(Codes) ? "other_native_issue" : *found;
}
ScanReport::ScanReport(Clock pClock) noexcept : clock(pClock ? pClock : Monotonic), started(clock()) {}
uint64_t ScanReport::Elapsed() const noexcept { return Difference(clock(), started); }
void ScanReport::Begin(Stage stage) {
    if (state != State::Running || phaseCount >= phases.size() || static_cast<size_t>(stage) != phaseCount ||
        (phaseCount && phases[phaseCount - 1].status != PhaseStatus::Completed))
        throw Error("other_native_issue", "invalid observed scan phase transition");
    phases[phaseCount++] = {stage, Elapsed(), 0, PhaseStatus::Begun};
}
void ScanReport::Complete(Stage stage) {
    if (state != State::Running || !phaseCount || phases[phaseCount - 1].stage != stage || phases[phaseCount - 1].status != PhaseStatus::Begun)
        throw Error("other_native_issue", "invalid observed scan phase completion");
    auto &phase = phases[phaseCount - 1]; phase.durationUs = Difference(Elapsed(), phase.startUs); phase.status = PhaseStatus::Completed;
}
void ScanReport::Finish() {
    if (state != State::Running || phaseCount != phases.size() || phases.back().status != PhaseStatus::Completed)
        throw Error("other_native_issue", "observed scan did not finish all phases");
    state = State::Completed;
}
void ScanReport::Fail(std::string_view code) noexcept {
    state = State::Failed; errorCode = SafeCode(code);
    if (phaseCount) { auto &phase = phases[phaseCount - 1]; failedStage = phase.stage; phase.durationUs = Difference(Elapsed(), phase.startUs); phase.status = PhaseStatus::Failed; }
}
bool ScanReport::Completed() const noexcept { return state == State::Completed; }
std::string ScanReport::ReportJson(const Catalog *catalog) const {
    Object out; out.Number("schema_version", 1); out.Token("status", Completed() ? "completed" : "failed");
    out.Raw("failed_stage", failedStage ? catalog::Json(std::string(StageName(*failedStage))) : "null");
    out.Raw("error_code", Completed() ? "null" : catalog::Json(std::string(errorCode)));
    out.Number("elapsed_us", Elapsed()); std::string phaseArray{"["};
    for (size_t i{}; i < phaseCount; ++i) {
        const auto &phase = phases[i]; Object item;
        item.Token("stage", StageName(phase.stage)); item.Number("start_us", phase.startUs); item.Number("duration_us", phase.durationUs);
        item.Token("status", phase.status == PhaseStatus::Completed ? "completed" : phase.status == PhaseStatus::Failed ? "failed" : "begun");
        if (i) phaseArray += ',';
        phaseArray += item.End();
    }
    out.Raw("phases", phaseArray + ']'); out.Raw("catalog_summary", catalog ? Summary(*catalog) : "null");
    auto result = out.End();
    if (result.size() > MaxReportBytes) throw Error("selected_report_limit", "bounded scan report exceeds fixed limit");
    return result;
}
std::string ScanReport::Envelope(const std::string &transientCatalogJson, const Catalog *catalog) const {
    Object out; out.Number("selected_scan_schema", 1); out.Token("status", Completed() ? "completed" : "failed");
    out.Raw("catalog", Completed() && !transientCatalogJson.empty() ? transientCatalogJson : "null");
    out.Raw("scan_report", ReportJson(catalog)); return out.End();
}
}
