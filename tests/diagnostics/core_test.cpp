#include "diagnostics.h"
#include <cassert>
#include <chrono>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
using namespace skyline::diagnostics;
int main(int argc, char **argv) {
    assert(argc == 2);
    std::filesystem::path root(argv[1]);
    std::filesystem::remove_all(root);
    Config c{root.string(), "native-test", 1024, 65536, 3, true, {}};
    assert(!Enabled()); Emit("test", "DISABLED"); assert(Stats().accepted == 0);
    assert(Start(c));
    while (LastStage() != "ready") Stage("ready");
    std::string owned = "hello\n\"世界 content://secret/game.nsp";
    auto before = Stats().accepted;
    while (Stats().accepted == before) Emit("test", "OWNED", {{"message", owned}});
    owned = "MUTATED";
    before = Stats().accepted;
    while (Stats().accepted == before) Emit("test", "INVALID_UTF8", {{"message", std::string("\xff\xed\xa0\x80", 4)}});
    Stop();
    std::ifstream first(root / "diagnostics-0.jsonl");
    std::string data((std::istreambuf_iterator<char>(first)), {});
    assert(data.find("MUTATED") == std::string::npos);
    assert(data.find("[REDACTED]") != std::string::npos);
    assert(data.find("secret") == std::string::npos);
    assert(data.find("\\u000a\\\"") != std::string::npos);
    assert(data.find("\"last_stage\":\"ready\"") != std::string::npos);
    std::filesystem::copy_file(root / "diagnostics-0.jsonl", root / "encoding.jsonl");
    c.queueCapacity = 1;
    assert(Start(c));
    std::vector<std::thread> threads;
    for (int n=0; n<8; ++n) threads.emplace_back([] { for (int i=0; i<20000; ++i) Emit("stress", "EVENT", {{"payload", std::string(2048, 'x')}}); });
    for (auto &t: threads) t.join();
    Stop();
    assert(Stats().dropped > 0); assert(Stats().written > 0); assert(Stats().accepted == Stats().written);
    { std::ifstream log(root / "diagnostics-0.jsonl"); std::string final((std::istreambuf_iterator<char>(log)), {});
      assert(final.find("SESSION_SUMMARY") != std::string::npos);
      assert(final.find("\"dropped_count\":" + std::to_string(Stats().dropped)) != std::string::npos); }

    assert(std::filesystem::exists(root / "diagnostics-2.jsonl"));
    for (int i=0; i<3; ++i) assert(std::filesystem::file_size(root / ("diagnostics-" + std::to_string(i) + ".jsonl")) <= c.maxFileBytes);
    c.maxFiles = 1;
    for (int i=0; i<20; ++i) { assert(Start(c)); auto previous = Stats().accepted; while (Stats().accepted == previous) Emit("test", "RESTART"); Stop(); }
    assert(!std::filesystem::exists(root / "diagnostics-1.jsonl"));
    assert(!std::filesystem::exists(root / "diagnostics-2.jsonl"));
    c.categories = "content";
    assert(Start(c));
    auto count = Stats().accepted;
    Emit("guest", "FILTERED"); assert(Stats().accepted == count);
    while (Stats().accepted == count) Emit("content", "SELECTED");
    Stop();
    c.categories.clear();
    std::atomic<bool> running{true};
    std::thread producer([&] { while (running) Emit("test", "CONCURRENT_RESTART"); });
    for (int i=0; i<20; ++i) { assert(Start(c)); Stop(); }
    running = false; producer.join();
    auto begin = std::chrono::steady_clock::now();
    for (int i=0; i<1000000; ++i) Emit("test", "DISABLED");
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    std::cout << "Host disabled Emit ns/call: " << ns / 1000000.0 << " (not Android FPS validation)\n";
    c.directory = (root / "not-a-directory").string();
    { std::ofstream file(c.directory); file << "block"; }
    assert(Start(c)); Emit("test", "DISK_FAILURE"); Stop(); assert(Stats().diskErrors == 1); assert(!Enabled());
    c.directory = root.string(); c.queueCapacity = 0; assert(!Start(c));
    std::cout << "PASS: disabled gate, ownership, UTF-8, escaping/redaction, overflow, rotation/retention, restart, disk failure\n";
}
