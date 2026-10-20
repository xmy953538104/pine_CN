// SPDX-License-Identifier: MPL-2.0
// Real temporary POSIX files, synthetic serialized records and static IO faults.
#include "resource_window.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>

static bool failAllocation{};
[[gnu::noinline]] void *operator new(size_t size) {
    if (failAllocation) throw std::bad_alloc();
    if (void *value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
[[gnu::noinline]] void *operator new[](size_t size) { return ::operator new(size); }
[[gnu::noinline]] void operator delete(void *value) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete[](void *value) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete(void *value, size_t) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete[](void *value, size_t) noexcept { std::free(value); }

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::abort(); } } while (false)
using namespace skyline::diagnostics;
namespace fs = std::filesystem;
static fs::path root;
static constexpr auto reason = ResourceWindowReason::Periodic;
static fs::path Directory(const char *name) { auto path = root / name; CHECK(fs::create_directories(path)); return path; }
static std::string Read(const fs::path &path) {
    std::ifstream input(path, std::ios::binary); CHECK(input.good());
    return {std::istreambuf_iterator<char>(input), {}};
}
static void Write(const fs::path &path, std::string_view value) {
    std::ofstream output(path, std::ios::binary); output.write(value.data(), value.size()); CHECK(output.good());
}
static bool Persist(RecentResourceWindow &window, const fs::path &path, std::string_view session = "synthetic-session") {
    return window.Persist(path.string(), session, reason);
}
static std::string Line(size_t bytes) {
    const std::string prefix = "{\"payload\":\"";
    CHECK(bytes >= prefix.size() + 3);
    return prefix + std::string(bytes - prefix.size() - 3, 'x') + "\"}\n";
}

static void EmptyAndFraming() {
    const auto empty = Directory("empty"); RecentResourceWindow window;
    CHECK(Persist(window, empty));
    const auto metadata = Read(empty / "resource-window.jsonl");
    CHECK(metadata.find("\"retained_records\":0") != std::string::npos && metadata.find("\"earliest_monotonic_us\":null") != std::string::npos);
    CHECK(metadata.find("not_complete_history") != std::string::npos);
    CHECK(window.Add(0, "{}")); CHECK(window.Add(1, "{\"sample\":1}\n"));
    for (auto bad : {"", "[]", "{}\n{}\n", "{}\r\n", "not-json"}) CHECK(!window.Add(2, bad));
    const char nul[]{'{', '\0', '}'};
    CHECK(!window.Add(2, std::string_view(nul, sizeof(nul))));
    CHECK(window.Stats().omittedInvalid == 6 && window.Size() == 2);
    const auto framed = Directory("framing"); CHECK(Persist(window, framed));
    const auto text = Read(framed / "resource-window.jsonl");
    CHECK(text.ends_with("{}\n{\"sample\":1}\n") && !fs::exists(framed / "resource-window.tmp"));
    struct stat identity{}; CHECK(!::stat((framed / "resource-window.jsonl").c_str(), &identity));
    CHECK(S_ISREG(identity.st_mode) && identity.st_nlink == 1 && (identity.st_mode & 0777) == 0600);
}

static void CountAndByteBudgets() {
    RecentResourceWindow count;
    for (unsigned i{}; i <= RecentResourceWindow::MaxRecords; ++i) CHECK(count.Add(1, "{\"i\":" + std::to_string(i) + "}"));
    CHECK(count.Size() == 240 && count.Stats().evictedCount == 1 && count.Stats().countBytes == 8);
    CHECK(count.Count() == 240 && count.Evictions() == 1);
    CHECK(Persist(count, Directory("count")));
    RecentResourceWindow bytes;
    const auto full = Line(RecentResourceWindow::MaxLineBytes);
    for (unsigned i{}; i < 3; ++i) CHECK(bytes.Add(i, full));
    CHECK(bytes.Add(3, Line(RecentResourceWindow::MaxDataBytes - 3 * full.size())));
    CHECK(bytes.Bytes() == RecentResourceWindow::MaxDataBytes);
    CHECK(bytes.Add(4, "{}"));
    CHECK(bytes.Stats().evictedBytes == 1 && bytes.Stats().budgetBytes == full.size());
    CHECK(bytes.Bytes() <= RecentResourceWindow::MaxDataBytes && bytes.Size() == 4);
    CHECK(!bytes.Add(5, std::string(RecentResourceWindow::MaxLineBytes + 1, 'x')));
    CHECK(!bytes.Add(6, Line(RecentResourceWindow::MaxLineBytes + 1)));
    CHECK(bytes.Stats().omittedOversize == 2);
    CHECK(bytes.Omissions() == 2 && bytes.Evictions() == 1);
    const auto directory = Directory("bytes"); CHECK(Persist(bytes, directory));
    CHECK(fs::file_size(directory / "resource-window.jsonl") <= RecentResourceWindow::MaxFileBytes);
}

