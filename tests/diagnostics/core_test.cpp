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
int main(int argc, char **argv) {
    assert(argc == 2);
    std::filesystem::path root(argv[1]);
    std::filesystem::remove_all(root);
    Config c{root.string(), "native-test", 1024, 65536, 3, true, {}};
    assert(!Enabled()); Emit("test", "DISABLED"); assert(Stats().accepted == 0);
    assert(Start(c));
    Eventually([] { Stage("ready"); return LastStage() == "ready"; });
    std::string owned = "hello\n\"世界 content://secret/game.nsp";
    Accepted([&] { Emit("test", "OWNED", {{"message", owned}}); });
    owned = "MUTATED";
    Accepted([] { Emit("test", "INVALID_UTF8", {{"message", std::string("\xff\xed\xa0\x80", 4)}}); });
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
    assert(data.find("\\u000a\\\"") != std::string::npos);
    assert(data.find("\"last_stage\":\"ready\"") != std::string::npos);
    assert(data.find("\"title_id\":\"0100000000001000\"") != std::string::npos);
    assert(data.find("\"title_id\":\"separate-dlc-id\"") != std::string::npos);
    CheckSummary(root);
    assert(Sanitize("scripts/data/loader.lua") == "scripts/data/loader.lua");
    assert(Sanitize("scripts/storage/guest.lua") == "scripts/storage/guest.lua");
    assert(Sanitize("failed '/data/user_de/0/app/My Secret.nsp' next") == "failed '[REDACTED]' next");
    assert(Sanitize("FILE:///mnt/private/secret CONTENT://provider/token") == "[REDACTED] [REDACTED]");
    std::filesystem::copy_file(root / "diagnostics-0.jsonl", root / "encoding.jsonl");
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
    std::cout << "PASS: disabled gate, ownership, UTF-8, redaction, title context/reset, flush, stable shutdown counters, overflow, rotation/retention, restart, rejected writes, symlink safety\n";
}
