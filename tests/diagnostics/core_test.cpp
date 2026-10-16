#include "diagnostics.h"
#include <cassert>
#include <chrono>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <csignal>
#include <pthread.h>
#include <sys/resource.h>
#include <thread>
#include <vector>
using namespace skyline::diagnostics;
template<typename F> void Eventually(F predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
template<typename F> void Accepted(F emit) {
    const auto before = Stats().accepted;
    Eventually([&] { emit(); return Stats().accepted > before; });
}
std::string Read(const std::filesystem::path &path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void CheckSummary(const std::filesystem::path &root) {
    std::ifstream file(root / "diagnostics-0.jsonl");
    std::string line, last;
    while (std::getline(file, line)) last = line;
    const auto stats = Stats();
    assert(last.find("\"event\":\"SESSION_SUMMARY\"") != std::string::npos);
    assert(last.find("\"dropped_count\":" + std::to_string(stats.dropped) + ',') != std::string::npos);
    assert(last.find("\"accepted\":\"" + std::to_string(stats.accepted) + "\"") != std::string::npos);
    assert(last.find("\"written\":\"" + std::to_string(stats.written) + "\"") != std::string::npos);
}
void CheckTitle(const std::filesystem::path &root, std::string_view event, std::string_view title) {
    std::ifstream file(root / "diagnostics-0.jsonl");
    std::string line;
    bool found{};
    while (std::getline(file, line)) {
        if (line.find("\"event\":\"" + std::string(event) + "\"") == std::string::npos) continue;
        const auto fields = line.find(",\"fields\":");
        assert(fields != std::string::npos);
        // Inspect the record identity, independently of any content title field.
        assert(line.substr(0, fields).find(",\"title_id\":\"" + std::string(title) + "\"") != std::string::npos);
        found = true;
    }
    assert(found);
}
void CheckActiveTitleLifecycle(const std::filesystem::path &root) {
    constexpr auto firstTitle = "0100000000001000";
    constexpr auto secondTitle = "0100000000002000";
    Config config{(root / "disabled-load").string(), "native-test", 1024, 65536, 3, false, {}};
    ClearTitleId();
    assert(!Enabled());
    const auto before = Stats();
    SetTitleId(firstTitle); // A real base can be selected while diagnostics are off.
    Emit("guest", "DISABLED_GAME_EVENT");
    const auto after = Stats();
    assert(!Enabled() && before.accepted == after.accepted && before.written == after.written);
    assert(before.dropped == after.dropped && before.diskErrors == after.diskErrors);
    assert(!std::filesystem::exists(config.directory));
    assert(Start(config));
    Accepted([] { Emit("lifecycle", "SESSION_METADATA"); });
    Accepted([secondTitle] { Emit("storage", "ACTIVE_GAME_READ", {{"title_id", secondTitle}}); });
    assert(Flush(2000));
    CheckTitle(config.directory, "SESSION_METADATA", firstTitle);
    Stop();
    CheckTitle(config.directory, "SESSION_METADATA", firstTitle);
    CheckTitle(config.directory, "ACTIVE_GAME_READ", firstTitle);
    CheckTitle(config.directory, "SESSION_SUMMARY", firstTitle);

    // Stop is a logging toggle, and Start can also replace a live configuration.
    config.directory = (root / "resumed").string();
    assert(Start(config));
    Accepted([] { Emit("lifecycle", "SESSION_METADATA"); });
    const auto resumed = config.directory;
    config.directory = (root / "reconfigured").string();
    config.verbose = true;
    config.categories = "guest";
    assert(Start(config));
    assert(Verbose());
    CheckTitle(resumed, "SESSION_METADATA", firstTitle);
    CheckTitle(resumed, "SESSION_SUMMARY", firstTitle);
    Accepted([] { Emit("lifecycle", "SESSION_METADATA"); });
    Accepted([] { Emit("guest", "ACTIVE_GAME_DEBUG"); });
    assert(Flush(2000));
    Stop();
    CheckTitle(config.directory, "SESSION_METADATA", firstTitle);
    CheckTitle(config.directory, "ACTIVE_GAME_DEBUG", firstTitle);
    CheckTitle(config.directory, "SESSION_SUMMARY", firstTitle);

    // The current base can change while logging remains disabled.
    SetTitleId(secondTitle);
    config.directory = (root / "disabled-replaced").string();
    config.verbose = false;
    config.categories.clear();
    assert(Start(config));
    Accepted([] { Emit("lifecycle", "SESSION_METADATA"); });
    Accepted([] { Emit("exception", "ACTIVE_GAME_ERROR", {}, "ERROR"); });
    assert(Flush(2000));
    Stop();
    CheckTitle(config.directory, "SESSION_METADATA", secondTitle);
    CheckTitle(config.directory, "ACTIVE_GAME_ERROR", secondTitle);
    CheckTitle(config.directory, "SESSION_SUMMARY", secondTitle);

    // The real game end clears only after its summary; a settings session has no game.
    ClearTitleId();
    config.directory = (root / "settings-after-end").string();
    assert(Start(config));
    Accepted([] { Emit("lifecycle", "SESSION_METADATA"); });
    assert(Flush(2000));
    Stop();
    CheckTitle(config.directory, "SESSION_METADATA", "");
    CheckTitle(config.directory, "SESSION_SUMMARY", "");

    // A new unknown-title game must not inherit an earlier known game's identity,
    // even when diagnostics stay enabled throughout the launch boundary.
    config.directory = (root / "unknown-game").string();
    assert(Start(config));
    SetTitleId(firstTitle);
    Accepted([] { Emit("guest", "PREVIOUS_GAME_EVENT"); });
    ClearTitleId();
    Accepted([secondTitle] { Emit("lifecycle", "SESSION_METADATA", {{"title_id", secondTitle}}); });
    assert(Flush(2000));
    Stop();
    CheckTitle(config.directory, "PREVIOUS_GAME_EVENT", firstTitle);
    CheckTitle(config.directory, "SESSION_METADATA", "");
    CheckTitle(config.directory, "SESSION_SUMMARY", "");
    ClearTitleId();
}
void CheckLocationRedaction() {
    for (std::string_view location : {
            "/storage/emulated/0/Alice Smith/秘密 \"save\".nsp",
            "/data/user/0/app/Private Name/O'Connor 秘密.nsp",
            "/cache/Private Name/秘密.nsp", "/home/Alice Smith/秘密.nsp",
            "C:\\Users\\Alice Smith\\秘密 \"save\".nsp",
            "D:/Private Name/O'Connor 秘密.nsp",
            "\\\\network\\Private Share\\Alice Smith.nsp",
            "\\\\research\\Private Share\\Alice Smith.nsp",
            "content://provider/Private Name/秘密 \"save\".nsp",
            "https://provider/Private Name/秘密.nsp",
            "FILE:///storage/emulated/0/Private Name/秘密.nsp",
            "file:/storage/emulated/0/Private Name/秘密.nsp",
            "file:C:\\Users\\Private Name\\秘密.nsp"}) {
        for (std::string_view prefix : {"failed: ", "读取失败：", "path=(", "path='", "path=\"", "path:", "std::filesystem::path:", "ns::"}) {
            assert(Sanitize(std::string(prefix) + std::string(location) + "'\" trailing detail") == std::string(prefix) + "[REDACTED]");
        }
        if (location.front() != '/') assert(Sanitize("正在打开" + std::string(location)) == "正在打开[REDACTED]");
    }
    const std::string relative = "scripts/data/a.lua ./data/a.lua ../data/a.lua 脚本/data/a.lua scripts/storage/a.lua /database/a.lua /datafile/a.lua rom:/data/a.lua romfs:/storage/a.lua sdmc:/data/a.lua ns::rom:/data/a.lua";
    assert(Sanitize(relative) == relative);
    const std::string lines = "世界 open /data/user/0/Alice Smith/秘密\nscripts/data/a.lua\r\n读取失败：file:/private/Alice Smith\r尾行 世界";
    assert(Sanitize(lines) == "世界 open [REDACTED]\nscripts/data/a.lua\r\n读取失败：[REDACTED]\r尾行 世界");
    assert(Sanitize(std::string("\xff\n", 2) + "path:/data/user/0/Alice Smith") == "\xef\xbf\xbd\npath:[REDACTED]");
    for (const std::string invalid : {"\xe0\x80\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80"}) {
        const auto safe = Sanitize(invalid + "/data/user/0/Alice Smith");
        assert(safe.ends_with("[REDACTED]") && safe.find("Alice") == std::string::npos);
    }
    assert(Sanitize(std::string(10000, 'a'), 32) == std::string(32, 'a'));
    assert(Sanitize("/data/user/0/Alice Smith", 0).empty());
    assert(Sanitize("\xe4\xb8\x96", 2) == "\xef\xbf\xbd\xef\xbf\xbd");
    assert(Sanitize("failed '/data/user_de/0/app/My Secret.nsp' next") == "failed '[REDACTED]");
    assert(Sanitize("FILE:///mnt/private/secret CONTENT://provider/token") == "[REDACTED]");
}
int main(int argc, char **argv) {
    assert(argc == 2);
    CheckLocationRedaction();
    std::filesystem::path root(argv[1]);
    std::filesystem::remove_all(root);
    Config c{root.string(), "native-test", 1024, 65536, 3, true, {}};
    assert(!Enabled()); Emit("test", "DISABLED"); assert(Stats().accepted == 0);
    CheckActiveTitleLifecycle(root / "title-lifecycle");
    assert(Start(c));
    Eventually([] { Stage("ready"); return LastStage() == "ready"; });
    std::string owned = "hello\n\"世界 content://secret/game.nsp";
    Accepted([&] { Emit("test", "OWNED", {{"message", owned}}); });
    owned = "MUTATED";
    Accepted([] { Emit("test", "INVALID_UTF8", {{"message", std::string("\xff\xed\xa0\x80", 4)}}); });
    Accepted([] { Emit("test", "SOURCE_LOCATION_REDACTION", {
        {"message", "open /data/user/0/Alice Smith/秘密 \"save\".nsp\nnext 世界"},
        {"windows", "正在打开\\\\network\\Alice Smith\\private file"},
        {"invalid_before_path", std::string("\xed\xa0\x80", 3) + "/data/user/0/Alice Smith"},
        {"relative", "scripts/data/a.lua rom:/data/a.lua"}}); });
    SetTitleId("0100000000001000");
    Accepted([] { Emit("guest", "TITLE_CONTEXT", {{"title_id", "separate-dlc-id"}}); });
    char originalName[16]{};
    assert(pthread_getname_np(pthread_self(), originalName, sizeof(originalName)) == 0);
    assert(pthread_setname_np(pthread_self(), "/sdcard/hidden") == 0);
    Accepted([] { Emit("test", "THREAD_PATH"); });
    assert(pthread_setname_np(pthread_self(), "invalid-\xff") == 0);
    Accepted([] { Emit("test", "THREAD_UTF8"); });
    assert(pthread_setname_np(pthread_self(), originalName) == 0);
    assert(Flush(2000));
    assert(Enabled() && Stats().accepted == Stats().written);
    assert(Read(root / "diagnostics-0.jsonl").find("TITLE_CONTEXT") != std::string::npos);
    Accepted([] { Emit("test", "AFTER_FLUSH"); });
    Stop();
    std::ifstream first(root / "diagnostics-0.jsonl");
    std::string data((std::istreambuf_iterator<char>(first)), {});
    assert(data.find("MUTATED") == std::string::npos);
    assert(data.find("[REDACTED]") != std::string::npos);
    assert(data.find("secret") == std::string::npos);
    assert(data.find("hidden") == std::string::npos);
    assert(data.find("Alice") == std::string::npos && data.find("Smith") == std::string::npos);
    assert(data.find("SOURCE_LOCATION_REDACTION") != std::string::npos);
    assert(data.find("open [REDACTED]\\u000anext 世界") != std::string::npos);
    assert(data.find("scripts/data/a.lua rom:/data/a.lua") != std::string::npos);
    assert(data.find("\\u000a\\\"") != std::string::npos);
    assert(data.find("\"last_stage\":\"ready\"") != std::string::npos);
    assert(data.find("\"title_id\":\"0100000000001000\"") != std::string::npos);
    assert(data.find("\"title_id\":\"separate-dlc-id\"") != std::string::npos);
    CheckSummary(root);
    std::filesystem::copy_file(root / "diagnostics-0.jsonl", root / "encoding.jsonl");
    ClearTitleId(); // This stress session starts after the preceding game ended.
    c.queueCapacity = 1;
    assert(Start(c));
    assert(Verbose());
    std::vector<std::thread> threads;
    for (int n=0; n<8; ++n) threads.emplace_back([] { for (int i=0; i<20000; ++i) Emit("stress", "EVENT", {{"payload", std::string(2048, 'x')}}); });
    for (auto &t: threads) t.join();
    Stop();
    assert(Stats().dropped > 0); assert(Stats().written > 0); assert(Stats().accepted == Stats().written);
    CheckSummary(root);
    assert(Read(root / "diagnostics-0.jsonl").find("0100000000001000") == std::string::npos);

    assert(std::filesystem::exists(root / "diagnostics-2.jsonl"));
    for (int i=0; i<3; ++i) assert(std::filesystem::file_size(root / ("diagnostics-" + std::to_string(i) + ".jsonl")) <= c.maxFileBytes);
    c.maxFiles = 1;
    for (int i=0; i<20; ++i) { assert(Start(c)); Accepted([] { Emit("test", "RESTART"); }); Stop(); CheckSummary(root); }
    assert(!std::filesystem::exists(root / "diagnostics-1.jsonl"));
    assert(!std::filesystem::exists(root / "diagnostics-2.jsonl"));
    c.categories = "content";
    c.verbose = false;
    assert(Start(c));
    assert(!Verbose());
    auto count = Stats().accepted;
    Emit("guest", "FILTERED"); assert(Stats().accepted == count);
    while (Stats().accepted == count) Emit("content", "SELECTED");
    Stop();
    c.categories.clear();
    std::atomic<bool> running{true};
    threads.clear();
    for (int n=0; n<8; ++n) threads.emplace_back([&] { while (running) { Stage("concurrent"); Emit("test", "CONCURRENT_RESTART"); } });
    for (int i=0; i<20; ++i) {
        assert(Start(c));
        Eventually([] { return Stats().accepted > 0; });
        Stop();
        const auto stats = Stats();
        CheckSummary(root);
        assert(stats.dropped == Stats().dropped && stats.accepted == Stats().accepted);
    }
    running = false; for (auto &thread : threads) thread.join();
    auto begin = std::chrono::steady_clock::now();
    for (int i=0; i<1000000; ++i) Emit("test", "DISABLED");
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    std::cout << "Host disabled Emit ns/call: " << ns / 1000000.0 << " (not Android FPS validation)\n";
    c.directory = (root / "not-a-directory").string();
    { std::ofstream file(c.directory); file << "block"; }
    assert(Start(c)); Emit("test", "DISK_FAILURE"); Stop(); assert(Stats().diskErrors == 1); assert(!Enabled()); assert(!Flush());

    // Exercise a real OS-rejected write after a record has left the queue.
    c.directory = (root / "write-failure").string();
    struct rlimit originalLimit{};
    assert(getrlimit(RLIMIT_FSIZE, &originalLimit) == 0);
    struct sigaction ignore{}, originalSignal{};
    ignore.sa_handler = SIG_IGN;
    assert(sigaction(SIGXFSZ, &ignore, &originalSignal) == 0);
    struct rlimit smallLimit{1, originalLimit.rlim_max};
    assert(setrlimit(RLIMIT_FSIZE, &smallLimit) == 0);
    assert(Start(c));
    Accepted([] { Emit("test", "REJECTED_WRITE"); });
    Eventually([] { return !Enabled(); });
    assert(!Flush(100));
    Stop();
    assert(setrlimit(RLIMIT_FSIZE, &originalLimit) == 0);
    assert(sigaction(SIGXFSZ, &originalSignal, nullptr) == 0);
    assert(Stats().diskErrors == 1 && Stats().written == 0);
    assert(Stats().dropped >= Stats().accepted);

    c.directory = (root / "retention-failure").string();
    std::filesystem::create_directories(std::filesystem::path(c.directory) / "diagnostics-1.jsonl");
    { std::ofstream keep(std::filesystem::path(c.directory) / "diagnostics-1.jsonl" / "keep"); keep << "unchanged"; }
    assert(Start(c));
    Eventually([] { return !Enabled(); });
    Stop(); assert(Stats().diskErrors == 1);
    assert(Read(std::filesystem::path(c.directory) / "diagnostics-1.jsonl" / "keep") == "unchanged");

    c.directory = (root / "symlink").string(); c.maxFiles = 3;
    std::filesystem::create_directories(c.directory);
    const auto outside = root / "outside-target";
    std::filesystem::create_symlink(outside, std::filesystem::path(c.directory) / "diagnostics-0.jsonl");
    assert(Start(c));
    Accepted([] { Emit("test", "SAFE_ROTATION"); });
    Stop();
    assert(Stats().diskErrors == 0 && !std::filesystem::exists(outside));
    CheckSummary(c.directory);
    c.directory = root.string(); c.queueCapacity = 0; assert(!Start(c));
    std::cout << "PASS: disabled gate, ownership, UTF-8, redaction, active title across off/start/reconfigure and explicit game boundaries, flush, stable shutdown counters, overflow, rotation/retention, restart, rejected writes, symlink safety\n";
}
