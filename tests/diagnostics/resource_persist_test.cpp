// SPDX-License-Identifier: MPL-2.0
// Real diagnostics.cpp writer and POSIX permission failures. Synthetic events;
// only the existing resource-sampling clock is frozen, never persistence time.
#include "diagnostics.h"
#include "resource_observer.h"
#include "test_hooks.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
using namespace skyline::diagnostics;
namespace {
std::filesystem::path directory;
std::string Read(const std::filesystem::path &path) {
    std::ifstream stream(path);
    return {(std::istreambuf_iterator<char>(stream)), {}};
}
void Require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Predicate> void Until(Predicate predicate, const char *message) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    do {
        if (predicate()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < end);
    throw std::runtime_error(message);
}
bool LogContains(std::string_view value) { return Read(directory / "diagnostics-0.jsonl").find(value) != std::string::npos; }
}
int main(int argc, char **argv) {
    if (argc != 2 || ::geteuid() == 0) return 2; // Never waive DAC by running as root.
    directory = argv[1];
    try {
        testing::resourceClockMs.store(1000);
        Config config; config.directory = directory.string(); config.sessionId = "persist-session";
        config.processRunId = "persist-process";
        Require(Start(config), "Start failed");
        SetLaunchId("persist-launch", true);
        ConfigureResourceObservation(true, "official");
        Until([] { return LogContains("\"event\":\"RESOURCE_GPU_SAMPLE\""); }, "Initial actual resource sample missing");
        Emit("exception", "CONTROLLED_BASELINE_CHECKPOINT");
        Until([] { return Read(directory / "resource-window.jsonl").find("CONTROLLED_BASELINE_CHECKPOINT") != std::string::npos; }, "Baseline checkpoint missing");
        Require(Flush(1000), "Baseline flush failed");
        const auto original = Read(directory / "resource-window.jsonl");
        Require(!original.empty() && original.ends_with('\n'), "Baseline not complete");
        Require(::chmod(directory.c_str(), 0500) == 0, "Cannot inject real directory write denial");
        for (int i{}; i < 8; ++i) {
            Emit("exception", "CONTROLLED_PERSIST_DENIED");
            Require(Flush(1000), "Existing JSONL descriptor must remain writable during denied checkpoint");
        }
        Emit("lifecycle", "DENIED_BARRIER");
        Until([] { return LogContains("\"event\":\"DENIED_BARRIER\""); }, "Denied barrier missing");
        Require(Flush(1000), "Denied barrier flush failed");
        Require(Read(directory / "resource-window.jsonl") == original, "A failed checkpoint changed the completed target");
        Require(!std::filesystem::exists(directory / "resource-window.tmp"), "Failed checkpoint left temporary output");
        Require(Stats().diskErrors == 0 && Stats().snapshotErrors >= 8, "Writer vs snapshot error distinction lost");
        Require(::chmod(directory.c_str(), 0700) == 0, "Cannot restore directory permissions");
        Emit("exception", "CONTROLLED_CHECKPOINT_RECOVERY");
        Until([] { return LogContains("\"event\":\"RESOURCE_WINDOW_PERSIST_RECOVERED\""); }, "Recovery event missing");
        Require(Flush(1000), "Recovery flush failed");
        Require(Read(directory / "resource-window.jsonl").find("CONTROLLED_CHECKPOINT_RECOVERY") != std::string::npos, "Recovered real checkpoint missing trigger");
        Stop();

        // The default-off resource path creates no checkpoint or failure report.
        config.directory = (directory / "resource-off").string();
        config.sessionId = "persist-off-session";
        Require(Start(config), "Default-off Start failed");
        ConfigureResourceObservation(false, "official");
        Emit("exception", "CONTROLLED_RESOURCE_OFF");
        Require(Flush(1000), "Default-off flush failed");
        Stop();
        Require(!std::filesystem::exists(std::filesystem::path(config.directory) / "resource-window.jsonl"), "Resource-off created a window");
        std::cout << "PASS actual writer: eight denied checkpoints preserve old file, one rate-limited report, recovery; default-off no window\n";
        return 0;
    } catch (const std::exception &error) {
        ::chmod(directory.c_str(), 0700);
        std::cerr << error.what() << '\n';
        Stop();
        return 1;
    }
}