static void AgeClockAndSession() {
    RecentResourceWindow age;
    CHECK(age.Add(0, "{\"i\":0}")); CHECK(age.Add(59999999, "{\"i\":1}")); CHECK(age.Add(60000000, "{\"i\":2}"));
    CHECK(age.Size() == 2 && age.Stats().evictedAge == 1);
    CHECK(Persist(age, Directory("age")));
    CHECK(age.Add(120000000, "{\"i\":3}") && age.Size() == 1 && age.Stats().evictedAge == 3);
    RecentResourceWindow clock;
    CHECK(clock.Add(100, "{}")); CHECK(clock.Add(200, "{}")); CHECK(clock.Add(10, "{\"new_epoch\":true}"));
    CHECK(clock.Size() == 1 && clock.Stats().clockResets == 1 && clock.Stats().evictedClock == 2);
    CHECK(Persist(clock, Directory("clock")));
    RecentResourceWindow session; const auto directory = Directory("session");
    CHECK(session.Add(1, "{\"session_a\":true}")); CHECK(Persist(session, directory, "session-a"));
    CHECK(session.Add(2, "{\"session_a\":true}")); CHECK(Persist(session, directory, "session-b"));
    CHECK(session.Size() == 0 && Read(directory / "resource-window.jsonl").find("session_a") == std::string::npos);
    CHECK(session.Add(3, "{\"session_b\":true}")); CHECK(Persist(session, directory, "session-b"));
    session.Reset(); CHECK(session.Size() == 0 && session.Bytes() == 0 && session.Stats().evictedAge == 0);
    CHECK(session.Add(4, "{\"session_c\":true}")); CHECK(Persist(session, directory, "session-c"));
}

