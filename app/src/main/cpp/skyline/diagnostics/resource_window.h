// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace skyline::diagnostics {
enum class ResourceWindowReason : uint8_t { Periodic, PostProgressNoNewPresent, Recovered, LaunchEnd, SessionEnd, Shutdown, ExceptionObserved, GuestBreakObserved };
enum class ResourceWindowRetention : uint8_t { Recent, Critical };

enum class ResourceWindowFailureStage : uint8_t {
    None, Validate, Metadata, OpenDirectory, InspectTarget, InspectTemporary,
    RemoveTemporary, OpenTemporary, ValidateTemporary, SetPermissions,
    WriteMetadata, WriteRecords, VerifySize, Sync, Close, RecheckTemporary,
    RecheckTarget, Rename, Exception,
};
enum class ResourceWindowFailureReason : uint8_t {
    None, InvalidInput, PolicyRejected, SizeLimit, SyscallError, ZeroWrite,
    InvalidWriteCount, ChangedFile, AllocationFailure, UnknownException,
};
// No path, session text, exception message, or dynamically allocated detail.
struct ResourceWindowFailure {
    ResourceWindowFailureStage stage;
    ResourceWindowFailureReason reason;
    int error; // Captured syscall errno, or zero for a non-syscall rejection.
};
static_assert(std::is_trivial_v<ResourceWindowFailure> && std::is_standard_layout_v<ResourceWindowFailure>);

inline const char *ResourceWindowFailureStageName(ResourceWindowFailureStage stage) noexcept {
    switch (stage) {
        case ResourceWindowFailureStage::None: return "none";
        case ResourceWindowFailureStage::Validate: return "validate";
        case ResourceWindowFailureStage::Metadata: return "metadata";
        case ResourceWindowFailureStage::OpenDirectory: return "open_directory";
        case ResourceWindowFailureStage::InspectTarget: return "inspect_target";
        case ResourceWindowFailureStage::InspectTemporary: return "inspect_temporary";
        case ResourceWindowFailureStage::RemoveTemporary: return "remove_temporary";
        case ResourceWindowFailureStage::OpenTemporary: return "open_temporary";
        case ResourceWindowFailureStage::ValidateTemporary: return "validate_temporary";
        case ResourceWindowFailureStage::SetPermissions: return "set_permissions";
        case ResourceWindowFailureStage::WriteMetadata: return "write_metadata";
        case ResourceWindowFailureStage::WriteRecords: return "write_records";
        case ResourceWindowFailureStage::VerifySize: return "verify_size";
        case ResourceWindowFailureStage::Sync: return "sync";
        case ResourceWindowFailureStage::Close: return "close";
        case ResourceWindowFailureStage::RecheckTemporary: return "recheck_temporary";
        case ResourceWindowFailureStage::RecheckTarget: return "recheck_target";
        case ResourceWindowFailureStage::Rename: return "rename";
        case ResourceWindowFailureStage::Exception: return "exception";
    }
    return "unknown_stage";
}
inline const char *ResourceWindowFailureReasonName(ResourceWindowFailureReason reason) noexcept {
    switch (reason) {
        case ResourceWindowFailureReason::None: return "none";
        case ResourceWindowFailureReason::InvalidInput: return "invalid_input";
        case ResourceWindowFailureReason::PolicyRejected: return "policy_rejected";
        case ResourceWindowFailureReason::SizeLimit: return "size_limit";
        case ResourceWindowFailureReason::SyscallError: return "syscall_error";
        case ResourceWindowFailureReason::ZeroWrite: return "zero_write";
        case ResourceWindowFailureReason::InvalidWriteCount: return "invalid_write_count";
        case ResourceWindowFailureReason::ChangedFile: return "changed_file";
        case ResourceWindowFailureReason::AllocationFailure: return "allocation_failure";
        case ResourceWindowFailureReason::UnknownException: return "unknown_exception";
    }
    return "unknown_reason";
}
inline const char *ResourceWindowErrnoName(int error) noexcept {
    switch (error) {
        case 0: return "none";
        case EACCES: return "EACCES";
        case EPERM: return "EPERM";
        case ENOENT: return "ENOENT";
        case ENOTDIR: return "ENOTDIR";
        case ELOOP: return "ELOOP";
        case EEXIST: return "EEXIST";
        case ENAMETOOLONG: return "ENAMETOOLONG";
        case ENOSPC: return "ENOSPC";
        case EDQUOT: return "EDQUOT";
        case EROFS: return "EROFS";
        case EIO: return "EIO";
        case EBADF: return "EBADF";
        case EINVAL: return "EINVAL";
        case EINTR: return "EINTR";
        case EAGAIN: return "EAGAIN";
        case EMFILE: return "EMFILE";
        case ENFILE: return "ENFILE";
        case ENOMEM: return "ENOMEM";
        case EFBIG: return "EFBIG";
        case EOVERFLOW: return "EOVERFLOW";
        case EBUSY: return "EBUSY";
        case EXDEV: return "EXDEV";
        case ENOSYS: return "ENOSYS";
        case ENOTEMPTY: return "ENOTEMPTY";
    }
    return "other_errno";
}

