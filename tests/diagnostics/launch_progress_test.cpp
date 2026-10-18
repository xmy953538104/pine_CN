// SPDX-License-Identifier: MPL-2.0
// Controlled host calls into the production observer, not a GPU/APK execution test.
#include "diagnostics.h"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
using namespace skyline::diagnostics;
static std::string Read(const std::filesystem::path &path) {
    std::ifstream stream(path); return {std::istreambuf_iterator<char>(stream), {}};
}
int main(int argc, char **argv) {
    assert(argc == 2); const std::filesystem::path root(argv[1]);
    Config config{root.string(), "launch-one-session", 1024, 65536, 3, false, {}};
    SetLaunchId("launch-one", true);
    SetContentIdentity("0100000000001000", "0100000000001001", "0100000000001000", "base-program", "cnmt_application");
    SetEffectiveExeFs("0100000000001801", "update-program", "update");
    assert(Start(config)); Stage("process_data_loaded");
    PresentResult(0); PresentResult(1000001003); PresentResult(-1000001004);
    // Writer first-present observation is independent of the render thread.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (Read(root / "diagnostics-0.jsonl").find("FIRST_PRESENT_OBSERVED") == std::string::npos) {
        assert(std::chrono::steady_clock::now() < deadline); std::this_thread::yield();
    }
    RequestLaunchStop(); EndLaunch("execute_returned"); Stop();
    auto text = Read(root / "critical-state.jsonl");
    assert(text.find("LAUNCH_ENDED") != std::string::npos && text.find("\"host_stop_requested\":\"true\"") != std::string::npos);
    assert(text.find("\"present_returns\":\"3\"") != std::string::npos && text.find("\"present_accepted\":\"2\"") != std::string::npos);
    assert(text.find("\"base_program_nca\":\"base-program\"") != std::string::npos);
    assert(text.find("\"effective_exefs_program_nca\":\"update-program\"") != std::string::npos);
    assert(text.find("\"effective_exefs_source\":\"update\"") != std::string::npos);
    assert(text.find("\"visibility\":\"not_observed\"") != std::string::npos);
    assert(text.find("\"last_success_stage_before_end\":\"process_data_loaded\"") != std::string::npos);

    PresentResult(0); // Closed gate must not add an observation.
    SetLaunchId("launch-two", true); ClearTitleId();
    config.sessionId = "launch-two-session"; assert(Start(config));
    Stage("process_data_loaded");
    const auto heartbeatDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(7);
    while (Read(root / "diagnostics-0.jsonl").find("LAUNCH_PROGRESS") == std::string::npos) {
        assert(std::chrono::steady_clock::now() < heartbeatDeadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EndLaunch("execute_returned"); Stop();
    assert(Read(root / "critical-state-1.jsonl") == text);
    text = Read(root / "critical-state.jsonl");
    assert(text.find("launch-two") != std::string::npos && text.find("launch-one") == std::string::npos);
    assert(text.find("\"present_returns\":\"0\"") != std::string::npos && text.find("\"host_stop_requested\":\"false\"") != std::string::npos);
    assert(text.find("FIRST_PRESENT_OBSERVED") == std::string::npos);
    // Multiple launches in one diagnostic session keep the last eight endings.
    config.sessionId = "multiple-launches"; assert(Start(config));
    for (int i{}; i < 12; ++i) { SetLaunchId("same-session-launch-" + std::to_string(i), true); EndLaunch("execute_returned"); }
    Stop();
    text = Read(root / "critical-state.jsonl");
    assert(text.find("same-session-launch-11") != std::string::npos);
    assert(text.find("same-session-launch-4") != std::string::npos);
    for (size_t start{}; start < text.size();) {
        auto end = text.find('\n', start); if (end == std::string::npos) end = text.size();
        auto line = text.substr(start, end - start);
        if (line.find("\"observed_event\":\"LAUNCH_ENDED\"") != std::string::npos)
            assert(line.find("same-session-launch-0\"") == std::string::npos);
        start = end + 1;
    }
    assert(std::filesystem::file_size(root / "critical-state.jsonl") <= 65536);
    assert(std::filesystem::is_regular_file(root / "critical-state-2.jsonl"));
    assert(!std::filesystem::exists(root / "critical-state-3.jsonl"));
}
