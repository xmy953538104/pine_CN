// SPDX-License-Identifier: MPL-2.0
#include "diagnostics.h"
#include "admission.h"
#include "bounded_queue.h"
#include "read_snapshot.h"
#include "test_hooks.h"
#include "legacy_error.h"
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include "identity_observer.h"
#include <thread>
#include <vector>
#include <pthread.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace skyline::diagnostics {
namespace {
using Clock = std::chrono::steady_clock;
constexpr uint64_t ProducersClosed = uint64_t{1} << 63;
constexpr uint64_t ProducerCountMask = (uint64_t{1} << 32) - 1;
constexpr uint64_t ProducerGenerationMask = ~(ProducerCountMask | ProducersClosed);
struct Record {
    uint64_t sequence{}, wall{}, mono{}, dropped{}, contention{}, full{}, allocation{}, disk{}, criticalDropped{};
    bool critical{};
    std::string category, event, level, thread, stage, guestStage, titleId, applicationSource, programId, cnmtId, programContentId, launchId, effectiveProgramContentId, effectiveProgramId, effectiveSource;
    std::vector<std::pair<std::string, std::string>> fields;
};
struct Context {
    std::string stage, guestStage, titleId, applicationSource, programId, cnmtId, programContentId, launchId;
    std::string effectiveProgramContentId, effectiveProgramId, effectiveSource;
};
struct Sink {
    // The closed bit and active count share one atomic so Stop cannot miss an
    // emitter entering between the enabled check and its counter increment.
    std::atomic<uint64_t> producers{ProducersClosed};
    std::atomic<bool> verbose{};
    std::atomic<uint64_t> accepted{}, dropped{}, written{}, diskErrors{};
    std::atomic<uint64_t> completedTicket{};
    uint32_t producerGeneration{}; // lifecycle mutex; prevents Start/Stop ABA for late registration
    LegacyErrorBudget audioErrors, shaderErrors, guestProtocol;
    std::atomic<uint64_t> guestProtocolSuppressed{};
    std::atomic<uint64_t> legacySuppressed{};
    std::atomic<uint64_t> droppedContention{}, droppedFull{}, droppedAllocation{}, droppedDisk{}, criticalDropped{}, snapshotErrors{}, snapshotEvictions{};
    std::mutex lifecycle, mutex, progressMutex, firstObservationMutex;
    std::condition_variable wake, progress;
    BoundedQueue<Record> queue;
    ReadSnapshot<Context> context;
    Config config;
    std::string stage;
    std::string guestStage{"unknown"};
    // Game identity outlives a logging session; only true game boundaries clear it.
    std::string activeTitleId, activeApplicationSource;
    std::string activeProgramId, activeCnmtId, activeProgramContentId, activeLaunchId;
    std::string effectiveProgramContentId, effectiveProgramId, effectiveSource;
    std::atomic<uint64_t> presentReturns{}, presentAccepted{}, presentOther{}, firstPresentUs{}, launchStartedUs{};
    std::atomic<int32_t> lastPresentResult{};
    std::atomic<bool> stopRequested{};
    std::atomic<uint64_t> observationEpoch{};
    uint64_t firstRecordedEpoch{}; // protected by firstObservationMutex, ordinary observers only
    std::atomic<bool> stopping{};
    std::thread worker;
    ~Sink();
};
Sink &Get() { static Sink sink; return sink; }
// Caller owns the ordinary metadata mutex; producers use the published snapshot.
void PublishContext(Sink &s) {
    s.context.Publish({s.stage, s.guestStage, s.activeTitleId, s.activeApplicationSource,
        s.activeProgramId, s.activeCnmtId, s.activeProgramContentId, s.activeLaunchId,
        s.effectiveProgramContentId, s.effectiveProgramId, s.effectiveSource});
}
void CopyContext(Record &r, Context &&c) {
    r.stage = std::move(c.stage); r.guestStage = std::move(c.guestStage); r.titleId = std::move(c.titleId); r.applicationSource = std::move(c.applicationSource);
    r.programId = std::move(c.programId); r.cnmtId = std::move(c.cnmtId); r.programContentId = std::move(c.programContentId); r.launchId = std::move(c.launchId);
    r.effectiveProgramContentId = std::move(c.effectiveProgramContentId); r.effectiveProgramId = std::move(c.effectiveProgramId); r.effectiveSource = std::move(c.effectiveSource);
}
bool IsEnabled(const Sink &s) { return !(s.producers.load(std::memory_order_relaxed) & ProducersClosed); }
class Producer {
    Sink *sink{};
public:
    explicit Producer(Sink &s) {
        auto state = s.producers.load(std::memory_order_relaxed);
        const auto generation = state & ProducerGenerationMask;
#ifdef PINE_DIAG_TEST_HOOKS
        testing::Notify(testing::Point::ProducerStateRead);
#endif
        // Lock-free registration owns every counted admission/drop. Unlike the
        // bounded queue retry, failed registration never mutates session stats.
        while (!(state & ProducersClosed) && (state & ProducerGenerationMask) == generation && (state & ProducerCountMask) != ProducerCountMask) {
            if (s.producers.compare_exchange_weak(state, state + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
                sink = &s;
                break;
            }
        }
    }
    ~Producer() {
        if (sink) {
            const auto previous = sink->producers.fetch_sub(1, std::memory_order_release);
            if ((previous & ProducersClosed) && (previous & ProducerCountMask) == 1) sink->producers.notify_all();
        }
    }
    explicit operator bool() const { return sink != nullptr; }
};
void CloseProducers(Sink &s) {
    auto state = s.producers.fetch_or(ProducersClosed, std::memory_order_acq_rel) | ProducersClosed;
    while (state & ProducerCountMask) {
        s.producers.wait(state, std::memory_order_acquire);
        state = s.producers.load(std::memory_order_acquire);
    }
}
std::string Quote(std::string_view text) {
    std::string result{"\""};
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { result += '\\'; result += c; }
        else if (c < 32) { char hex[7]; std::snprintf(hex, sizeof(hex), "\\u%04x", c); result += hex; }
        else result += c;
    }
    return result + '"';
}
std::string Serialize(const Record &r, const Config &c, bool snapshot = false,
                      std::string_view observedEvent = {}, std::string_view observedLevel = {}, uint64_t evictions = 0) {
    std::string out = "{\"schema\":1,\"session_id\":" + Quote(c.sessionId) + ",\"sequence\":" + std::to_string(r.sequence)
        + ",\"wall_clock_ms\":" + std::to_string(r.wall) + ",\"monotonic_us\":" + std::to_string(r.mono)
        + ",\"dropped_count\":" + std::to_string(r.dropped)
        + ",\"drop_counts\":{\"contention\":" + std::to_string(r.contention) + ",\"queue_full\":" + std::to_string(r.full)
        + ",\"allocation\":" + std::to_string(r.allocation) + ",\"disk_failure\":" + std::to_string(r.disk)
        + ",\"critical\":" + std::to_string(r.criticalDropped) + "}"
        + ",\"record_kind\":" + Quote(snapshot ? "critical_snapshot" : "event") + ",\"thread\":" + Quote(r.thread)
        + ",\"category\":" + Quote(r.category) + ",\"event\":" + Quote(r.event) + ",\"level\":" + Quote(r.level)
        + ",\"last_stage\":" + Quote(r.stage) + ",\"last_load_stage\":" + Quote(r.stage)
        + ",\"last_guest_stage\":" + Quote(r.guestStage) + ",\"title_id\":" + Quote(r.titleId)
        + ",\"application_title_id\":" + Quote(r.titleId) + ",\"application_identity_source\":" + Quote(r.applicationSource)
        + ",\"program_id\":" + Quote(r.programId)
        + ",\"cnmt_title_id\":" + Quote(r.cnmtId) + ",\"program_nca_id\":" + Quote(r.programContentId)
        + ",\"program_nca_id_scope\":\"legacy_base_program_container\",\"base_program_nca\":" + Quote(r.programContentId)
        + ",\"effective_exefs_program_nca\":" + Quote(r.effectiveProgramContentId)
        + ",\"effective_exefs_program_id\":" + Quote(r.effectiveProgramId)
        + ",\"effective_exefs_source\":" + Quote(r.effectiveSource)
        + ",\"launch_id\":" + Quote(r.launchId) + ",\"process_run_id\":" + Quote(c.processRunId)
        + (snapshot ? ",\"snapshot_metadata\":{\"observed_event\":" + Quote(observedEvent) + ",\"observed_level\":" + Quote(observedLevel)
            + ",\"fields_omitted\":0,\"evictions\":" + std::to_string(evictions) + ",\"scope\":\"accepted_critical_last_state_not_replay\"}" : "")
        + ",\"fields\":{";
    bool first = true;
    for (const auto &[key, value] : r.fields) { if (!first) out += ','; first = false; out += Quote(key) + ':' + Quote(value); }
    return out + "}}\n";
}
void CopyDropCounts(Record &r, const Sink &s) {
    r.dropped = s.dropped; r.contention = s.droppedContention; r.full = s.droppedFull;
    r.allocation = s.droppedAllocation; r.disk = s.droppedDisk; r.criticalDropped = s.criticalDropped;
}
// Writer-owned, small last-observed-state cache. It is never an event replay or a lossless journal.
class CriticalSnapshot {
    Sink &sink;
    const Config &config;
    std::deque<std::pair<std::string, Record>> records;
    bool pending{}, failed{};
    Clock::time_point nextWrite{};
public:
    CriticalSnapshot(Sink &sink, const Config &config) : sink(sink), config(config) {}
    void Observe(const Record &record) noexcept {
        if (!record.critical || failed) return;
        try {
        std::string key = record.launchId + ':' + record.event;
        for (const auto &[name, value] : record.fields)
            if (name == "exit_record_id" || name == "title_id" || name == "loader_source" || name == "loader_index" || name == "nca_type") key += ":" + name + "=" + value;
        auto found = std::find_if(records.begin(), records.end(), [&](const auto &item) { return item.first == key; });
        if (found != records.end()) records.erase(found);
        auto evict = [&] {
            // Keep up to eight most recent accepted launch endings even when
            // other critical state changes within the same logging session.
            auto victim = std::find_if(records.begin(), records.end(), [](const auto &item) { return item.second.event != "LAUNCH_ENDED"; });
            records.erase(victim == records.end() ? records.begin() : victim); ++sink.snapshotEvictions;
        };
        if (record.event == "LAUNCH_ENDED" && std::count_if(records.begin(), records.end(), [](const auto &item) { return item.second.event == "LAUNCH_ENDED"; }) >= 8) {
            records.erase(std::find_if(records.begin(), records.end(), [](const auto &item) { return item.second.event == "LAUNCH_ENDED"; })); ++sink.snapshotEvictions;
        }
        if (records.size() == 32) evict();
        records.emplace_back(std::move(key), record); pending = true;
        } catch (...) { ++sink.snapshotErrors; failed = true; pending = false; }
    }
    bool Pending() const { return pending; }
    Clock::time_point Deadline() const { return nextWrite; }
    void Persist(bool force = false) noexcept {
        if (!pending || failed || (!force && Clock::now() < nextWrite)) return;
        int fd = -1;
        try {
            std::string text;
            for (;;) {
                text.clear();
                for (const auto &[key, original] : records) {
                    auto record = original;
                    // Producer fields are already bounded. Keep all of them, including
                    // late trace frames/Build IDs; snapshot metadata has its own root object.
                    record.fields.emplace_back("observed_event", original.event); // schema-1 compatibility
                    record.event = "CRITICAL_STATE_SNAPSHOT"; record.level = "INFO";
                    text += Serialize(record, config, true, original.event, original.level, sink.snapshotEvictions.load());
                }
                if (text.size() <= 65536) break;
                auto victim = std::find_if(records.begin(), records.end(), [](const auto &item) { return item.second.event != "LAUNCH_ENDED"; });
                records.erase(victim == records.end() ? records.begin() : victim); ++sink.snapshotEvictions;
            }
            const auto directory = std::filesystem::path(config.directory);
            const auto temporary = directory / "critical-state.tmp";
            fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
            struct stat status{};
            if (fd < 0 || fstat(fd, &status) || !S_ISREG(status.st_mode)) throw std::runtime_error("snapshot open failed");
            size_t offset{};
            while (offset < text.size()) {
                const auto count = write(fd, text.data() + offset, text.size() - offset);
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) throw std::runtime_error("snapshot write failed");
                offset += static_cast<size_t>(count);
            }
            const auto result = close(fd); fd = -1;
            if (result) throw std::runtime_error("snapshot close failed");
            std::filesystem::rename(temporary, directory / "critical-state.jsonl");
            pending = false; nextWrite = Clock::now() + std::chrono::milliseconds(500);
        } catch (...) {
            if (fd >= 0) close(fd);
            ++sink.snapshotErrors; failed = true; pending = false;
        }
    }
};
uint64_t MonotonicUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}
bool Enqueue(Sink &, std::string_view, std::string_view,
    std::span<const std::pair<std::string_view, std::string_view>>, std::string_view,
    const std::string_view *, std::string_view) noexcept;
