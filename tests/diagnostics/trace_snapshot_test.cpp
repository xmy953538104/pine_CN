// SPDX-License-Identifier: MPL-2.0
#include "diagnostics.h"
#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
using namespace skyline::diagnostics;
static std::string Read(const std::filesystem::path &path) { std::ifstream input(path); return {std::istreambuf_iterator<char>(input), {}}; }
template<class Operation> static void Accepted(Operation operation) {
    const auto before = Stats().accepted;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (Stats().accepted == before) { assert(std::chrono::steady_clock::now() < end); operation(); std::this_thread::yield(); }
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const std::filesystem::path root(argv[1]);
    Config config{root.string(), "trace-snapshot-test", 1024, 65536, 2, false, {}};
    assert(Start(config));
    std::string frames;
    for (int index = 0; index < 16; ++index) { if (index) frames += ';'; frames += "libskyline.so+0x1234@" + std::string(40, 'a'); }
    const std::string id(64, 'c');
    // Same full 24-field order as the production signaled-exit import. Host
    // exit time/scope must survive beside historical identity and actual frames.
    const std::array<std::pair<std::string_view, std::string_view>, 24> fields{{
        {"source", "Android_ApplicationExitInfo"}, {"historical_record", "true"}, {"exit_record_id", id},
        {"reason_code", "2"}, {"status", "11"}, {"exit_wall_clock_ms", "1792166400000"},
        {"trace_scope", "host_process_candidate_not_guest"},
        {"association_status", "verified"}, {"previous_application_title_id", "0100751007ADA000"},
        {"previous_application_identity_source", "cnmt_application"},
        {"previous_session_id", "previous-session"}, {"previous_launch_id", "previous-launch"},
        {"previous_process_run_id", "previous-process"}, {"previous_commit", "cccccccccccccccccccccccccccccccccccccccc"},
        {"trace_status", "ok"}, {"signal_number", "11"}, {"abort_signature", "unknown"},
        {"frames", frames}, {"frames_seen", "16"}, {"frames_omitted", "0"},
        {"trace_complete", "true"}, {"trace_bytes_read", "393216"}, {"trace_capture_status", "complete"},
        {"trace_attempt", "1"}}};
    Accepted([&] { EmitFields("lifecycle", "PREVIOUS_NATIVE_TRACE", fields); });
    assert(Flush(2000));
    const std::string payload(2048, 'x');
    for (int i = 0; i < 240; ++i) Accepted([&] { Emit("storage", "DLC_DATA_READ", {{"payload", payload}}); });
    Stop();
    assert(Read(root / "diagnostics-0.jsonl").find("PREVIOUS_NATIVE_TRACE") == std::string::npos);
    assert(Read(root / "diagnostics-1.jsonl").find("PREVIOUS_NATIVE_TRACE") == std::string::npos);
    const auto state = Read(root / "critical-state.jsonl");
    assert(state.find(frames) != std::string::npos && state.find("\"frames_seen\":\"16\"") != std::string::npos);
    assert(state.find("\"fields_omitted\":0") != std::string::npos);
    assert(state.find("\"trace_complete\":\"true\"") != std::string::npos && state.find("\"trace_attempt\":\"1\"") != std::string::npos);
    assert(state.find("\"exit_wall_clock_ms\":\"1792166400000\"") != std::string::npos && state.find("host_process_candidate_not_guest") != std::string::npos);
    assert(std::filesystem::file_size(root / "critical-state.jsonl") <= 65536);
}
