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
#include <sys/wait.h>
#include <vector>

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
using Stage = ResourceWindowFailureStage;
using FailureReason = ResourceWindowFailureReason;
template<class Window> static void Failure(const Window &window, Stage stage, FailureReason reason, int error = 0) {
    const auto failure = window.LastFailure();
    CHECK(failure.stage == stage && failure.reason == reason && failure.error == error);
}
template<class Window> static void Cleared(const Window &window) { Failure(window, Stage::None, FailureReason::None); }
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
using Retention = ResourceWindowRetention;
static void Bounds(const RecentResourceWindow &window) {
    CHECK(window.Count() <= window.MaxRecords && window.Bytes() <= window.MaxDataBytes);
    CHECK(window.CriticalCount() <= window.MaxCriticalRecords && window.CriticalBytes() <= window.MaxCriticalBytes);
    CHECK(window.RecentCount() + window.CriticalCount() == window.Count());
    CHECK(window.RecentBytes() + window.CriticalBytes() == window.Bytes());
    CHECK(window.Stats().evictedRecent + window.Stats().evictedCritical == window.Evictions());
}
static std::string Record(uint64_t sequence, const char *event, size_t bytes = 0) {
    auto line = "{\"schema\":1,\"session_id\":\"synthetic-session\",\"process_run_id\":\"synthetic-process\",\"sequence\":"
        + std::to_string(sequence) + ",\"monotonic_us\":" + std::to_string(1000000 - sequence)
        + ",\"event\":\"" + event + "\",\"fields\":{\"payload\":\"";
    constexpr std::string_view ending = "\"}}\n";
    if (bytes) { CHECK(bytes >= line.size() + ending.size()); line.append(bytes - line.size() - ending.size(), 'x'); }
    return line + std::string(ending);
}
static std::string RecordsOnly(const fs::path &directory) {
    const auto file = Read(directory / "resource-window.jsonl");
    const auto end = file.find('\n'); CHECK(end != std::string::npos); return file.substr(end + 1);
}