bool EmitProgress(Sink &s, std::string_view event, std::string_view expectedLaunch = {}) {
    const auto epoch = s.observationEpoch.load();
    if (epoch & 1) return false;
    Context context;
    if (!s.context.Read(context) || context.launchId.empty() || (!expectedLaunch.empty() && context.launchId != expectedLaunch)) return false;
    const auto accepted = s.presentAccepted.load(std::memory_order_relaxed);
    const auto other = s.presentOther.load(std::memory_order_relaxed);
    const auto returns = s.presentReturns.load(std::memory_order_relaxed);
    const auto started = s.launchStartedUs.load();
    const std::pair<std::string, std::string> values[]{
        {"source", "vulkan_queue_present_khr_return"},
        {"observation_layer", "host_vulkan_api_return"}, {"counter_scope", "diagnostics_enabled_observations_per_launch"},
        {"present_returns", std::to_string(returns)}, {"present_accepted", std::to_string(accepted)},
        {"present_other_results", std::to_string(other)}, {"last_present_result", std::to_string(s.lastPresentResult.load())},
        {"first_accepted_present_monotonic_us", std::to_string(s.firstPresentUs.load())},
        {"launch_elapsed_us", std::to_string(started ? MonotonicUs() - started : 0)},
        {"visibility", "not_observed"}, {"guest_execution_phase", "not_inferred"}};
    std::array<std::pair<std::string_view, std::string_view>, std::size(values)> fields;
    for (size_t i{}; i < fields.size(); ++i) fields[i] = values[i];
    if (s.observationEpoch.load() != epoch) return false;
    return Enqueue(s, "lifecycle", event, fields, "INFO", nullptr, context.launchId);
}
void EmitFirstPresent(Sink &s, std::string_view expectedLaunch = {}) {
    std::lock_guard lock(s.firstObservationMutex);
    const auto epoch = s.observationEpoch.load();
    if (!(epoch & 1) && s.firstRecordedEpoch != epoch && s.firstPresentUs.load() &&
        EmitProgress(s, "FIRST_PRESENT_OBSERVED", expectedLaunch)) s.firstRecordedEpoch = epoch;
}
// Only fixed names are rotated. Preserve completed prior session state instead
// of overwriting it on the next application/session start. No directory crawl.
void RotateCriticalState(const Config &c) {
    const auto path = [&](size_t i) { return std::filesystem::path(c.directory) / (i ? "critical-state-" + std::to_string(i) + ".jsonl" : "critical-state.jsonl"); };
    const auto keep = std::min<size_t>(8, c.maxFiles);
    for (size_t i = keep; i < 8; ++i) std::filesystem::remove(path(i));
    std::filesystem::remove(path(keep - 1));
    for (size_t i = keep - 1; i > 0; --i) {
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(path(i - 1), ec);
        if (ec == std::errc::no_such_file_or_directory) continue;
        if (ec) throw std::runtime_error("state retention failed");
        if (std::filesystem::exists(status)) std::filesystem::rename(path(i - 1), path(i));
    }
}
void Run(Sink &s) noexcept {
    bool pendingRecord{}, pendingCritical{};
    try {
        const Config c = s.config;
        CriticalSnapshot snapshot(s, c);
        auto nextProgress = Clock::now() + std::chrono::seconds(5);
        auto path = [&](size_t i) { return std::filesystem::path(c.directory) / ("diagnostics-" + std::to_string(i) + ".jsonl"); };
        std::ofstream file;
        size_t bytes{};
        auto rotate = [&] {
            if (file.is_open()) {
                file.close();
                if (!file) throw std::runtime_error("diagnostic close failed");
            }
            std::error_code ec;
            std::filesystem::remove(path(c.maxFiles - 1), ec);
            if (ec) throw std::runtime_error("diagnostic retention failed");
            for (size_t i = c.maxFiles - 1; i > 0; --i) {
                ec.clear();
                // symlink_status also detects dangling links: never follow an
                // old fixed-name link when opening the next output file.
                auto status = std::filesystem::symlink_status(path(i - 1), ec);
                if (ec == std::errc::no_such_file_or_directory) ec.clear();
                if (!ec && std::filesystem::exists(status)) std::filesystem::rename(path(i - 1), path(i), ec);
                if (ec) throw std::runtime_error("diagnostic rotation failed");
            }
            file.open(path(0), std::ios::trunc); bytes = 0;
            if (!file) throw std::runtime_error("diagnostic open failed");
        };
        std::filesystem::create_directories(c.directory);
        try { RotateCriticalState(c); } catch (...) { ++s.snapshotErrors; }
        // Prune only our fixed allowlisted filenames when retention is reduced.
        for (size_t i = c.maxFiles; i < 16; ++i) {
            std::error_code ec;
            std::filesystem::remove(path(i), ec);
            if (ec) throw std::runtime_error("diagnostic retention failed");
        }
        rotate();
        while (true) {
            if (!s.stopping && IsEnabled(s)) {
                Context context;
                if (s.context.Read(context) && !context.launchId.empty()) {
                    if (s.firstPresentUs.load(std::memory_order_relaxed)) EmitFirstPresent(s, context.launchId);
                    if (Clock::now() >= nextProgress) EmitProgress(s, "LAUNCH_PROGRESS", context.launchId);
                }
                if (Clock::now() >= nextProgress) nextProgress = Clock::now() + std::chrono::seconds(5);
            }
            Record record;
            {
                std::unique_lock lock(s.mutex);
                s.wake.wait_for(lock, std::chrono::milliseconds(50), [&] { return s.stopping || !s.queue.Empty(); });
                uint64_t sequence{};
                if (!s.queue.Pop(record, sequence)) {
                    if (s.stopping && s.queue.Empty()) break;
                    lock.unlock(); snapshot.Persist(); continue;
                }
                record.sequence = sequence;
                if (record.category == "guest" && record.launchId == s.activeLaunchId) {
                    if (record.event == "DFX_MAIN_ENTER" || record.event == "DFX_MODINDEX_READY" || record.event == "DFX_MODMAIN_OK" || record.event == "DFX_HAMLET_ENTER") s.guestStage = record.event;
                    else if (s.guestStage == "unknown" && (record.event == "GUEST_DEBUG_MESSAGE" || record.event == "GUEST_RUNTIME_ERROR")) s.guestStage = "guest_message_observed";
                    if (record.guestStage != s.guestStage) PublishContext(s);
                    record.guestStage = s.guestStage;
                }
                pendingRecord = true; pendingCritical = record.critical;
            }
            auto line = Serialize(record, c);
            snapshot.Observe(record);
#ifdef __ANDROID__
            // Android log entries have a small payload limit: never emit broken JSON.
            const auto priority = record.level == "ERROR" ? ANDROID_LOG_ERROR : record.level == "WARN" ? ANDROID_LOG_WARN : record.level == "DEBUG" ? ANDROID_LOG_DEBUG : ANDROID_LOG_INFO;
            if (line.size() <= 3800) __android_log_write(priority, "PineDiag", line.c_str());
            else {
                record.fields = {{"detail", "oversized_event_in_jsonl"}};
                auto brief = Serialize(record, c);
                __android_log_write(priority, "PineDiag", brief.c_str());
            }
#endif
            if (bytes && bytes + line.size() > c.maxFileBytes) rotate();
#ifdef PINE_DIAG_TEST_HOOKS
            testing::Notify(testing::Point::BeforeRecordWrite, record.event);
#endif
            file.write(line.data(), line.size()); file.flush();
            if (!file) throw std::runtime_error("diagnostic write failed");
            bytes += line.size();
            snapshot.Persist();
            { std::lock_guard lock(s.progressMutex); ++s.written; s.completedTicket = record.sequence; }
            s.progress.notify_all(); pendingRecord = false;
        }
        Record summary;
        summary.sequence = s.accepted.load() + 1;
        summary.wall = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        summary.mono = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
        CopyDropCounts(summary, s);
        summary.category = "lifecycle"; summary.event = "SESSION_SUMMARY"; summary.level = "INFO"; summary.thread = "diagnostic-writer";
        { std::lock_guard lock(s.mutex); summary.stage = s.stage; summary.guestStage = s.guestStage; summary.titleId = s.activeTitleId; summary.applicationSource = s.activeApplicationSource;
          summary.programId = s.activeProgramId; summary.cnmtId = s.activeCnmtId;
          summary.programContentId = s.activeProgramContentId; summary.launchId = s.activeLaunchId;
          summary.effectiveProgramContentId = s.effectiveProgramContentId; summary.effectiveProgramId = s.effectiveProgramId; summary.effectiveSource = s.effectiveSource; }
        summary.fields = {{"accepted", std::to_string(s.accepted.load())}, {"written", std::to_string(s.written.load())},
            {"disk_errors", std::to_string(s.diskErrors.load())}, {"snapshot_errors", std::to_string(s.snapshotErrors.load())},
            {"snapshot_evictions", std::to_string(s.snapshotEvictions.load())}, {"legacy_error_rate_suppressed", std::to_string(s.legacySuppressed.load())},
            {"guest_protocol_rate_suppressed", std::to_string(s.guestProtocolSuppressed.load())},
            {"admission_policy", "bounded_mpsc_64_cas_and_64_context_attempts_no_mutex_wait"}};
        auto line = Serialize(summary, c);
        if (bytes && bytes + line.size() > c.maxFileBytes) rotate();
        file.write(line.data(), line.size()); file.flush();
        if (!file) throw std::runtime_error("diagnostic summary write failed");
#ifdef __ANDROID__
        __android_log_write(ANDROID_LOG_INFO, "PineDiag", line.c_str());
#endif
        summary.critical = true; snapshot.Observe(summary); snapshot.Persist(true);
    } catch (...) {
        s.producers.fetch_or(ProducersClosed, std::memory_order_acq_rel);
        { std::lock_guard lock(s.progressMutex); ++s.diskErrors; }
        s.progress.notify_all();
        CloseProducers(s);
        uint64_t sequence{}; Record lost;
        if (pendingRecord) { ++s.dropped; ++s.droppedDisk; if (pendingCritical) ++s.criticalDropped; }
        while (s.queue.Pop(lost, sequence)) {
            ++s.dropped; ++s.droppedDisk; if (lost.critical) ++s.criticalDropped;
        }
    }
}
void StopLocked(Sink &s) {
    // Emit is bounded and never waits for the writer. Only this ordinary-thread
    // lifecycle path waits for active emitters, keeping final counters stable.
    CloseProducers(s);
    { std::lock_guard lock(s.mutex); s.stopping = true; }
    s.wake.notify_one();
    if (s.worker.joinable()) s.worker.join();
}
Sink::~Sink() { StopLocked(*this); }
bool CategorySelected(std::string_view selected, std::string_view category) {
    if (selected.empty() || category == "lifecycle") return true;
    while (true) {
        auto end = selected.find(',');
        if (selected.substr(0, end) == category) return true;
        if (end == std::string_view::npos) return false;
        selected.remove_prefix(end + 1);
    }
}
bool Enqueue(Sink &s, std::string_view category, std::string_view event,
             std::span<const std::pair<std::string_view, std::string_view>> fields,
             std::string_view level, const std::string_view *stage = nullptr, std::string_view expectedLaunch = {}) noexcept {
    const bool critical = CriticalEvent(category, event, level);
    auto drop = [&](std::atomic<uint64_t> &reason) { ++s.dropped; ++reason; if (critical) ++s.criticalDropped; };
    Producer producer(s);
    if (!producer) return false;
    try {
        // Field sanitation and host thread formatting do not hold the admission lock.
        Record r; r.critical = critical;
        (void)stage;
        if (!CategorySelected(s.config.categories, category)) return false;
        Context context;
        if (!s.context.Read(context)) { drop(s.droppedContention); return false; }
        if (!expectedLaunch.empty() && context.launchId != expectedLaunch) return false;
        CopyContext(r, std::move(context));
        r.category = Sanitize(category, 64); r.event = Sanitize(event, 96); r.level = Sanitize(level, 16);
        char name[16]{}; pthread_getname_np(pthread_self(), name, sizeof(name));
        std::ostringstream thread; thread << name << ':' << std::this_thread::get_id(); r.thread = Sanitize(thread.str(), 128);
        r.wall = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        r.mono = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
        size_t budget = 4096;
        for (auto &[key, value] : fields) { if (r.fields.size() == 24 || !budget) break; auto copy = Sanitize(value, std::min<size_t>(2048, budget)); budget -= std::min(budget, copy.size()); r.fields.emplace_back(Sanitize(key, 64), std::move(copy)); }
        if (!IsEnabled(s) || s.stopping) return false;
        CopyDropCounts(r, s);
        const auto result = s.queue.Push(std::move(r), AdmissionLimit(s.config.queueCapacity, critical));
        if (result == Admission::Full) { drop(s.droppedFull); return false; }
        if (result == Admission::Contended) { drop(s.droppedContention); return false; }
#ifdef PINE_DIAG_TEST_HOOKS
        testing::Notify(testing::Point::PublishedBeforeCount, event);
#endif
        ++s.accepted; s.wake.notify_one(); return true;
    } catch (...) { drop(s.droppedAllocation); return false; }
}
}
std::string Sanitize(std::string_view input, size_t limit) {
    // Scan only the bounded input. Spaces/quotes are legal filename bytes, so a
    // recognized location consumes its line tail, preserving context and line endings.
    const auto text = input.substr(0, limit);
    auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    auto alpha = [&](unsigned char c) { return lower(c) >= 'a' && lower(c) <= 'z'; };
    auto digit = [](unsigned char c) { return c >= '0' && c <= '9'; };
    auto whitespace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    auto schemeChar = [&](unsigned char c) { return alpha(c) || digit(c) || c == '+' || c == '.' || c == '-'; };
    auto pathChar = [&](unsigned char c) { return alpha(c) || digit(c) || c == '_' || c == '.' || c == '/' || c == '\\' || c == '-'; };
    auto matches = [&](size_t pos, std::string_view prefix) {
        if (prefix.size() > text.size() - pos) return false;
        for (size_t i{}; i < prefix.size(); ++i)
            if (lower(text[pos + i]) != lower(prefix[i])) return false;
        return true;
    };
    auto guestMount = [&](size_t pos, size_t lineStart) {
        for (std::string_view mount : {"rom:", "romfs:", "sdmc:"}) {
            if (pos - lineStart < mount.size()) continue;
            const auto start = pos - mount.size();
            if (matches(start, mount) && (start == lineStart ||
                (!schemeChar(text[start - 1]) && text[start - 1] != '_'))) return true;
        }
        return false;
    };
    auto unixBoundary = [&](size_t pos, size_t lineStart) {
        if (pos == lineStart) return true;
        const auto previous = static_cast<unsigned char>(text[pos - 1]);
        if (previous < 0x80) return !pathChar(previous);
        // Keep UTF-8 relative guest names, while accepting common Unicode log
        // punctuation/spacing such as the full-width colon immediately before a path.
        size_t first = pos - 1;
        while (first > lineStart && (static_cast<unsigned char>(text[first]) & 0xc0) == 0x80) --first;
        const auto lead = static_cast<unsigned char>(text[first]);
        const size_t count = lead >= 0xc2 && lead <= 0xdf ? 2 : lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
        if (!count || pos - first != count) return true;
        uint32_t code = lead & (0x7f >> count);
        for (size_t i = first + 1; i < pos; ++i) code = (code << 6) | (static_cast<unsigned char>(text[i]) & 0x3f);
        if ((count == 3 && code < 0x800) || (count == 4 && code < 0x10000) ||
            (code >= 0xd800 && code <= 0xdfff) || code > 0x10ffff) return true;
        return code == 0xa0 || code == 0x1680 || (code >= 0x2000 && code <= 0x206f) ||
            (code >= 0x3000 && code <= 0x303f) || (code >= 0xff01 && code <= 0xff0f) ||
            (code >= 0xff1a && code <= 0xff20) || (code >= 0xff3b && code <= 0xff40) ||
            (code >= 0xff5b && code <= 0xff65);
    };
    std::string out;
    out.reserve(text.size());
    size_t schemeEnd{}, lineStart{};
    // Every recognized location contains a slash or backslash. Numeric IDs,
    // enum values and field names can skip location scanning, but still repair UTF-8.
    if (text.find_first_of("/\\") == std::string_view::npos) {
        out.assign(text);
    } else {
        for (size_t pos{}; pos < text.size();) {
            if (text[pos] == '\r' || text[pos] == '\n') {
                out += text[pos++]; lineStart = pos; continue;
            }
            bool location{};
            if (alpha(text[pos])) {
                // Cache each ASCII scheme run's end; a long plain word must not
                // trigger a fresh suffix scan at every byte.
                if (pos >= schemeEnd) {
                    schemeEnd = pos;
                    while (schemeEnd < text.size() && schemeChar(text[schemeEnd])) ++schemeEnd;
                }
                location = matches(schemeEnd, "://");
                for (std::string_view prefix : {"file:", "content:"}) {
                    if (!matches(pos, prefix)) continue;
                    const auto next = pos + prefix.size();
                    location = location || (next < text.size() && text[next] == '/') ||
                        (next + 2 < text.size() && alpha(text[next]) && text[next + 1] == ':' &&
                         (text[next + 2] == '/' || text[next + 2] == '\\'));
                }
            }
            const bool windowsBoundary = pos == lineStart || !pathChar(text[pos - 1]);
            location = location || (windowsBoundary && pos + 2 < text.size() &&
                ((alpha(text[pos]) && text[pos + 1] == ':' && (text[pos + 2] == '/' || text[pos + 2] == '\\')) ||
                 (text[pos] == '\\' && text[pos + 1] == '\\' && !whitespace(text[pos + 2]) && text[pos + 2] != '\\')));
            if (!location && text[pos] == '/' && unixBoundary(pos, lineStart) && !guestMount(pos, lineStart)) {
                for (std::string_view prefix : {"/storage", "/sdcard", "/data", "/mnt", "/cache", "/home", "/Users"}) {
                    if (!matches(pos, prefix)) continue;
                    const auto next = pos + prefix.size();
                    location = next == text.size() || std::string_view("/\\ \t\r\n\"'<>").find(text[next]) != std::string_view::npos;
                    if (location) break;
                }
            }
            if (location) {
                out += "[REDACTED]";
                while (pos < text.size() && text[pos] != '\r' && text[pos] != '\n') ++pos;
            } else out += text[pos++];
        }
    }
    // Repair invalid/truncated UTF-8, rather than writing invalid JSON strings.
    std::string clean;
    for (size_t i{}; i < out.size();) {
        unsigned char b = out[i]; size_t n = b < 0x80 ? 1 : b >= 0xc2 && b <= 0xdf ? 2 : b >= 0xe0 && b <= 0xef ? 3 : b >= 0xf0 && b <= 0xf4 ? 4 : 0;
        bool valid = n && i + n <= out.size();
        for (size_t j = 1; valid && j < n; ++j) valid = (static_cast<unsigned char>(out[i + j]) & 0xc0) == 0x80;
        if (valid && n >= 3) { auto next = static_cast<unsigned char>(out[i + 1]); valid = !(b == 0xe0 && next < 0xa0) && !(b == 0xed && next >= 0xa0) && !(b == 0xf0 && next < 0x90) && !(b == 0xf4 && next >= 0x90); }
        if (valid) { clean.append(out, i, n); i += n; } else { clean += "\xef\xbf\xbd"; ++i; }
    }
    return clean;
}
bool Enabled() noexcept { return IsEnabled(Get()); }
bool Verbose() noexcept { return Enabled() && Get().verbose.load(std::memory_order_relaxed); }
bool Start(const Config &config) noexcept {
    auto &s = Get();
    try {
        std::lock_guard life(s.lifecycle); StopLocked(s);
        if (config.directory.empty() || config.sessionId.empty() || !config.queueCapacity || config.queueCapacity > 65536 || config.maxFiles < 1 || config.maxFiles > 16 || config.maxFileBytes < 65536) return false;
        try {
            { std::lock_guard lock(s.mutex); s.config = config; s.config.sessionId = Sanitize(config.sessionId, 128); s.config.processRunId = Sanitize(config.processRunId, 128); s.queue.Reset(config.queueCapacity); s.stage.clear(); s.stopping = false; PublishContext(s); }
            s.accepted = 0; s.dropped = 0; s.written = 0; s.completedTicket = 0; s.diskErrors = 0;
            s.droppedContention = 0; s.droppedFull = 0; s.droppedAllocation = 0; s.droppedDisk = 0;
            s.criticalDropped = 0; s.snapshotErrors = 0; s.snapshotEvictions = 0;
            s.audioErrors.Reset(); s.shaderErrors.Reset(); s.guestProtocol.Reset(); s.guestProtocolSuppressed = 0; s.legacySuppressed = 0; s.verbose = config.verbose;
            if (s.producerGeneration == 0x7fffffff) return false;
            s.producers.store(uint64_t{++s.producerGeneration} << 32, std::memory_order_release);
            s.worker = std::thread([&s] { Run(s); });
            return true;
        } catch (...) {
            // Keep the lifecycle lock while undoing a failed start so it cannot
            // disable or clear a different caller's newly started session.
            CloseProducers(s);
            Record lost; uint64_t sequence{};
            while (s.queue.Pop(lost, sequence)) { ++s.dropped; if (lost.critical) ++s.criticalDropped; }
            ++s.diskErrors; return false;
        }
    } catch (...) { return false; }
}
void Stop() noexcept { try { auto &s = Get(); std::lock_guard life(s.lifecycle); StopLocked(s); } catch (...) {} }
bool Flush(uint32_t timeoutMilliseconds) noexcept {
    try {
        auto &s = Get();
        std::unique_lock life(s.lifecycle, std::try_to_lock);
        if (!life.owns_lock()) return false;
        // A count can lag publication on one producer while another has already
        // returned. Reservation tickets follow actual ring/flush order instead.
        const auto target = s.queue.ReservedTicket();
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
        std::unique_lock lock(s.progressMutex);
        s.progress.wait_until(lock, deadline, [&] { return s.completedTicket >= target || s.diskErrors; });
        return !s.diskErrors && s.completedTicket >= target;
    } catch (...) { return false; }
}
void Emit(std::string_view category, std::string_view event, std::initializer_list<std::pair<std::string_view, std::string_view>> fields, std::string_view level) noexcept {
    auto &s = Get(); if (!IsEnabled(s)) return;
    Enqueue(s, category, event, {fields.begin(), fields.size()}, level);
}
bool EmitFields(std::string_view category, std::string_view event,
                std::span<const std::pair<std::string_view, std::string_view>> fields, std::string_view level) noexcept {
    auto &s = Get(); if (!IsEnabled(s)) return false;
    return Enqueue(s, category, event, fields, level);
}
void EmitLegacyError(std::string_view component, std::string_view message) noexcept {
    auto &s = Get(); if (!IsEnabled(s)) return;
    Producer producer(s); if (!producer) return;
    const auto category = component == "audio_core" ? "audio" : component == "shader_compiler" ? "gpu" : "";
    if (std::string_view(category).empty() || !CategorySelected(s.config.categories, category)) return;
    const auto second = std::chrono::duration_cast<std::chrono::seconds>(Clock::now().time_since_epoch()).count();
    uint64_t suppressed{};
    auto &budget = component == "audio_core" ? s.audioErrors : s.shaderErrors;
    if (!budget.Take(static_cast<uint64_t>(second), suppressed)) { ++s.legacySuppressed; return; }
    try {
        Emit(category, component == "audio_core" ? "AUDIO_BACKEND_ERROR" : "SHADER_COMPILER_ERROR",
            {{"component", component}, {"message", message}, {"fatality", "unknown"},
             {"rate_policy", "first_4_per_component_per_second"}, {"prior_rate_suppressed", std::to_string(suppressed)}}, "ERROR");
    } catch (...) {}
}
bool AllowGuestProtocol(uint64_t &priorSuppressed) noexcept {
    auto &s = Get(); if (!IsEnabled(s)) return false;
    Producer producer(s); if (!producer) return false;
    const auto second = std::chrono::duration_cast<std::chrono::seconds>(Clock::now().time_since_epoch()).count();
    if (s.guestProtocol.Take(static_cast<uint64_t>(second), priorSuppressed, 16)) return true;
    ++s.guestProtocolSuppressed; return false;
}
void SetTitleId(std::string_view titleId) noexcept {
    try {
        auto copy = Sanitize(titleId, 32);
        auto &s = Get();
        std::lock_guard lock(s.mutex);
        s.activeTitleId = std::move(copy); s.activeApplicationSource = s.activeTitleId.empty() ? "" : "ui_metadata_hint_not_runtime_verified"; PublishContext(s);
    } catch (...) {}
}
void ClearTitleId() noexcept {
    try {
        auto &s = Get();
        std::lock_guard lock(s.mutex);
        s.activeTitleId.clear(); s.activeApplicationSource.clear();
        s.activeProgramId.clear(); s.activeCnmtId.clear(); s.activeProgramContentId.clear(); s.guestStage = "unknown";
        s.effectiveProgramContentId.clear(); s.effectiveProgramId.clear(); s.effectiveSource.clear(); PublishContext(s);
    } catch (...) {}
}
void SetContentIdentity(std::string_view applicationId, std::string_view programId,
                        std::string_view cnmtId, std::string_view programContentId, std::string_view applicationSource) noexcept {
    try {
        auto app = Sanitize(applicationId, 32), program = Sanitize(programId, 32);
        auto cnmt = Sanitize(cnmtId, 32), content = Sanitize(programContentId, 64);
        auto source = Sanitize(applicationSource, 64);
        {
            auto &s = Get(); std::lock_guard lock(s.mutex);
            s.activeTitleId = app; s.activeApplicationSource = source; s.activeProgramId = std::move(program);
            s.activeCnmtId = std::move(cnmt); s.activeProgramContentId = std::move(content); PublishContext(s);
        }
        NotifyContentIdentity(app, source); // Never call Java under the sink mutex.
    } catch (...) {}
}
std::string ApplicationTitleId() { auto &s = Get(); std::lock_guard lock(s.mutex); return s.activeTitleId; }
void SetEffectiveExeFs(std::string_view programId, std::string_view contentId, std::string_view source) noexcept {
    try {
        auto program = Sanitize(programId, 32), content = Sanitize(contentId, 64), origin = Sanitize(source, 64);
        auto &s = Get(); std::lock_guard lock(s.mutex);
        s.effectiveProgramId = std::move(program); s.effectiveProgramContentId = std::move(content);
        s.effectiveSource = std::move(origin); PublishContext(s);
    } catch (...) {}
}
void SetLaunchId(std::string_view launchId, bool newLaunch) noexcept {
    try {
        auto value = Sanitize(launchId, 128); auto &s = Get(); std::lock_guard lock(s.mutex);
        s.observationEpoch.fetch_add(1); // Reject a timer snapshot during the reset/publication boundary.
        try {
        s.activeLaunchId = std::move(value);
        if (newLaunch) {
            s.stage.clear(); s.presentReturns = 0; s.presentAccepted = 0; s.presentOther = 0;
            s.firstPresentUs = 0; s.lastPresentResult = 0; s.stopRequested = false; s.launchStartedUs = MonotonicUs();
        }
        if (newLaunch || launchId.empty()) s.guestStage = "unknown";
        PublishContext(s);
        } catch (...) { s.observationEpoch.fetch_add(1); throw; }
        s.observationEpoch.fetch_add(1);
    } catch (...) {}
}
void PresentResult(int32_t result) noexcept {
    auto &s = Get(); if (!IsEnabled(s)) return;
    s.presentReturns.fetch_add(1, std::memory_order_relaxed); s.lastPresentResult.store(result, std::memory_order_relaxed);
    if (result == 0 || result == 1000001003) { // VK_SUCCESS / VK_SUBOPTIMAL_KHR: accepted by the host API.
        s.presentAccepted.fetch_add(1, std::memory_order_relaxed);
        uint64_t empty{};
        if (!s.firstPresentUs.load(std::memory_order_relaxed)) s.firstPresentUs.compare_exchange_strong(empty, MonotonicUs(), std::memory_order_relaxed);
    } else s.presentOther.fetch_add(1, std::memory_order_relaxed);
}
void RequestLaunchStop() noexcept {
    auto &s = Get(); s.stopRequested = true;
    Emit("lifecycle", "LAUNCH_STOP_REQUESTED", {{"source", "JNI_stopEmulation"}, {"scope", "host_stop_request_not_crash_classification"}});
}
void EndLaunch(std::string_view observedReason) noexcept {
    auto &s = Get(); if (!IsEnabled(s)) return;
    try {
        EmitFirstPresent(s);
        EmitProgress(s, "LAUNCH_PROGRESS");
        Emit("lifecycle", "LAUNCH_ENDED", {{"end_reason", observedReason},
            {"end_reason_source", "JNI_executeApplication_control_flow"}, {"host_stop_requested", s.stopRequested ? "true" : "false"},
            {"last_success_stage_before_end", LastStage()}, {"present_returns", std::to_string(s.presentReturns.load())},
            {"present_accepted", std::to_string(s.presentAccepted.load())}, {"present_other_results", std::to_string(s.presentOther.load())},
            {"first_accepted_present_monotonic_us", std::to_string(s.firstPresentUs.load())},
            {"visibility", "not_observed"}, {"guest_execution_phase", "not_inferred"}});
    } catch (...) {}
}
void Stage(std::string_view stage) noexcept {
    auto &s = Get(); if (!IsEnabled(s)) return;
    try { std::lock_guard lock(s.mutex); s.stage = Sanitize(stage, 128); PublishContext(s); } catch (...) {}
    const std::pair<std::string_view, std::string_view> fields[]{ {"stage", stage} };
    Enqueue(s, "lifecycle", "STAGE_COMPLETED", fields, "INFO", &stage);
}
std::string LastStage() { auto &s = Get(); std::lock_guard lock(s.mutex); return s.stage; }
Statistics Stats() noexcept { auto &s = Get(); return {s.accepted.load(), s.dropped.load(), s.written.load(), s.diskErrors.load(),
    s.droppedContention.load(), s.droppedFull.load(), s.droppedAllocation.load(), s.droppedDisk.load(),
    s.criticalDropped.load(), s.snapshotErrors.load(), s.snapshotEvictions.load()}; }
}