static void QuotedMetadataAndInvalidSession() {
    RecentResourceWindow window; const auto directory = Directory("quoted"); CHECK(window.Add(99, "{}"));
    CHECK(Persist(window, directory, "session\"\\\n\t测试"));
    const auto original = Read(directory / "resource-window.jsonl");
    CHECK(original.find("\\u000a") != std::string::npos && original.find("\\u0009") != std::string::npos);
    CHECK(!Persist(window, directory, "")); CHECK(!Persist(window, directory, std::string(129, 'a')));
    for (const auto invalid : {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf0"}) CHECK(!Persist(window, directory, invalid));
    CHECK(!window.Persist(directory.string(), "session", static_cast<ResourceWindowReason>(255)));
    CHECK(Read(directory / "resource-window.jsonl") == original);
    const auto max = Directory("maximum-metadata");
    CHECK(Persist(window, max, std::string(RecentResourceWindow::MaxSessionBytes, '\1')));
    CHECK(Read(max / "resource-window.jsonl").size() < RecentResourceWindow::MetadataReserveBytes);
}

static void AllocationExceptionsCannotEscapeOrReplaceTarget() {
    failAllocation = true;
    { RecentResourceWindow construction; construction.Reset(); CHECK(construction.Size() == 0); }
    failAllocation = false;
    RecentResourceWindow window; const auto directory = Directory("allocation");
    CHECK(window.Add(0, "{}")); CHECK(Persist(window, directory));
    const auto original = Read(directory / "resource-window.jsonl");
    const auto large = Line(4000);
    failAllocation = true; const auto accepted = window.Add(1, large); failAllocation = false;
    CHECK(!accepted && window.Size() == 1 && window.Stats().omittedAllocation == 1);
    const auto path = directory.string();
    failAllocation = true; const auto persisted = window.Persist(path, "synthetic-session", reason); failAllocation = false;
    CHECK(!persisted && Read(directory / "resource-window.jsonl") == original);
    CHECK(!fs::exists(directory / "resource-window.tmp"));
}

struct FaultIo : resource_window_detail::PosixIo {
    enum class Mode { Normal, FailWrite, ZeroWrite, InvalidWriteCount, FailSync, FailClose, FailRename, ShortAndInterrupt, AlwaysInterrupt };
    static inline Mode mode{};
    static inline unsigned calls{};
    static ssize_t Write(int fd, const void *data, size_t size) noexcept {
        ++calls;
        if (mode == Mode::FailWrite) { errno = ENOSPC; return -1; }
        if (mode == Mode::ZeroWrite) return 0;
        if (mode == Mode::InvalidWriteCount) return static_cast<ssize_t>(size + 1);
        if (mode == Mode::AlwaysInterrupt || (mode == Mode::ShortAndInterrupt && calls == 1)) { errno = EINTR; return -1; }
        return ::write(fd, data, mode == Mode::ShortAndInterrupt ? std::min<size_t>(7, size) : size);
    }
    static int Sync(int fd) noexcept { if (mode == Mode::FailSync) { errno = EIO; return -1; } return ::fsync(fd); }
    static int Close(int fd) noexcept { const auto closed = ::close(fd); if (mode == Mode::FailClose) { errno = EIO; return -1; } return closed; }
    static int Rename(int directory, const char *from, const char *to) noexcept {
        if (mode == Mode::FailRename) { errno = EACCES; return -1; }
        return ::renameat(directory, from, directory, to);
    }
};
static void IoFailuresPreserveExistingTarget() {
    using Window = BasicRecentResourceWindow<FaultIo>;
    unsigned index{};
    for (auto mode : {FaultIo::Mode::FailWrite, FaultIo::Mode::ZeroWrite, FaultIo::Mode::InvalidWriteCount,
        FaultIo::Mode::FailSync, FaultIo::Mode::FailClose, FaultIo::Mode::FailRename, FaultIo::Mode::AlwaysInterrupt}) {
        const auto name = "io-failure-" + std::to_string(index++); const auto directory = Directory(name.c_str());
        const auto target = directory / "resource-window.jsonl"; Write(target, "{\"old_target\":true}\n");
        const auto old = Read(target);
        Window window; CHECK(window.Add(1, "{\"replacement\":true}")); FaultIo::mode = mode; FaultIo::calls = 0;
        CHECK(!window.Persist(directory.string(), "synthetic", reason));
        CHECK(Read(target) == old && !fs::exists(directory / "resource-window.tmp"));
        if (mode == FaultIo::Mode::AlwaysInterrupt) CHECK(FaultIo::calls == 33);
        FaultIo::mode = FaultIo::Mode::Normal;
        CHECK(window.Persist(directory.string(), "synthetic", reason)); CHECK(Read(target).find("replacement") != std::string::npos);
    }
    Window partial; const auto directory = Directory("partial-writes"); CHECK(partial.Add(1, "{}"));
    FaultIo::mode = FaultIo::Mode::ShortAndInterrupt; FaultIo::calls = 0;
    CHECK(partial.Persist(directory.string(), "synthetic", reason) && FaultIo::calls > 2);
    FaultIo::mode = FaultIo::Mode::Normal;
}

static void LinksSpecialFilesAndAnchoredDirectory() {
    RecentResourceWindow window; CHECK(window.Add(1, "{}"));
    const auto directory = Directory("links"); const auto outside = root / "outside-data";
    Write(outside, "PUBLIC_SYNTHETIC_OUTSIDE"); const auto target = directory / "resource-window.jsonl";
    const auto temporary = directory / "resource-window.tmp";
    fs::create_symlink(outside, target); CHECK(!Persist(window, directory)); CHECK(fs::is_symlink(target)); fs::remove(target);
    fs::create_hard_link(outside, target); CHECK(!Persist(window, directory)); fs::remove(target);
    Write(target, "old-target");
    fs::create_symlink(outside, temporary); CHECK(!Persist(window, directory)); fs::remove(temporary);
    fs::create_hard_link(outside, temporary); CHECK(!Persist(window, directory)); fs::remove(temporary);
    CHECK(!::mkfifo(temporary.c_str(), 0600)); CHECK(!Persist(window, directory)); fs::remove(temporary);
    CHECK(Read(target) == "old-target" && Read(outside) == "PUBLIC_SYNTHETIC_OUTSIDE");
    fs::remove(target); CHECK(!::mkfifo(target.c_str(), 0600)); CHECK(!Persist(window, directory)); fs::remove(target);
    fs::create_directory(target); CHECK(!Persist(window, directory)); fs::remove(target);
    const auto real = Directory("real-directory"); fs::create_directory(real / "child"); const auto link = root / "directory-link";
    fs::create_directory_symlink(real, link); CHECK(!Persist(window, link)); CHECK(!Persist(window, link / "child"));
    CHECK(!fs::exists(real / "resource-window.jsonl") && !fs::exists(real / "child/resource-window.jsonl"));
    CHECK(!window.Persist((directory / "../real-directory").string(), "synthetic", reason));
    CHECK(!Persist(window, root / "missing-directory"));
    const auto pathWithNul = directory.string() + std::string("\0hidden", 7);
    CHECK(!window.Persist(pathWithNul, "synthetic", reason));
}

static void StaleOwnedTemporaryCanRecover() {
    RecentResourceWindow window; const auto directory = Directory("stale"); CHECK(window.Add(1, "{}"));
    Write(directory / "resource-window.tmp", "incomplete old write"); Write(directory / "resource-window.jsonl", "old target");
    CHECK(Persist(window, directory)); CHECK(!fs::exists(directory / "resource-window.tmp"));
    CHECK(Read(directory / "resource-window.jsonl").find("resource_window_metadata") != std::string::npos);
}

static void ExceptionReasonPreservesObservedRecord() {
    RecentResourceWindow window; const auto directory = Directory("exception");
    constexpr std::string_view record = "{\"event\":\"GUEST_SIGNAL\",\"category\":\"exception\",\"sequence\":44}\n";
    CHECK(window.Add(1, record));
    CHECK(window.Persist(directory.string(), "synthetic-session", ResourceWindowReason::ExceptionObserved));
    const auto text = Read(directory / "resource-window.jsonl");
    CHECK(text.find("\"reason\":\"exception_observed\"") != std::string::npos && text.ends_with(record));
}

static void IngestionTimeDoesNotReinterpretEventOrder() {
    RecentResourceWindow window; const auto directory = Directory("ingestion-time");
    CHECK(window.Add(100, "{\"monotonic_us\":9999999,\"sequence\":1}"));
    CHECK(window.Add(101, "{\"monotonic_us\":1,\"sequence\":2}"));
    CHECK(window.Count() == 2 && window.Stats().clockResets == 0);
    CHECK(Persist(window, directory));
    const auto text = Read(directory / "resource-window.jsonl");
    CHECK(text.find("writer_ingestion_monotonic_not_event_execution") != std::string::npos);
    CHECK(text.find("\"retained_span_us\":1") != std::string::npos);
}

int main(int argc, char **argv) {
    CHECK(argc == 2); root = argv[1]; CHECK(fs::create_directories(root));
    struct Case { const char *name; void (*run)(); };
    constexpr Case cases[]{
        {"empty metadata, single-line framing, private regular output", EmptyAndFraming},
        {"240-record/byte/line caps without truncation", CountAndByteBudgets},
        {"60s age, clock rollback and session reset", AgeClockAndSession},
        {"metadata JSON escaping, bounded UTF-8 session and reason", QuotedMetadataAndInvalidSession},
        {"allocation exceptions preserve old target", AllocationExceptionsCannotEscapeOrReplaceTarget},
        {"disk/write/sync/close/rename faults preserve old target", IoFailuresPreserveExistingTarget},
        {"symlink/hardlink/FIFO/non-directory rejection", LinksSpecialFilesAndAnchoredDirectory},
        {"safe stale owned temporary recovery", StaleOwnedTemporaryCanRecover},
        {"explicit exception reason preserves caller-observed record", ExceptionReasonPreservesObservedRecord},
        {"ingestion-time window preserves out-of-order original event timestamps", IngestionTimeDoesNotReinterpretEventOrder},
    };
    for (const auto &test : cases) { test.run(); std::printf("PASS: %s\n", test.name); }
    std::printf("PASS: %zu synthetic resource-window cases; object_bytes=%zu payload_cap=%zu metadata_reserve=%zu; no sink/Guest integration\n",
        std::size(cases), sizeof(RecentResourceWindow), RecentResourceWindow::MaxDataBytes, RecentResourceWindow::MetadataReserveBytes);
}