static void EmptyAndFraming() {
    const auto empty = Directory("empty"); RecentResourceWindow window;
    Cleared(window);
    CHECK(Persist(window, empty));
    Cleared(window);
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
    Failure(window, Stage::Validate, FailureReason::SizeLimit);
    for (const auto invalid : {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf0"}) CHECK(!Persist(window, directory, invalid));
    Failure(window, Stage::Validate, FailureReason::InvalidInput);
    CHECK(!window.Persist(directory.string(), "session", static_cast<ResourceWindowReason>(255)));
    Failure(window, Stage::Validate, FailureReason::InvalidInput);
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
    Failure(window, Stage::Metadata, FailureReason::AllocationFailure);
    CHECK(window.Add(2, "{}")); // Add must not erase the persistence diagnosis.
    Failure(window, Stage::Metadata, FailureReason::AllocationFailure);
    CHECK(!fs::exists(directory / "resource-window.tmp"));
    CHECK(Persist(window, directory)); Cleared(window);
}

struct FaultIo : resource_window_detail::PosixIo {
    enum class Mode { Normal, FailWrite, FailRecordWrite, ZeroWrite, InvalidWriteCount,
        SkipRecordWrite, FailSync, FailClose, FailRename, ThrowUnknown, ShortAndInterrupt, AlwaysInterrupt };
    static inline Mode mode{};
    static inline unsigned calls{};
    static ssize_t Write(int fd, const void *data, size_t size) noexcept {
        ++calls;
        if (mode == Mode::FailWrite) { errno = ENOSPC; return -1; }
        if (mode == Mode::FailRecordWrite && calls == 2) { errno = EIO; return -1; }
        if (mode == Mode::SkipRecordWrite && calls == 2) return static_cast<ssize_t>(size);
        if (mode == Mode::ZeroWrite) return 0;
        if (mode == Mode::InvalidWriteCount) return static_cast<ssize_t>(size + 1);
        if (mode == Mode::AlwaysInterrupt || (mode == Mode::ShortAndInterrupt && calls == 1)) { errno = EINTR; return -1; }
        return ::write(fd, data, mode == Mode::ShortAndInterrupt ? std::min<size_t>(7, size) : size);
    }
    static int Sync(int fd) noexcept { if (mode == Mode::FailSync) { errno = EIO; return -1; } return ::fsync(fd); }
    static int Close(int fd) noexcept { const auto closed = ::close(fd); if (mode == Mode::FailClose) { errno = EIO; return -1; } return closed; }
    static int Rename(int directory, const char *from, const char *to) {
        if (mode == Mode::FailRename) { errno = EACCES; return -1; }
        if (mode == Mode::ThrowUnknown) throw 1;
        return ::renameat(directory, from, directory, to);
    }
};
static void IoFailuresPreserveExistingTarget() {
    using Window = BasicRecentResourceWindow<FaultIo>;
    unsigned index{};
    struct Fault { FaultIo::Mode mode; Stage stage; FailureReason reason; int error; };
    constexpr Fault faults[]{
        {FaultIo::Mode::FailWrite, Stage::WriteMetadata, FailureReason::SyscallError, ENOSPC},
        {FaultIo::Mode::FailRecordWrite, Stage::WriteRecords, FailureReason::SyscallError, EIO},
        {FaultIo::Mode::ZeroWrite, Stage::WriteMetadata, FailureReason::ZeroWrite, 0},
        {FaultIo::Mode::InvalidWriteCount, Stage::WriteMetadata, FailureReason::InvalidWriteCount, 0},
        {FaultIo::Mode::SkipRecordWrite, Stage::VerifySize, FailureReason::ChangedFile, 0},
        {FaultIo::Mode::FailSync, Stage::Sync, FailureReason::SyscallError, EIO},
        {FaultIo::Mode::FailClose, Stage::Close, FailureReason::SyscallError, EIO},
        {FaultIo::Mode::FailRename, Stage::Rename, FailureReason::SyscallError, EACCES},
        {FaultIo::Mode::ThrowUnknown, Stage::Exception, FailureReason::UnknownException, 0},
        {FaultIo::Mode::AlwaysInterrupt, Stage::WriteMetadata, FailureReason::SyscallError, EINTR},
    };
    for (const auto fault : faults) {
        const auto name = "io-failure-" + std::to_string(index++); const auto directory = Directory(name.c_str());
        const auto target = directory / "resource-window.jsonl"; Write(target, "{\"old_target\":true}\n");
        const auto old = Read(target);
        Window window; CHECK(window.Add(1, "{\"replacement\":true}")); FaultIo::mode = fault.mode; FaultIo::calls = 0;
        CHECK(!window.Persist(directory.string(), "synthetic", reason));
        CHECK(Read(target) == old && !fs::exists(directory / "resource-window.tmp"));
        // Cleanup and subsequent syscalls may change errno, never this POD.
        errno = ERANGE;
        Failure(window, fault.stage, fault.reason, fault.error);
        if (fault.mode == FaultIo::Mode::AlwaysInterrupt) CHECK(FaultIo::calls == 33);
        FaultIo::mode = FaultIo::Mode::Normal;
        CHECK(window.Persist(directory.string(), "synthetic", reason)); CHECK(Read(target).find("replacement") != std::string::npos);
        Cleared(window);
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
    fs::create_symlink(outside, target); CHECK(!Persist(window, directory)); CHECK(fs::is_symlink(target));
    Failure(window, Stage::InspectTarget, FailureReason::PolicyRejected); fs::remove(target);
    fs::create_hard_link(outside, target); CHECK(!Persist(window, directory));
    Failure(window, Stage::InspectTarget, FailureReason::PolicyRejected); fs::remove(target);
    Write(target, "old-target");
    fs::create_symlink(outside, temporary); CHECK(!Persist(window, directory));
    Failure(window, Stage::InspectTemporary, FailureReason::PolicyRejected); fs::remove(temporary);
    fs::create_hard_link(outside, temporary); CHECK(!Persist(window, directory));
    Failure(window, Stage::InspectTemporary, FailureReason::PolicyRejected); fs::remove(temporary);
    CHECK(!::mkfifo(temporary.c_str(), 0600)); CHECK(!Persist(window, directory));
    Failure(window, Stage::InspectTemporary, FailureReason::PolicyRejected); fs::remove(temporary);
    CHECK(Read(target) == "old-target" && Read(outside) == "PUBLIC_SYNTHETIC_OUTSIDE");
    fs::remove(target); CHECK(!::mkfifo(target.c_str(), 0600)); CHECK(!Persist(window, directory)); fs::remove(target);
    fs::create_directory(target); CHECK(!Persist(window, directory)); fs::remove(target);
    const auto real = Directory("real-directory"); fs::create_directory(real / "child"); const auto link = root / "directory-link";
    fs::create_directory_symlink(real, link); CHECK(!Persist(window, link)); CHECK(!Persist(window, link / "child"));
    CHECK(window.LastFailure().stage == Stage::OpenDirectory && window.LastFailure().reason == FailureReason::SyscallError);
    CHECK(!fs::exists(real / "resource-window.jsonl") && !fs::exists(real / "child/resource-window.jsonl"));
    CHECK(!window.Persist((directory / "../real-directory").string(), "synthetic", reason));
    Failure(window, Stage::Validate, FailureReason::InvalidInput);
    CHECK(!Persist(window, root / "missing-directory"));
    Failure(window, Stage::OpenDirectory, FailureReason::SyscallError, ENOENT);
    const auto pathWithNul = directory.string() + std::string("\0hidden", 7);
    CHECK(!window.Persist(pathWithNul, "synthetic", reason));
    Failure(window, Stage::Validate, FailureReason::InvalidInput);
}

// Root can bypass read/search permission checks. When invoked as root, run
// these actual POSIX permission regressions in a child that drops privileges;
// no test silently claims success from root's DAC override.
template<class Test> static void Unprivileged(Test test) {
    if (::geteuid()) { test(); return; }
    std::fflush(nullptr);
    const auto child = ::fork(); CHECK(child >= 0);
    if (!child) {
        CHECK(!::setgid(65534)); CHECK(!::setuid(65534)); CHECK(::geteuid());
        test(); std::_Exit(0);
    }
    int status{}; CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void SearchOnlyAncestorCanPersist() {
    Unprivileged([] {
        char pattern[] = "/tmp/pine-resource-search-only-XXXXXX";
        const auto created = ::mkdtemp(pattern); CHECK(created);
        const fs::path base(created), ancestor = base / "search-only", leaf = ancestor / "private";
        CHECK(fs::create_directories(leaf));
        CHECK(!::chmod(ancestor.c_str(), 0111));
        errno = 0;
        const auto ancestorRead = ::open(ancestor.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        CHECK(ancestorRead < 0 && errno == EACCES); // Proves this is not a root bypass.
        const auto directLeaf = ::open(leaf.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        CHECK(directLeaf >= 0); CHECK(!::close(directLeaf));
        RecentResourceWindow window; CHECK(window.Add(1, "{\"search_only\":true}"));
        CHECK(Persist(window, leaf)); Cleared(window);
        CHECK(Read(leaf / "resource-window.jsonl").ends_with("{\"search_only\":true}\n"));
        CHECK(!::chmod(ancestor.c_str(), 0700));
        CHECK(fs::remove_all(base) >= 4);
    });
}

static void RealPermissionFailuresAndRecovery() {
    Unprivileged([] {
        char pattern[] = "/tmp/pine-resource-permission-XXXXXX";
        const auto created = ::mkdtemp(pattern); CHECK(created);
        const fs::path base(created), ancestor = base / "ancestor", leaf = ancestor / "private";
        CHECK(fs::create_directories(leaf));
        const auto target = leaf / "resource-window.jsonl", temporary = leaf / "resource-window.tmp";
        RecentResourceWindow window; CHECK(window.Add(1, "{\"new\":true}"));
        constexpr std::string_view old = "{\"complete_old_target\":true}\n";
        const auto verifyAndRecover = [&] {
            CHECK(Read(target) == old);
            CHECK(Persist(window, leaf)); Cleared(window);
            CHECK(Read(target).ends_with("{\"new\":true}\n"));
            Write(target, old);
        };
        Write(target, old);
        CHECK(!::chmod(ancestor.c_str(), 0000)); CHECK(!Persist(window, leaf));
        CHECK(!::chmod(ancestor.c_str(), 0700));
        Failure(window, Stage::OpenDirectory, FailureReason::SyscallError, EACCES); verifyAndRecover();
        CHECK(!::chmod(leaf.c_str(), 0000)); CHECK(!Persist(window, leaf));
        CHECK(!::chmod(leaf.c_str(), 0700));
        Failure(window, Stage::InspectTarget, FailureReason::SyscallError, EACCES); verifyAndRecover();
        CHECK(!::chmod(leaf.c_str(), 0500)); CHECK(!Persist(window, leaf));
        CHECK(!::chmod(leaf.c_str(), 0700));
        Failure(window, Stage::OpenTemporary, FailureReason::SyscallError, EACCES); verifyAndRecover();
        Write(temporary, "owned-stale-temporary");
        CHECK(!::chmod(leaf.c_str(), 0500)); CHECK(!Persist(window, leaf));
        CHECK(!::chmod(leaf.c_str(), 0700));
        Failure(window, Stage::RemoveTemporary, FailureReason::SyscallError, EACCES);
        CHECK(Read(temporary) == "owned-stale-temporary"); verifyAndRecover();
        CHECK(!fs::exists(temporary)); CHECK(fs::remove_all(base) >= 4);
    });
}

struct MutationIo : resource_window_detail::PosixIo {
    enum class Mode { Normal, RemoveTemporary, SwapTemporary, SwapTarget, RemoveTarget, SymlinkTarget };
    static inline Mode mode{};
    static inline fs::path directory;
    static int Sync(int fd) {
        if (::fsync(fd)) return -1;
        if (mode == Mode::Normal) return 0;
        const auto temporary = directory / "resource-window.tmp", target = directory / "resource-window.jsonl", held = directory / "held-complete-file";
        if (mode == Mode::RemoveTemporary) CHECK(!::unlink(temporary.c_str()));
        else if (mode == Mode::SwapTemporary) {
            CHECK(!::rename(temporary.c_str(), held.c_str()));
            ::Write(temporary, "{\"another_temporary\":true}\n");
        } else {
            CHECK(!::rename(target.c_str(), held.c_str()));
            if (mode == Mode::SwapTarget) ::Write(target, "{\"another_complete_target\":true}\n");
            else if (mode == Mode::SymlinkTarget) fs::create_symlink(held, target);
        }
        return 0;
    }
};
static void ChangedFilesFailWithoutReplacingAnotherFile() {
    unsigned index{};
    for (const auto mode : {MutationIo::Mode::RemoveTemporary, MutationIo::Mode::SwapTemporary,
        MutationIo::Mode::SwapTarget, MutationIo::Mode::RemoveTarget, MutationIo::Mode::SymlinkTarget}) {
        const auto name = "changed-file-" + std::to_string(index++); const auto directory = Directory(name.c_str());
        const auto target = directory / "resource-window.jsonl", temporary = directory / "resource-window.tmp", held = directory / "held-complete-file";
        constexpr std::string_view old = "{\"old_complete_target\":true}\n";
        Write(target, old);
        BasicRecentResourceWindow<MutationIo> window; CHECK(window.Add(1, "{\"new\":true}"));
        MutationIo::directory = directory; MutationIo::mode = mode;
        CHECK(!window.Persist(directory.string(), "synthetic", reason));
        if (mode == MutationIo::Mode::RemoveTemporary) {
            Failure(window, Stage::RecheckTemporary, FailureReason::SyscallError, ENOENT);
            CHECK(Read(target) == old && !fs::exists(temporary));
        } else if (mode == MutationIo::Mode::SwapTemporary) {
            Failure(window, Stage::RecheckTemporary, FailureReason::ChangedFile);
            CHECK(Read(target) == old && Read(temporary) == "{\"another_temporary\":true}\n");
        } else {
            CHECK(Read(held) == old && !fs::exists(temporary));
            if (mode == MutationIo::Mode::SymlinkTarget) {
                Failure(window, Stage::RecheckTarget, FailureReason::PolicyRejected); CHECK(fs::is_symlink(target));
            } else {
                Failure(window, Stage::RecheckTarget, FailureReason::ChangedFile);
                if (mode == MutationIo::Mode::SwapTarget) CHECK(Read(target) == "{\"another_complete_target\":true}\n");
                else CHECK(!fs::exists(target));
            }
        }
        // Restore the fixture, then retry on this instance with real POSIX IO.
        fs::remove(temporary); fs::remove(target); Write(target, old);
        MutationIo::mode = MutationIo::Mode::Normal;
        CHECK(window.Add(2, "{\"recovered\":true}"));
        CHECK(window.Persist(directory.string(), "synthetic", reason)); Cleared(window);
        CHECK(Read(target).ends_with("{\"recovered\":true}\n"));
    }
}

static void FixedFailureNamesAndPersistentReset() {
    struct StageName { Stage value; const char *name; };
    constexpr StageName stages[]{
        {Stage::None,"none"}, {Stage::Validate,"validate"}, {Stage::Metadata,"metadata"},
        {Stage::OpenDirectory,"open_directory"}, {Stage::InspectTarget,"inspect_target"},
        {Stage::InspectTemporary,"inspect_temporary"}, {Stage::RemoveTemporary,"remove_temporary"},
        {Stage::OpenTemporary,"open_temporary"}, {Stage::ValidateTemporary,"validate_temporary"},
        {Stage::SetPermissions,"set_permissions"}, {Stage::WriteMetadata,"write_metadata"},
        {Stage::WriteRecords,"write_records"}, {Stage::VerifySize,"verify_size"}, {Stage::Sync,"sync"},
        {Stage::Close,"close"}, {Stage::RecheckTemporary,"recheck_temporary"},
        {Stage::RecheckTarget,"recheck_target"}, {Stage::Rename,"rename"}, {Stage::Exception,"exception"},
    };
    struct ReasonName { FailureReason value; const char *name; };
    constexpr ReasonName reasons[]{
        {FailureReason::None,"none"}, {FailureReason::InvalidInput,"invalid_input"},
        {FailureReason::PolicyRejected,"policy_rejected"}, {FailureReason::SizeLimit,"size_limit"},
        {FailureReason::SyscallError,"syscall_error"}, {FailureReason::ZeroWrite,"zero_write"},
        {FailureReason::InvalidWriteCount,"invalid_write_count"}, {FailureReason::ChangedFile,"changed_file"},
        {FailureReason::AllocationFailure,"allocation_failure"}, {FailureReason::UnknownException,"unknown_exception"},
    };
    failAllocation = true;
    for (const auto pair : stages) CHECK(std::string_view(ResourceWindowFailureStageName(pair.value)) == pair.name);
    for (const auto pair : reasons) CHECK(std::string_view(ResourceWindowFailureReasonName(pair.value)) == pair.name);
    CHECK(std::string_view(ResourceWindowFailureStageName(static_cast<Stage>(255))) == "unknown_stage");
    CHECK(std::string_view(ResourceWindowFailureReasonName(static_cast<FailureReason>(255))) == "unknown_reason");
    CHECK(std::string_view(ResourceWindowErrnoName(0)) == "none");
    CHECK(std::string_view(ResourceWindowErrnoName(EACCES)) == "EACCES");
    CHECK(std::string_view(ResourceWindowErrnoName(ENOSPC)) == "ENOSPC");
    CHECK(std::string_view(ResourceWindowErrnoName(EIO)) == "EIO");
    CHECK(std::string_view(ResourceWindowErrnoName(-1)) == "other_errno");
    CHECK(std::string_view(ResourceWindowErrnoName(std::numeric_limits<int>::max())) == "other_errno");
    failAllocation = false;
    RecentResourceWindow window; Cleared(window);
    CHECK(!window.Persist("private-path-canary", "", reason));
    Failure(window, Stage::Validate, FailureReason::InvalidInput);
    CHECK(window.Add(1, "{}")); CHECK(!window.Add(2, "not-an-object"));
    Failure(window, Stage::Validate, FailureReason::InvalidInput);
    window.Reset(); Cleared(window);
    CHECK(!window.Persist(std::string(4097, 'x'), "synthetic", reason));
    Failure(window, Stage::Validate, FailureReason::SizeLimit);
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

static void CriticalSurvivesDenseRecentAndPreservesRawOrder() {
    RecentResourceWindow window;
    struct Item { std::string line; bool critical; unsigned recentOrdinal; };
    std::vector<Item> admitted;
    uint64_t sequence{};
    const auto critical = [&](uint64_t now, const char *event) {
        auto line = Record(++sequence, event);
        CHECK(window.Add(now, line, Retention::Critical)); admitted.push_back({std::move(line), true, 0}); Bounds(window);
    };
    critical(0, "GUEST_BREAK_REQUEST");
    for (unsigned index = 1; index <= 100; ++index) {
        auto line = Record(++sequence, "RESOURCE_GPU_SAMPLE", 2000);
        CHECK(window.Add(index * 400000ULL, line)); admitted.push_back({std::move(line), false, index});
        if (index == 20) critical(index * 400000ULL, "GUEST_SIGNAL");
        if (index == 80) critical(index * 400000ULL, "GUEST_RUNTIME_ERROR");
        Bounds(window);
    }
    CHECK(window.CriticalCount() == 3 && window.RecentCount() > 0 && window.RecentCount() < 100);
    CHECK(window.Stats().evictedCritical == 0 && window.Stats().evictedRecent == 100 - window.RecentCount());
    CHECK(window.Stats().evictedRecentBytes == 2000 * window.Stats().evictedRecent);
    const auto directory = Directory("layered-dense");
    static_assert(static_cast<uint8_t>(ResourceWindowReason::GuestBreakObserved) == 7);
    CHECK(window.Persist(directory.string(), "synthetic-session", ResourceWindowReason::GuestBreakObserved));
    std::string expected;
    for (const auto &entry : admitted)
        if (entry.critical || entry.recentOrdinal > 100 - window.RecentCount()) expected += entry.line;
    CHECK(RecordsOnly(directory) == expected); // Every timestamp/ID/sequence byte unchanged.
    CHECK(Read(directory / "resource-window.jsonl").find("guest_break_observed") != std::string::npos);
}

static void CriticalCountCeilingAndEqualTimeAdmissionOrder() {
    RecentResourceWindow window;
    for (uint64_t sequence = 1; sequence <= 16; ++sequence) CHECK(window.Add(1, Record(sequence, "CRITICAL"), Retention::Critical));
    for (uint64_t sequence = 17; sequence <= 256; ++sequence) CHECK(window.Add(1, Record(sequence, "RECENT")));
    CHECK(window.CriticalCount() == 16 && window.RecentCount() == 224 && window.Count() == 240);
    CHECK(window.Add(1, Record(257, "CRITICAL"), Retention::Critical)); Bounds(window);
    CHECK(window.Stats().evictedCritical == 1 && window.Stats().evictedRecent == 16 && window.Stats().evictedCount == 17);
    std::string expected;
    for (uint64_t sequence = 2; sequence <= 16; ++sequence) expected += Record(sequence, "CRITICAL");
    for (uint64_t sequence = 33; sequence <= 256; ++sequence) expected += Record(sequence, "RECENT");
    expected += Record(257, "CRITICAL");
    const auto directory = Directory("layered-count"); CHECK(Persist(window, directory));
    CHECK(RecordsOnly(directory) == expected); // Equal ingestion timestamps are not sorted by class.
}

static void CriticalByteCeilingAndBorrowingUnusedReserve() {
    RecentResourceWindow window;
    const auto full = Line(window.MaxLineBytes);
    CHECK(window.Add(1, full, Retention::Critical));
    CHECK(window.Add(1, full)); CHECK(window.Add(1, full));
    CHECK(window.Add(1, Line(window.MaxDataBytes - 3 * full.size())));
    CHECK(window.Bytes() == window.MaxDataBytes && window.CriticalBytes() == window.MaxCriticalBytes);
    const auto before = Directory("layered-full-budget");
    CHECK(Persist(window, before, std::string(window.MaxSessionBytes, '\1')));
    CHECK(window.Add(1, "{}")); Bounds(window);
    CHECK(window.Stats().evictedRecent == 1 && window.Stats().evictedCritical == 0);
    CHECK(window.CriticalBytes() == full.size());
    CHECK(window.Add(1, Line(1024), Retention::Critical)); Bounds(window);
    CHECK(window.Stats().evictedCritical == 1 && window.Stats().evictedCriticalBytes == full.size());
    CHECK(window.CriticalCount() == 1 && window.CriticalBytes() == 1024);
    CHECK(window.Add(1, full)); Bounds(window);
    CHECK(window.RecentBytes() > window.MaxDataBytes - window.MaxCriticalBytes); // Borrowed unused reserve.
    CHECK(!window.Add(1, Line(window.MaxLineBytes + 1), Retention::Critical));
    CHECK(window.Stats().omittedOversize == 1 && window.CriticalBytes() == 1024);
    CHECK(Persist(window, Directory("layered-bytes"), std::string(window.MaxSessionBytes, '\1')));
}

static void BothClassesExpireResetAndHandleUint64ClockBounds() {
    RecentResourceWindow window;
    CHECK(window.Add(0, Record(1, "CRITICAL"), Retention::Critical));
    CHECK(window.Add(1, Record(2, "RECENT")));
    CHECK(window.Add(2, Record(3, "CRITICAL"), Retention::Critical));
    CHECK(window.Add(window.MaxAgeUs - 1, Record(4, "RECENT")));
    CHECK(window.Add(window.MaxAgeUs, Record(5, "RECENT"))); Bounds(window);
    CHECK(window.CriticalCount() == 1 && window.RecentCount() == 3 && window.Stats().evictedAge == 1);
    CHECK(window.Stats().evictedCritical == 1 && window.Stats().evictedRecent == 0);
    CHECK(Persist(window, Directory("layered-age")));
    CHECK(window.Add(10, Record(6, "CRITICAL"), Retention::Critical)); Bounds(window);
    CHECK(window.Count() == 1 && window.CriticalCount() == 1 && window.Stats().evictedClock == 4);
    CHECK(Persist(window, Directory("layered-clock")));
    window.Reset(); Bounds(window);
    CHECK(window.CriticalCount() == 0 && window.CriticalBytes() == 0 && window.Evictions() == 0);
    constexpr auto maximum = std::numeric_limits<uint64_t>::max();
    CHECK(window.Add(maximum - window.MaxAgeUs, Record(7, "CRITICAL"), Retention::Critical));
    CHECK(window.Add(maximum - window.MaxAgeUs + 1, Record(8, "CRITICAL"), Retention::Critical));
    CHECK(window.Add(maximum, Record(9, "RECENT"))); Bounds(window);
    CHECK(window.CriticalCount() == 1 && window.RecentCount() == 1 && window.Stats().evictedAge == 1);
    CHECK(Persist(window, Directory("layered-max-time")));
    const auto changed = Directory("layered-session");
    CHECK(Persist(window, changed, "new-session")); Bounds(window);
    CHECK(window.Count() == 0 && window.CriticalCount() == 0 && window.Bytes() == 0 && window.Evictions() == 0);
}

static void CriticalAdmissionFailureKeepsClassAccountingAndOriginalFile() {
    RecentResourceWindow window;
    CHECK(window.Add(1, Record(1, "CRITICAL"), Retention::Critical));
    CHECK(window.Add(2, Record(2, "RECENT")));
    const auto directory = Directory("layered-allocation"); CHECK(Persist(window, directory));
    const auto original = Read(directory / "resource-window.jsonl");
    const auto count = window.CriticalCount(), bytes = window.CriticalBytes(), total = window.Bytes();
    const auto line = Record(3, "CRITICAL", 4000);
    failAllocation = true;
    const auto added = window.Add(3, line, Retention::Critical);
    failAllocation = false;
    CHECK(!added && window.Stats().omittedAllocation == 1);
    CHECK(window.CriticalCount() == count && window.CriticalBytes() == bytes && window.Bytes() == total); Bounds(window);
    CHECK(!window.Add(4, "{}", static_cast<Retention>(255)) && window.Stats().omittedInvalid == 1); Bounds(window);
    const auto path = directory.string();
    failAllocation = true; const auto saved = window.Persist(path, "synthetic-session", reason); failAllocation = false;
    CHECK(!saved && Read(directory / "resource-window.jsonl") == original);
    Failure(window, Stage::Metadata, FailureReason::AllocationFailure);
    CHECK(Persist(window, directory)); Cleared(window); Bounds(window);
    RecentResourceWindow defaultClass;
    CHECK(defaultClass.Add(1, Record(1, "GUEST_BREAK_REQUEST")));
    CHECK(defaultClass.CriticalCount() == 0 && defaultClass.RecentCount() == 1); // No event-name inference.
}

static void LayeredReferenceModelAndPersistence() {
    struct Entry { uint64_t now; std::string line; bool critical; };
    std::vector<Entry> expected;
    RecentResourceWindow window;
    uint64_t seed = 19, now{}, recentEvictions{}, criticalEvictions{}, recentBytes{}, criticalBytes{};
    const auto evict = [&](size_t index) {
        const auto &entry = expected[index];
        if (entry.critical) { ++criticalEvictions; criticalBytes += entry.line.size(); }
        else { ++recentEvictions; recentBytes += entry.line.size(); }
        expected.erase(expected.begin() + static_cast<std::ptrdiff_t>(index));
    };
    const auto totalBytes = [&] { size_t bytes{}; for (const auto &entry : expected) bytes += entry.line.size(); return bytes; };
    const auto classCount = [&] { size_t count{}; for (const auto &entry : expected) count += entry.critical; return count; };
    const auto classBytes = [&] { size_t bytes{}; for (const auto &entry : expected) if (entry.critical) bytes += entry.line.size(); return bytes; };
    const auto firstClass = [&](bool critical) {
        const auto found = std::find_if(expected.begin(), expected.end(), [&](const auto &entry) { return entry.critical == critical; });
        CHECK(found != expected.end()); return static_cast<size_t>(found - expected.begin());
    };
    for (uint64_t sequence = 1; sequence <= 1000; ++sequence) {
        seed = seed * 6364136223846793005ULL + 1; // Defined unsigned wrap; deterministic synthetic admissions.
        if (sequence == 333) { while (!expected.empty()) evict(0); now = 7; }
        else now += 100000;
        while (!expected.empty() && now - expected.front().now >= window.MaxAgeUs) evict(0);
        const bool critical = (seed >> 32) % 7 == 0;
        const auto line = Record(sequence, critical ? "CRITICAL" : "RECENT", sequence % 31 == 0 ? window.MaxLineBytes : 512 + seed % 3500);
        expected.push_back({now, line, critical});
        while (classCount() > window.MaxCriticalRecords) evict(firstClass(true));
        while (classBytes() > window.MaxCriticalBytes) evict(firstClass(true));
        while (expected.size() > window.MaxRecords) evict(firstClass(false));
        while (totalBytes() > window.MaxDataBytes) evict(firstClass(false));
        CHECK(window.Add(now, line, critical ? Retention::Critical : Retention::Recent)); Bounds(window);
        CHECK(window.Count() == expected.size() && window.Bytes() == totalBytes());
        CHECK(window.CriticalCount() == classCount() && window.CriticalBytes() == classBytes());
        CHECK(window.Stats().evictedCritical == criticalEvictions && window.Stats().evictedRecent == recentEvictions);
        CHECK(window.Stats().evictedCriticalBytes == criticalBytes && window.Stats().evictedRecentBytes == recentBytes);
        if (sequence == 100 || sequence == 200 || sequence == 400 || sequence == 1000) {
            const auto name = "layered-model-" + std::to_string(sequence); const auto directory = Directory(name.c_str());
            CHECK(Persist(window, directory));
            std::string rows; for (const auto &entry : expected) rows += entry.line;
            CHECK(RecordsOnly(directory) == rows);
        }
    }
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
        {"real unprivileged search-only ancestor supports private leaf persistence", SearchOnlyAncestorCanPersist},
        {"real POSIX permission failures retain exact stage/errno and recover", RealPermissionFailuresAndRecovery},
        {"changed temporary/target identities fail without replacing another file", ChangedFilesFailWithoutReplacingAnotherFile},
        {"fixed allocation-free detail names and persistent failure/reset", FixedFailureNamesAndPersistentReset},
        {"safe stale owned temporary recovery", StaleOwnedTemporaryCanRecover},
        {"explicit exception reason preserves caller-observed record", ExceptionReasonPreservesObservedRecord},
        {"ingestion-time window preserves out-of-order original event timestamps", IngestionTimeDoesNotReinterpretEventOrder},
        {"critical rows survive dense Recent without raw-row or admission-order changes", CriticalSurvivesDenseRecentAndPreservesRawOrder},
        {"16-critical count ceiling and equal-time admission order", CriticalCountCeilingAndEqualTimeAdmissionOrder},
        {"32KiB critical ceiling and unused reserve borrowing", CriticalByteCeilingAndBorrowingUnusedReserve},
        {"both classes share 60s age/reset and uint64 clock boundaries", BothClassesExpireResetAndHandleUint64ClockBounds},
        {"critical admission allocation failure keeps original file/accounting", CriticalAdmissionFailureKeepsClassAccountingAndOriginalFile},
        {"1000-admission independent reference model and exact persisted rows", LayeredReferenceModelAndPersistence},
    };
    for (const auto &test : cases) { test.run(); std::printf("PASS: %s\n", test.name); }
    std::printf("PASS: %zu synthetic resource-window cases; object_bytes=%zu payload_cap=%zu metadata_reserve=%zu; no sink/Guest integration\n",
        std::size(cases), sizeof(RecentResourceWindow), RecentResourceWindow::MaxDataBytes, RecentResourceWindow::MetadataReserveBytes);
}
