// SPDX-License-Identifier: MPL-2.0
// Real background writer and files, synthetic admitted guest events. No APK.
#include "diagnostics.h"
#include "resource_observer.h"
#include "test_hooks.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
using namespace skyline::diagnostics;
namespace {
std::mutex mutex;
std::condition_variable condition;
bool paused{}, release{};
void Hook(testing::Point point, std::string_view event) {
    if (point != testing::Point::BeforeRecordWrite || event != "BLOCK_WRITER") return;
    std::unique_lock lock(mutex); paused = true; condition.notify_all();
    condition.wait(lock, [] { return release; });
}
std::string Read(const std::filesystem::path &path) {
    std::ifstream file(path); return {std::istreambuf_iterator<char>(file), {}};
}
void Save(const std::filesystem::path &from, const std::filesystem::path &to) { std::ofstream(to) << Read(from); }
template<class Predicate> void Await(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) { assert(std::chrono::steady_clock::now() < deadline); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
}
void Pause() {
    { std::lock_guard lock(mutex); paused = release = false; }
    testing::hook = Hook; Emit("test", "BLOCK_WRITER");
    std::unique_lock lock(mutex); assert(condition.wait_for(lock, std::chrono::seconds(5), [] { return paused; }));
}
void Resume() {
    { std::lock_guard lock(mutex); release = true; condition.notify_all(); }
    assert(Flush(5000)); testing::hook = nullptr;
}
void Break(bool notification = false) {
    Emit("guest", "GUEST_BREAK_REQUEST", {{"notification_only", notification ? "true" : "false"},
        {"location", "kernel::svc::Break"}, {"break_reason", notification ? "2147483648" : "0"}}, "WARN");
}
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const std::filesystem::path root(argv[1]);
    testing::resourceClockMs = 1000;
    Config config; config.directory = (root / "enabled").string(); config.sessionId = "break-session";
    config.processRunId = "break-process"; config.maxFileBytes = 1024 * 1024;
    const auto directory = std::filesystem::path(config.directory);
    assert(Start(config)); SetLaunchId("break-launch", true); ConfigureResourceObservation(true, "official");
    Await([&] { return Read(directory / "diagnostics-0.jsonl").find("RESOURCE_GPU_SAMPLE") != std::string::npos; });
    assert(Flush(2000));
    const auto initial = Read(directory / "resource-window.jsonl"); assert(!initial.empty());
    Break(true);
    Emit("guest", "GUEST_BREAK_REQUEST", {{"notification_only", "unknown"}}, "WARN");
    Emit("guest", "GUEST_BREAK_REQUEST", {}, "WARN");
    assert(Flush(2000));
    assert(Read(directory / "resource-window.jsonl") == initial); // Notification is not a checkpoint request.
    Pause();
    const auto before = Stats().accepted;
    Break(); // Returns while the ordinary writer is blocked in a test hook.
    assert(Stats().accepted == before + 1);
    assert(Read(directory / "resource-window.jsonl") == initial);
    Resume();
    const auto checkpoint = Read(directory / "resource-window.jsonl");
    assert(checkpoint.find("\"reason\":\"guest_break_observed\"") != std::string::npos);
    assert(checkpoint.find("GUEST_BREAK_REQUEST") != std::string::npos);
    assert(Read(directory / "critical-state.jsonl").find("GUEST_BREAK_REQUEST") != std::string::npos);
    Save(directory / "resource-window.jsonl", directory / "break-window.jsonl");
    Save(directory / "critical-state.jsonl", directory / "break-critical.jsonl");
    const std::string payload(1900, 's');
    for (int index{}; index < 120; ++index) {
        Emit("resource", "CONTROLLED_DENSE_SAMPLE", {{"sample", payload}}); assert(Flush(2000));
    }
    Emit("exception", "CONTROLLED_AFTER_DENSE_SAMPLES"); assert(Flush(2000));
    Save(directory / "resource-window.jsonl", directory / "dense-window.jsonl");
    Stop();

    config.directory = (root / "resource-off").string(); config.sessionId = "resource-off";
    assert(Start(config)); Break(); assert(Flush(2000));
    assert(!std::filesystem::exists(std::filesystem::path(config.directory) / "resource-window.jsonl"));
    assert(Read(std::filesystem::path(config.directory) / "critical-state.jsonl").find("GUEST_BREAK_REQUEST") != std::string::npos);
    Stop();
    const auto accepted = Stats().accepted;
    config.directory = (root / "diagnostics-off").string();
    Break(); // Full diagnostics off: no writer, queue admission or file.
    assert(Stats().accepted == accepted && !std::filesystem::exists(config.directory));

    // The existing bounded queue may reject the request. Never promise crash atomicity.
    config.directory = (root / "overflow").string(); config.sessionId = "overflow"; config.queueCapacity = 16;
    assert(Start(config)); Pause();
    for (int index{}; index < 128; ++index) Emit("exception", "FILL_CRITICAL_QUEUE");
    const auto full = Stats(); Break();
    assert(Stats().accepted == full.accepted && Stats().criticalDropped == full.criticalDropped + 1);
    Resume(); Stop();
    assert(Read(std::filesystem::path(config.directory) / "critical-state.jsonl").find("GUEST_BREAK_REQUEST") == std::string::npos);

    config.directory = (root / "checkpoint-fault").string(); config.sessionId = "checkpoint-fault";
    config.queueCapacity = 1024;
    const auto fault = std::filesystem::path(config.directory);
    assert(Start(config)); SetLaunchId("fault-launch", true); ConfigureResourceObservation(true, "official");
    Await([&] { return Read(fault / "diagnostics-0.jsonl").find("RESOURCE_GPU_SAMPLE") != std::string::npos; });
    assert(Flush(2000));
    const auto old = Read(fault / "resource-window.jsonl"); assert(!old.empty());
    std::filesystem::create_directory(fault / "resource-window.tmp"); // Real policy-rejected temporary inode.
    Break(); assert(Flush(2000));
    assert(Stats().diskErrors == 0 && Stats().snapshotErrors > 0);
    assert(Read(fault / "resource-window.jsonl") == old);
    assert(Read(fault / "critical-state.jsonl").find("GUEST_BREAK_REQUEST") != std::string::npos);
    Save(fault / "critical-state.jsonl", fault / "break-critical.jsonl");
    assert(std::filesystem::remove(fault / "resource-window.tmp"));
    Emit("exception", "CONTROLLED_BREAK_CHECKPOINT_RECOVERY"); assert(Flush(2000));
    assert(Read(fault / "resource-window.jsonl").find("CONTROLLED_BREAK_CHECKPOINT_RECOVERY") != std::string::npos);
    Stop();
}