namespace resource_window_detail {
// The default performs real POSIX operations. Test instantiations can inject a
// bounded write/sync/close/rename failure while retaining the actual safety and
// formatting code. There is no runtime/global hook or production sink change.
struct PosixIo {
    static ssize_t Write(int fd, const void *data, size_t size) noexcept { return ::write(fd, data, size); }
    static int Sync(int fd) noexcept { return ::fsync(fd); }
    static int Close(int fd) noexcept { return ::close(fd); }
    static int Rename(int dir, const char *from, const char *to) noexcept { return ::renameat(dir, from, dir, to); }
};
struct Descriptor {
    int value{-1};
    ~Descriptor() { if (value >= 0) ::close(value); }
    explicit Descriptor(int fd = -1) noexcept : value(fd) {}
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
};
inline bool OwnedRegular(const struct stat &value) noexcept {
    return S_ISREG(value.st_mode) && value.st_nlink == 1 && value.st_uid == ::geteuid();
}
inline bool SameFile(const struct stat &a, const struct stat &b) noexcept { return a.st_dev == b.st_dev && a.st_ino == b.st_ino; }
inline int OpenDirectory(std::string_view path, ResourceWindowFailure &failure) {
    const auto reject = [&](ResourceWindowFailureStage stage, ResourceWindowFailureReason reason, int error = 0) {
        failure = {stage, reason, error}; return -1;
    };
    if (path.size() > 4096) return reject(ResourceWindowFailureStage::Validate, ResourceWindowFailureReason::SizeLimit);
    if (path.empty() || path.find('\0') != path.npos) return reject(ResourceWindowFailureStage::Validate, ResourceWindowFailureReason::InvalidInput);
    for (size_t position{}; position < path.size();) {
        const auto end = path.find('/', position);
        if (path.substr(position, end == path.npos ? path.size() - position : end - position) == "..")
            return reject(ResourceWindowFailureStage::Validate, ResourceWindowFailureReason::InvalidInput);
        if (end == path.npos) break;
        position = end + 1;
    }
    // Traversal needs search permission, not permission to list every ancestor.
    // O_NOFOLLOW|O_DIRECTORY still rejects symlink components with O_PATH.
#if defined(O_PATH)
    const int flags = O_PATH | O_DIRECTORY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
#elif defined(O_SEARCH)
    const int flags = O_SEARCH | O_DIRECTORY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
#else
    return reject(ResourceWindowFailureStage::OpenDirectory, ResourceWindowFailureReason::PolicyRejected);
    const int flags = 0;
#endif
    Descriptor directory(::open(path.front() == '/' ? "/" : ".", flags));
    if (directory.value < 0) return reject(ResourceWindowFailureStage::OpenDirectory, ResourceWindowFailureReason::SyscallError, errno);
    size_t position{};
    while (position < path.size()) {
        const auto end = path.find('/', position);
        const auto part = path.substr(position, end == path.npos ? path.size() - position : end - position);
        if (!part.empty() && part != ".") {
            const std::string component(part);
            const int next = ::openat(directory.value, component.c_str(), flags);
            if (next < 0) return reject(ResourceWindowFailureStage::OpenDirectory, ResourceWindowFailureReason::SyscallError, errno);
            ::close(directory.value); directory.value = next;
        }
        if (end == path.npos) break;
        position = end + 1;
    }
    struct stat identity{};
    if (::fstat(directory.value, &identity)) return reject(ResourceWindowFailureStage::OpenDirectory, ResourceWindowFailureReason::SyscallError, errno);
    if (!S_ISDIR(identity.st_mode) || identity.st_uid != ::geteuid())
        return reject(ResourceWindowFailureStage::OpenDirectory, ResourceWindowFailureReason::PolicyRejected);
    const int result = directory.value; directory.value = -1; return result;
}
inline bool Utf8(std::string_view value) noexcept {
    for (size_t i{}; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i++]);
        if (first < 0x80) continue;
        size_t count{}; uint32_t code{}, minimum{};
        if (first >= 0xc2 && first <= 0xdf) { count = 1; code = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { count = 2; code = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 3; code = first & 7; minimum = 0x10000; }
        else return false;
        if (count > value.size() - i) return false;
        while (count--) {
            const auto next = static_cast<unsigned char>(value[i++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}
inline std::string Quote(std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result("\"");
    for (const auto raw : value) {
        const auto byte = static_cast<unsigned char>(raw);
        if (byte == '"' || byte == '\\') { result += '\\'; result += raw; }
        else if (byte < 0x20) { result += "\\u00"; result += digits[byte >> 4]; result += digits[byte & 15]; }
        else result += raw;
    }
    return result + '"';
}
}

// Serialized writer-thread ownership only. The byte budget is the retained
// serialized payload, not a claim about std::deque/allocator bookkeeping. The
// optional queue makes construction/reset allocation-free; Add owns its copy.
template<class Io = resource_window_detail::PosixIo> class BasicRecentResourceWindow {
  public:
    static constexpr size_t MaxRecords = 240;
    static constexpr size_t MaxFileBytes = 128 * 1024;
    static constexpr size_t MetadataReserveBytes = 2048;
    static constexpr size_t MaxDataBytes = MaxFileBytes - MetadataReserveBytes;
    static constexpr size_t MaxLineBytes = 32 * 1024; // Includes one terminating LF.
    static constexpr size_t MaxCriticalRecords = 16;
    static constexpr size_t MaxCriticalBytes = 32 * 1024;
    static constexpr uint64_t MaxAgeUs = 60 * 1000000ULL;
    static constexpr size_t MaxSessionBytes = 128;
    struct Statistics {
        uint64_t evictedAge{}, evictedBytes{}, evictedCount{}, evictedClock{};
        uint64_t ageBytes{}, budgetBytes{}, countBytes{}, clockBytes{};
        uint64_t omittedOversize{}, omittedInvalid{}, omittedAllocation{}, clockResets{};
        uint64_t evictedRecent{}, evictedCritical{}, evictedRecentBytes{}, evictedCriticalBytes{};
    };

  private:
    struct Entry { uint64_t observedUs; std::string line; ResourceWindowRetention retention; };
    static_assert(std::is_nothrow_move_assignable_v<Entry>);
    static_assert(MaxCriticalRecords < MaxRecords && MaxCriticalBytes < MaxDataBytes && MaxLineBytes <= MaxCriticalBytes);
    std::optional<std::deque<Entry>> entries;
    size_t bytes{};
    size_t criticalCount{}, criticalBytes{};
    uint64_t lastObservedUs{};
    bool haveTime{}, sessionBound{};
    std::array<char, MaxSessionBytes> sessionBytes{};
    size_t sessionLength{};
    Statistics statistics{};
    ResourceWindowFailure lastFailure{};
    bool Fail(ResourceWindowFailureStage stage, ResourceWindowFailureReason reason, int error = 0) noexcept {
        lastFailure = {stage, reason, error}; return false;
    }
    static void AddCounter(uint64_t &value, uint64_t amount = 1) noexcept {
        constexpr auto max = std::numeric_limits<uint64_t>::max();
        value = amount > max - value ? max : value + amount;
    }
    void Evict(uint64_t &records, uint64_t &evictedBytes, size_t index = 0) noexcept {
        const auto &entry = (*entries)[index];
        const auto size = entry.line.size();
        if (entry.retention == ResourceWindowRetention::Critical) {
            --criticalCount; criticalBytes -= size;
            AddCounter(statistics.evictedCritical); AddCounter(statistics.evictedCriticalBytes, size);
        } else {
            AddCounter(statistics.evictedRecent); AddCounter(statistics.evictedRecentBytes, size);
        }
        bytes -= size;
        entries->erase(entries->begin() + static_cast<std::ptrdiff_t>(index));
        AddCounter(records); AddCounter(evictedBytes, size);
    }
    size_t Oldest(ResourceWindowRetention retention) const noexcept {
        for (size_t index{}; index < Size(); ++index) if ((*entries)[index].retention == retention) return index;
        return Size();
    }
    void Advance(uint64_t now) noexcept {
        if (haveTime && now < lastObservedUs) {
            AddCounter(statistics.clockResets);
            while (Size()) Evict(statistics.evictedClock, statistics.clockBytes);
        }
        haveTime = true; lastObservedUs = now;
        while (Size() && now - entries->front().observedUs >= MaxAgeUs) Evict(statistics.evictedAge, statistics.ageBytes);
    }
    static const char *ReasonName(ResourceWindowReason reason) noexcept {
        switch (reason) {
            case ResourceWindowReason::Periodic: return "periodic";
            case ResourceWindowReason::PostProgressNoNewPresent: return "post_progress_no_new_present";
            case ResourceWindowReason::Recovered: return "recovered";
            case ResourceWindowReason::LaunchEnd: return "launch_end";
            case ResourceWindowReason::SessionEnd: return "session_end";
            case ResourceWindowReason::Shutdown: return "shutdown";
            case ResourceWindowReason::ExceptionObserved: return "exception_observed";
            case ResourceWindowReason::GuestBreakObserved: return "guest_break_observed";
        }
        return nullptr;
    }
    std::string Metadata(std::string_view session, const char *reason) const {
        using resource_window_detail::Quote;
        std::string result = "{\"schema\":1,\"record_kind\":\"resource_window_metadata\",\"session_id\":" + Quote(session)
            + ",\"scope\":\"writer_retained_recent_window_not_complete_history\",\"reason\":" + Quote(reason)
            + ",\"time_scope\":\"writer_ingestion_monotonic_not_event_execution\""
            + ",\"as_of_monotonic_us\":" + (haveTime ? std::to_string(lastObservedUs) : "null")
            + ",\"retained_records\":" + std::to_string(Size()) + ",\"retained_bytes\":" + std::to_string(bytes)
            + ",\"earliest_monotonic_us\":" + (Size() ? std::to_string(entries->front().observedUs) : "null")
            + ",\"latest_monotonic_us\":" + (Size() ? std::to_string(entries->back().observedUs) : "null")
            + ",\"retained_span_us\":" + (Size() ? std::to_string(entries->back().observedUs - entries->front().observedUs) : "null")
            + ",\"window_limit_us\":" + std::to_string(MaxAgeUs) + ",\"record_limit\":" + std::to_string(MaxRecords)
            + ",\"file_byte_limit\":" + std::to_string(MaxFileBytes) + ",\"data_byte_limit\":" + std::to_string(MaxDataBytes);
        const auto number = [&](const char *name, uint64_t value) { result += ",\""; result += name; result += "\":"; result += std::to_string(value); };
        number("evicted_age", statistics.evictedAge); number("evicted_bytes", statistics.evictedBytes);
        number("evicted_count", statistics.evictedCount); number("evicted_clock", statistics.evictedClock);
        number("evicted_age_bytes", statistics.ageBytes); number("evicted_budget_bytes", statistics.budgetBytes);
        number("evicted_count_bytes", statistics.countBytes); number("evicted_clock_bytes", statistics.clockBytes);
        number("omitted_oversize", statistics.omittedOversize); number("omitted_invalid", statistics.omittedInvalid);
        number("omitted_allocation", statistics.omittedAllocation); number("clock_resets", statistics.clockResets);
        result += ",\"retention\":{\"policy\":\"critical_reserve_recent_borrows_unused\",\"age_scope\":\"both_ingestion\",\"eviction_scope\":\"age_clock_global_and_class_limits\",\"critical\":{\"records\":"
            + std::to_string(criticalCount) + ",\"bytes\":" + std::to_string(criticalBytes)
            + ",\"record_limit\":" + std::to_string(MaxCriticalRecords) + ",\"byte_limit\":" + std::to_string(MaxCriticalBytes)
            + ",\"evicted_records\":" + std::to_string(statistics.evictedCritical) + ",\"evicted_bytes\":" + std::to_string(statistics.evictedCriticalBytes)
            + "},\"recent\":{\"records\":" + std::to_string(RecentCount()) + ",\"bytes\":" + std::to_string(RecentBytes())
            + ",\"record_capacity\":" + std::to_string(MaxRecords - criticalCount) + ",\"byte_capacity\":" + std::to_string(MaxDataBytes - criticalBytes)
            + ",\"evicted_records\":" + std::to_string(statistics.evictedRecent) + ",\"evicted_bytes\":" + std::to_string(statistics.evictedRecentBytes) + "}}";
        return result + "}\n";
    }
    bool WriteAll(int fd, std::string_view value, ResourceWindowFailureStage stage) noexcept {
        unsigned interrupts{};
        while (!value.empty()) {
            const auto count = Io::Write(fd, value.data(), value.size());
            if (count < 0) {
                const int error = errno;
                if (error == EINTR && ++interrupts <= 32) continue;
                return Fail(stage, ResourceWindowFailureReason::SyscallError, error);
            }
            if (!count) return Fail(stage, ResourceWindowFailureReason::ZeroWrite);
            if (static_cast<size_t>(count) > value.size()) return Fail(stage, ResourceWindowFailureReason::InvalidWriteCount);
            interrupts = 0; value.remove_prefix(static_cast<size_t>(count));
        }
        return true;
    }
    bool Target(int directory, struct stat &identity, bool &exists, ResourceWindowFailureStage stage) noexcept {
        if (!::fstatat(directory, "resource-window.jsonl", &identity, AT_SYMLINK_NOFOLLOW)) {
            exists = true;
            return resource_window_detail::OwnedRegular(identity) || Fail(stage, ResourceWindowFailureReason::PolicyRejected);
        }
        const int error = errno;
        exists = false;
        return error == ENOENT || Fail(stage, ResourceWindowFailureReason::SyscallError, error);
    }
    struct Temporary {
        int directory, fd{-1};
        struct stat identity{};
        bool identified{}, committed{};
        ~Temporary() {
            if (fd >= 0) ::close(fd);
            struct stat current{};
            if (identified && !committed && !::fstatat(directory, "resource-window.tmp", &current, AT_SYMLINK_NOFOLLOW)
                && resource_window_detail::SameFile(identity, current)) ::unlinkat(directory, "resource-window.tmp", 0);
        }
    };

  public:
    BasicRecentResourceWindow() noexcept = default;
    size_t Size() const noexcept { return entries ? entries->size() : 0; }
    size_t Count() const noexcept { return Size(); }
    size_t Bytes() const noexcept { return bytes; }
    size_t CriticalCount() const noexcept { return criticalCount; }
    size_t CriticalBytes() const noexcept { return criticalBytes; }
    size_t RecentCount() const noexcept { return Size() - criticalCount; }
    size_t RecentBytes() const noexcept { return bytes - criticalBytes; }
    Statistics Stats() const noexcept { return statistics; }
    // Last Persist result only. Add omission counters are separate; later
    // samples do not erase an outstanding persistence failure. Reset clears it.
    ResourceWindowFailure LastFailure() const noexcept { return lastFailure; }
    uint64_t Evictions() const noexcept {
        uint64_t total{};
        AddCounter(total, statistics.evictedAge); AddCounter(total, statistics.evictedBytes);
        AddCounter(total, statistics.evictedCount); AddCounter(total, statistics.evictedClock);
        return total;
    }
    uint64_t Omissions() const noexcept {
        uint64_t total{};
        AddCounter(total, statistics.omittedOversize); AddCounter(total, statistics.omittedInvalid);
        AddCounter(total, statistics.omittedAllocation); return total;
    }
    void Reset() noexcept {
        entries.reset(); bytes = criticalCount = criticalBytes = 0; lastObservedUs = 0; haveTime = sessionBound = false;
        sessionBytes = {}; sessionLength = 0; statistics = {}; lastFailure = {};
    }
    // nowUs is writer-ingestion time, not the record's potentially out-of-order
    // event timestamp. Original serialized fields/timestamps remain untouched.
    // Precondition: one already-sanitized object from the sink Serialize path.
    // This checks physical framing, not JSON semantics; it never edits/truncates
    // the object. Exactly one optional trailing LF is normalized to one LF.
    // Critical has its own bounded ceiling; Recent borrows unused reserve. The
    // single deque preserves admission order, including equal ingestion times.
    // Only the writer owns these bounded allocations/linear <=241-entry erases.
    bool Add(uint64_t nowUs, std::string_view line, ResourceWindowRetention retention = ResourceWindowRetention::Recent) noexcept {
        Advance(nowUs);
        if (retention != ResourceWindowRetention::Recent && retention != ResourceWindowRetention::Critical) {
            AddCounter(statistics.omittedInvalid); return false;
        }
        if (line.size() > MaxLineBytes) { AddCounter(statistics.omittedOversize); return false; }
        if (!line.empty() && line.back() == '\n') line.remove_suffix(1);
        if (line.size() >= MaxLineBytes) { AddCounter(statistics.omittedOversize); return false; }
        if (line.size() < 2 || line.front() != '{' || line.back() != '}' || line.find_first_of("\r\n\0", 0, 3) != line.npos) {
            AddCounter(statistics.omittedInvalid); return false;
        }
        try {
            std::string owned(line.size() + 1, '\0');
            std::memcpy(owned.data(), line.data(), line.size()); owned.back() = '\n';
            if (!entries) entries.emplace();
            const auto added = owned.size();
            entries->push_back({nowUs, std::move(owned), retention}); bytes += added;
            if (retention == ResourceWindowRetention::Critical) { ++criticalCount; criticalBytes += added; }
            while (criticalCount > MaxCriticalRecords)
                Evict(statistics.evictedCount, statistics.countBytes, Oldest(ResourceWindowRetention::Critical));
            while (criticalBytes > MaxCriticalBytes)
                Evict(statistics.evictedBytes, statistics.budgetBytes, Oldest(ResourceWindowRetention::Critical));
            while (Size() > MaxRecords)
                Evict(statistics.evictedCount, statistics.countBytes, Oldest(ResourceWindowRetention::Recent));
            while (bytes > MaxDataBytes)
                Evict(statistics.evictedBytes, statistics.budgetBytes, Oldest(ResourceWindowRetention::Recent));
            return true;
        } catch (...) { AddCounter(statistics.omittedAllocation); return false; }
    }

    // Background writer only. Atomic replacement after every precommit step
    // succeeds; no assertion of crash durability of the containing directory.
    bool Persist(std::string_view directory, std::string_view session, ResourceWindowReason reason) noexcept {
        using namespace resource_window_detail;
        using Stage = ResourceWindowFailureStage;
        using FailureReason = ResourceWindowFailureReason;
        auto activeStage = Stage::Validate;
        try {
            const auto name = ReasonName(reason);
            if (session.size() > MaxSessionBytes) return Fail(Stage::Validate, FailureReason::SizeLimit);
            if (!name || session.empty() || !Utf8(session)) return Fail(Stage::Validate, FailureReason::InvalidInput);
            if (sessionBound && std::string_view(sessionBytes.data(), sessionLength) != session) Reset();
            if (!sessionBound) {
                std::memcpy(sessionBytes.data(), session.data(), session.size()); sessionLength = session.size(); sessionBound = true;
            }
            activeStage = Stage::Metadata;
            const auto metadata = Metadata(session, name);
            if (metadata.size() > MetadataReserveBytes || metadata.size() + bytes > MaxFileBytes) return Fail(Stage::Metadata, FailureReason::SizeLimit);
            activeStage = Stage::OpenDirectory;
            Descriptor folder(OpenDirectory(directory, lastFailure));
            if (folder.value < 0) return false;
            struct stat prior{}; bool priorExists{};
            if (!Target(folder.value, prior, priorExists, Stage::InspectTarget)) return false;
            struct stat stale{};
            if (!::fstatat(folder.value, "resource-window.tmp", &stale, AT_SYMLINK_NOFOLLOW)) {
                if (!OwnedRegular(stale)) return Fail(Stage::InspectTemporary, FailureReason::PolicyRejected);
                if (::unlinkat(folder.value, "resource-window.tmp", 0)) return Fail(Stage::RemoveTemporary, FailureReason::SyscallError, errno);
            } else {
                const int error = errno;
                if (error != ENOENT) return Fail(Stage::InspectTemporary, FailureReason::SyscallError, error);
            }
            Temporary temporary{folder.value};
            temporary.fd = ::openat(folder.value, "resource-window.tmp", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
            if (temporary.fd < 0) return Fail(Stage::OpenTemporary, FailureReason::SyscallError, errno);
            if (::fstat(temporary.fd, &temporary.identity)) return Fail(Stage::ValidateTemporary, FailureReason::SyscallError, errno);
            temporary.identified = true;
            if (!OwnedRegular(temporary.identity)) return Fail(Stage::ValidateTemporary, FailureReason::PolicyRejected);
            if (::fchmod(temporary.fd, 0600)) return Fail(Stage::SetPermissions, FailureReason::SyscallError, errno);
            if (!WriteAll(temporary.fd, metadata, Stage::WriteMetadata)) return false;
            if (entries) for (const auto &entry : *entries) if (!WriteAll(temporary.fd, entry.line, Stage::WriteRecords)) return false;
            struct stat complete{};
            if (::fstat(temporary.fd, &complete)) return Fail(Stage::VerifySize, FailureReason::SyscallError, errno);
            if (!OwnedRegular(complete)) return Fail(Stage::VerifySize, FailureReason::PolicyRejected);
            if (!SameFile(complete, temporary.identity) || complete.st_size < 0 || static_cast<uint64_t>(complete.st_size) != metadata.size() + bytes)
                return Fail(Stage::VerifySize, FailureReason::ChangedFile);
            if (Io::Sync(temporary.fd)) return Fail(Stage::Sync, FailureReason::SyscallError, errno);
            const auto closed = Io::Close(temporary.fd); temporary.fd = -1;
            if (closed) return Fail(Stage::Close, FailureReason::SyscallError, errno);
            struct stat currentTemporary{}, currentTarget{}; bool targetExists{};
            if (::fstatat(folder.value, "resource-window.tmp", &currentTemporary, AT_SYMLINK_NOFOLLOW))
                return Fail(Stage::RecheckTemporary, FailureReason::SyscallError, errno);
            if (!OwnedRegular(currentTemporary)) return Fail(Stage::RecheckTemporary, FailureReason::PolicyRejected);
            if (!SameFile(currentTemporary, temporary.identity)) return Fail(Stage::RecheckTemporary, FailureReason::ChangedFile);
            if (!Target(folder.value, currentTarget, targetExists, Stage::RecheckTarget)) return false;
            if (targetExists != priorExists || (priorExists && !SameFile(prior, currentTarget))) return Fail(Stage::RecheckTarget, FailureReason::ChangedFile);
            activeStage = Stage::Rename;
            if (Io::Rename(folder.value, "resource-window.tmp", "resource-window.jsonl")) return Fail(Stage::Rename, FailureReason::SyscallError, errno);
            temporary.committed = true; lastFailure = {}; return true;
        } catch (const std::bad_alloc &) { return Fail(activeStage, FailureReason::AllocationFailure); }
        catch (...) { return Fail(Stage::Exception, FailureReason::UnknownException); }
    }
};
using RecentResourceWindow = BasicRecentResourceWindow<>;
}
