// SPDX-License-Identifier: MPL-2.0
#include "diagnostics.h"
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
struct Record {
    uint64_t sequence{}, wall{}, mono{}, dropped{};
    std::string category, event, level, thread, stage, titleId;
    std::vector<std::pair<std::string, std::string>> fields;
};
struct Sink {
    // The closed bit and active count share one atomic so Stop cannot miss an
    // emitter entering between the enabled check and its counter increment.
    std::atomic<uint64_t> producers{ProducersClosed};
    std::atomic<bool> verbose{};
    std::atomic<uint64_t> accepted{}, dropped{}, written{}, diskErrors{};
    std::mutex lifecycle, mutex, progressMutex;
    std::condition_variable wake, progress;
    std::deque<Record> queue;
    Config config;
    std::string stage, titleId;
    bool stopping{};
    std::thread worker;
    ~Sink();
};
Sink &Get() { static Sink sink; return sink; }
bool IsEnabled(const Sink &s) { return !(s.producers.load(std::memory_order_relaxed) & ProducersClosed); }
class Producer {
    Sink *sink{};
public:
    explicit Producer(Sink &s) {
        auto state = s.producers.load(std::memory_order_relaxed);
        while (!(state & ProducersClosed)) {
            if (s.producers.compare_exchange_weak(state, state + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
                sink = &s;
                break;
            }
        }
    }
    ~Producer() {
        if (sink && sink->producers.fetch_sub(1, std::memory_order_release) == ProducersClosed + 1)
            sink->producers.notify_all();
    }
    explicit operator bool() const { return sink != nullptr; }
};
void CloseProducers(Sink &s) {
    auto state = s.producers.fetch_or(ProducersClosed, std::memory_order_acq_rel) | ProducersClosed;
    while (state != ProducersClosed) {
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
std::string Serialize(const Record &r, const Config &c) {
    std::string out = "{\"schema\":1,\"session_id\":" + Quote(c.sessionId) + ",\"sequence\":" + std::to_string(r.sequence)
        + ",\"wall_clock_ms\":" + std::to_string(r.wall) + ",\"monotonic_us\":" + std::to_string(r.mono)
        + ",\"dropped_count\":" + std::to_string(r.dropped) + ",\"thread\":" + Quote(r.thread)
        + ",\"category\":" + Quote(r.category) + ",\"event\":" + Quote(r.event) + ",\"level\":" + Quote(r.level)
        + ",\"last_stage\":" + Quote(r.stage) + ",\"title_id\":" + Quote(r.titleId) + ",\"fields\":{";
    bool first = true;
    for (const auto &[key, value] : r.fields) { if (!first) out += ','; first = false; out += Quote(key) + ':' + Quote(value); }
    return out + "}}\n";
}
void Run(Sink &s) noexcept {
    bool pendingRecord{};
    try {
        const Config c = s.config;
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
        // Prune only our fixed allowlisted filenames when retention is reduced.
        for (size_t i = c.maxFiles; i < 16; ++i) {
            std::error_code ec;
            std::filesystem::remove(path(i), ec);
            if (ec) throw std::runtime_error("diagnostic retention failed");
        }
        rotate();
        while (true) {
            Record record;
            {
                std::unique_lock lock(s.mutex);
                s.wake.wait(lock, [&] { return s.stopping || !s.queue.empty(); });
                if (s.queue.empty()) break;
                record = std::move(s.queue.front()); s.queue.pop_front();
                pendingRecord = true;
            }
            auto line = Serialize(record, c);
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
            file.write(line.data(), line.size()); file.flush();
            if (!file) throw std::runtime_error("diagnostic write failed");
            bytes += line.size();
            { std::lock_guard lock(s.progressMutex); ++s.written; }
            s.progress.notify_all(); pendingRecord = false;
        }
        Record summary;
        summary.sequence = s.accepted.load() + 1;
        summary.wall = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        summary.mono = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
        summary.dropped = s.dropped;
        summary.category = "lifecycle"; summary.event = "SESSION_SUMMARY"; summary.level = "INFO"; summary.thread = "diagnostic-writer";
        { std::lock_guard lock(s.mutex); summary.stage = s.stage; summary.titleId = s.titleId; }
        summary.fields = {{"accepted", std::to_string(s.accepted.load())}, {"written", std::to_string(s.written.load())}, {"disk_errors", std::to_string(s.diskErrors.load())}};
        auto line = Serialize(summary, c);
        if (bytes && bytes + line.size() > c.maxFileBytes) rotate();
        file.write(line.data(), line.size()); file.flush();
        if (!file) throw std::runtime_error("diagnostic summary write failed");
#ifdef __ANDROID__
        __android_log_write(ANDROID_LOG_INFO, "PineDiag", line.c_str());
#endif
    } catch (...) {
        s.producers.fetch_or(ProducersClosed, std::memory_order_acq_rel);
        { std::lock_guard lock(s.progressMutex); ++s.diskErrors; }
        s.progress.notify_all();
        std::lock_guard lock(s.mutex); s.dropped += s.queue.size() + (pendingRecord ? 1 : 0); s.queue.clear();
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
void Enqueue(Sink &s, std::string_view category, std::string_view event,
             std::initializer_list<std::pair<std::string_view, std::string_view>> fields,
             std::string_view level, const std::string_view *stage = nullptr) noexcept {
    Producer producer(s);
    if (!producer) return;
    try {
        std::unique_lock lock(s.mutex, std::try_to_lock);
        if (!lock.owns_lock()) { ++s.dropped; return; }
        if (!IsEnabled(s) || s.stopping) return;
        if (!CategorySelected(s.config.categories, category)) return;
        if (stage) s.stage = Sanitize(*stage, 128);
        if (s.queue.size() >= s.config.queueCapacity) { ++s.dropped; return; }
        Record r;
        r.category = Sanitize(category, 64); r.event = Sanitize(event, 96); r.level = Sanitize(level, 16); r.stage = s.stage; r.titleId = s.titleId;
        char name[16]{}; pthread_getname_np(pthread_self(), name, sizeof(name));
        std::ostringstream thread; thread << name << ':' << std::this_thread::get_id(); r.thread = Sanitize(thread.str(), 128);
        r.wall = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        r.mono = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
        size_t budget = 4096;
        for (auto &[key, value] : fields) { if (r.fields.size() == 16 || !budget) break; auto copy = Sanitize(value, std::min<size_t>(2048, budget)); budget -= std::min(budget, copy.size()); r.fields.emplace_back(Sanitize(key, 64), std::move(copy)); }
        r.sequence = s.accepted.load() + 1; r.dropped = s.dropped;
        s.queue.push_back(std::move(r)); ++s.accepted; lock.unlock(); s.wake.notify_one();
    } catch (...) { ++s.dropped; }
}
}
std::string Sanitize(std::string_view input, size_t limit) {
    // Bound first; replace private URI/path tokens, preserving guest relative source names.
    std::string out(input.substr(0, limit));
    for (std::string_view prefix : {"content://", "file://", "/storage/", "/sdcard/", "/data/", "/mnt/", "/cache/"}) {
        size_t pos{};
        while (pos < out.size()) {
            auto found = std::search(out.begin() + pos, out.end(), prefix.begin(), prefix.end(), [](unsigned char a, unsigned char b) {
                return (a >= 'A' && a <= 'Z' ? a + ('a' - 'A') : a) == b;
            });
            if (found == out.end()) break;
            pos = static_cast<size_t>(found - out.begin());
            if (prefix.front() == '/' && pos && std::string_view(" \t\r\n\"'=([{:<>").find(out[pos - 1]) == std::string_view::npos) {
                ++pos; continue; // Relative guest names such as scripts/data/a.lua.
            }
            // A quoted path can contain spaces; keep the surrounding quotes.
            auto end = pos && (out[pos - 1] == '\'' || out[pos - 1] == '"')
                ? out.find(out[pos - 1], pos) : out.find_first_of(" \t\r\n\"'", pos);
            out.replace(pos, (end == std::string::npos ? out.size() : end) - pos, "[REDACTED]"); pos += 10;
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
            { std::lock_guard lock(s.mutex); s.config = config; s.config.sessionId = Sanitize(config.sessionId, 128); s.queue.clear(); s.stage.clear(); s.titleId.clear(); s.stopping = false; }
            s.accepted = 0; s.dropped = 0; s.written = 0; s.diskErrors = 0; s.verbose = config.verbose;
            s.producers.store(0, std::memory_order_release);
            s.worker = std::thread([&s] { Run(s); });
            return true;
        } catch (...) {
            // Keep the lifecycle lock while undoing a failed start so it cannot
            // disable or clear a different caller's newly started session.
            CloseProducers(s);
            std::lock_guard lock(s.mutex); s.dropped += s.queue.size(); s.queue.clear();
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
        const auto target = s.accepted.load();
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
        std::unique_lock lock(s.progressMutex);
        s.progress.wait_until(lock, deadline, [&] { return s.written >= target || s.diskErrors; });
        return !s.diskErrors && s.written >= target;
    } catch (...) { return false; }
}
void Emit(std::string_view category, std::string_view event, std::initializer_list<std::pair<std::string_view, std::string_view>> fields, std::string_view level) noexcept {
    auto &s = Get(); if (!IsEnabled(s)) return;
    Enqueue(s, category, event, fields, level);
}
void SetTitleId(std::string_view titleId) noexcept {
    auto &s = Get(); if (!IsEnabled(s)) return;
    Producer producer(s);
    if (!producer) return;
    try {
        auto copy = Sanitize(titleId, 32);
        std::lock_guard lock(s.mutex);
        if (IsEnabled(s) && !s.stopping) s.titleId = std::move(copy);
    } catch (...) { ++s.dropped; }
}
void Stage(std::string_view stage) noexcept {
    auto &s = Get(); if (!IsEnabled(s)) return;
    Enqueue(s, "lifecycle", "STAGE_COMPLETED", {{"stage", stage}}, "INFO", &stage);
}
std::string LastStage() { auto &s = Get(); std::lock_guard lock(s.mutex); return s.stage; }
Statistics Stats() noexcept { auto &s = Get(); return {s.accepted.load(), s.dropped.load(), s.written.load(), s.diskErrors.load()}; }
}
