// SPDX-License-Identifier: MPL-2.0
#include "diagnostics.h"
#include "admission.h"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include <vector>
using namespace skyline::diagnostics;
static std::string Read(const std::filesystem::path &path) {
    std::ifstream file(path); return {std::istreambuf_iterator<char>(file), {}};
}
template<class Operation> static void Accepted(Operation operation) {
    const auto before = Stats().accepted;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (Stats().accepted == before) { assert(std::chrono::steady_clock::now() < deadline); operation(); std::this_thread::yield(); }
}
int main(int argc, char **argv) {
    assert(argc == 2);
    assert(AdmissionLimit(8, false) == 7 && AdmissionLimit(8, true) == 8);
    assert(AdmissionLimit(1024, false) == 960 && AdmissionLimit(1024, true) == 1024);
    assert(AdmissionLimit(1, false) == 1 && AdmissionLimit(1, true) == 1);
    assert(CriticalEvent("content", "DLC_REGISTERED", "INFO"));
    assert(!CriticalEvent("guest", "GUEST_DEBUG_MESSAGE", "ERROR"));
    assert(CriticalEvent("guest", "DFX_MAIN_ENTER", "INFO"));
    const std::filesystem::path root(argv[1]);
    Config config{root.string(), "reserve-test", 64, 65536, 2, false, {}};
    assert(Start(config));
    Accepted([] { Emit("content", "BASE_SELECTED", {{"detail", "path:/data/private/secret-name"}}); });
    assert(Flush(2000));
    const std::string payload(2048, 'x');
    for (int i = 0; i < 200; ++i) Accepted([&] { Emit("storage", "DLC_DATA_READ", {{"payload", payload}}); });
    Stop();
    const auto state = Read(root / "critical-state.jsonl");
    assert(state.find("BASE_SELECTED") != std::string::npos && state.find("secret-name") == std::string::npos);
    assert(state.find("critical_snapshot") != std::string::npos && state.find("CRITICAL_STATE_SNAPSHOT") != std::string::npos);
    assert(Read(root / "diagnostics-0.jsonl").find("BASE_SELECTED") == std::string::npos);
    assert(Read(root / "diagnostics-1.jsonl").find("BASE_SELECTED") == std::string::npos);
    assert(std::filesystem::file_size(root / "critical-state.jsonl") <= 65536);

    assert(Start(config));
    for (int i = 0; i < 80; ++i) Accepted([&] { Emit("content", "DLC_REGISTERED", {{"title_id", std::to_string(i)}}); });
    Stop();
    assert(Stats().snapshotEvictions > 0 && Stats().snapshotErrors == 0);
    assert(std::filesystem::file_size(root / "critical-state.jsonl") <= 65536);
    const auto bounded = Read(root / "critical-state.jsonl");
    assert(std::count(bounded.begin(), bounded.end(), '\n') <= 32);

    // Saturation is exposed; CAS/context contention is permitted but no longer
    // forced by unrelated producer/writer metadata mutex ownership.
    assert(Start(config));
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) threads.emplace_back([&] {
        for (int j = 0; j < 20000; ++j) Emit(j % 32 ? "storage" : "content", j % 32 ? "DLC_DATA_READ" : "DLC_REGISTERED", {{"payload", payload}});
    });
    for (auto &thread : threads) thread.join();
    Stop();
    const auto stats = Stats();
    assert(stats.dropped > 0 && stats.droppedFull > 0);
    assert(stats.dropped == stats.droppedContention + stats.droppedFull + stats.droppedAllocation + stats.droppedDisk);
    assert(stats.criticalDropped > 0 && stats.criticalDropped <= stats.dropped);

    // A hostile fixed temp link disables only the optional snapshot; the regular sink survives.
    config.directory = (root / "snapshot-failure").string();
    std::filesystem::create_directories(config.directory);
    const auto outside = root / "unrelated";
    { std::ofstream file(outside); file << "preserve"; }
    std::filesystem::create_symlink(outside, std::filesystem::path(config.directory) / "critical-state.tmp");
    assert(Start(config));
    Accepted([] { Stage("ready"); });
    assert(Flush(2000)); Stop();
    assert(Stats().snapshotErrors == 1 && Stats().diskErrors == 0 && Stats().accepted == Stats().written);
    assert(Read(outside) == "preserve");
    const auto disabled = root / "disabled";
    Emit("content", "BASE_SELECTED"); assert(!std::filesystem::exists(disabled));
    std::cout << "PASS: finite critical reserve, pressure reason counters, rotation-surviving bounded/redacted state and isolated snapshot failure\n";
}
