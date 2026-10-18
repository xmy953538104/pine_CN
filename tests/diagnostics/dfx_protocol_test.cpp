// SPDX-License-Identifier: MPL-2.0
#include "dfx_protocol.h"
#include "guest.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
using namespace skyline::diagnostics;
static std::string Read(const std::filesystem::path &path) {
    std::ifstream stream(path); return {std::istreambuf_iterator<char>(stream), {}};
}
int main(int argc, char **argv) {
    assert(argc == 2);
    constexpr std::string_view probe = "DFX1|event=DFX_PROBE|probe_id=controlled-001|message=%E4%B8%AD%E6%96%87%20%7C%20%25";
    auto parsed = ParseDfx1(probe);
    assert(parsed.recognized && parsed.valid && parsed.probe == "controlled-001");
    assert(parsed.message == "中文 | %" && parsed.file.empty() && parsed.line.empty());
    assert(ParseDfx1(std::string(probe) + "\r\n").valid);
    constexpr std::string_view error = "DFX1|event=DFX_LUA_ERROR|probe_id=controlled-001|file=scripts/test.lua|line=42|function=fixture|message=controlled%20error|traceback=scripts/test.lua:42%0Anext%20frame";
    parsed = ParseDfx1(error);
    assert(parsed.valid && parsed.file == "scripts/test.lua" && parsed.line == "42");
    assert(parsed.traceback == "scripts/test.lua:42\nnext frame");
    for (auto wire : {"ordinary DFX1|event=DFX_MAIN_ENTER|probe_id=x", " DFX1|event=DFX_MAIN_ENTER|probe_id=x",
                      "DFX2|event=DFX_MAIN_ENTER|probe_id=x", "DFX_MAIN_ENTER legacy"}) assert(!ParseDfx1(wire).recognized);
    for (auto wire : {"DFX1|event=DFX_MAIN_ENTER", "DFX1|event=DFX_MAIN_ENTER|probe_id=",
                      "DFX1|event=DFX_MAIN_ENTER_FAKE|probe_id=x", "DFX1|event=DFX_MAIN_ENTER|probe_id=x|probe_id=y",
                      "DFX1|event=DFX_MAIN_ENTER|probe_id=x|extra=x", "DFX1|event=DFX_MAIN_ENTER|probe_id=x|",
                      "DFX1|event=DFX_MAIN_ENTER|probe_id=x|line=0", "DFX1|event=DFX_MAIN_ENTER|probe_id=x|line=-1",
                      "DFX1|event=DFX_MAIN_ENTER|probe_id=x|line=2147483648", "DFX1|event=DFX_MAIN_ENTER|probe_id=x|message=%GG",
                      "DFX1|event=DFX_MAIN_ENTER|probe_id=x|message=%00", "DFX1|event=DFX_MAIN_ENTER|probe_id=x|message=%ED%A0%80",
                      "DFX1|event=DFX_MAIN_ENTER|probe_id=x|file=bad%0Afile"}) {
        auto invalid = ParseDfx1(wire); assert(invalid.recognized && !invalid.valid && !invalid.reason.empty());
    }
    assert(!ParseDfx1("DFX1|event=DFX_PROBE|probe_id=x|message=" + std::string(385, 'x')).valid);
    assert(!ParseDfx1("DFX1|event=DFX_PROBE|probe_id=x|message=" + std::string(2100, 'x')).valid);
    assert(!ParseDfx1("DFX1|event=DFX_PROBE|probe_id=x|message=" + std::string(380, 'a') + "|traceback=" + std::string(750, 'b')).valid);
    assert(GuestMarker("DFX1|event=DFX_MAIN_ENTER|probe_id=x").empty()); // Legacy parser stays strict; the versioned path is explicit.
    assert(GuestMarker("DFX_MAIN_ENTER: existing producer") == "DFX_MAIN_ENTER");
    Config config{argv[1], "dfx-controlled-host", 1024, 262144, 2, false, {}};
    SetLaunchId("dfx-controlled-launch", true); assert(Start(config));
    GuestMessage(probe, "OutputDebugString");
    GuestMessage(error, "LogManager", "INFO", "client_log.cpp", "7", "NativeBridge", "Guest1");
    GuestMessage("DFX1|event=DFX_MAIN_ENTER_FAKE|probe_id=x", "OutputDebugString");
    for (int i{}; i < 100; ++i) GuestMessage(probe, "OutputDebugString");
    Stop();
    const auto text = Read(std::filesystem::path(argv[1]) / "diagnostics-0.jsonl");
    assert(text.find("\"event\":\"DFX_PROBE\"") != std::string::npos);
    assert(text.find("\"event\":\"DFX_LUA_ERROR\"") != std::string::npos);
    assert(text.find("\"event\":\"DFX_PROTOCOL_REJECTED\"") != std::string::npos);
    assert(text.find("\"event\":\"DFX_MAIN_ENTER\"") == std::string::npos);
    assert(text.find("\"guest_file\":\"client_log.cpp\"") != std::string::npos);
    assert(text.find("\"guest_line\":\"7\"") != std::string::npos);
    assert(text.find("\"game_script_file\":\"scripts/test.lua\"") != std::string::npos);
    assert(text.find("\"game_script_line\":\"42\"") != std::string::npos);
    assert(text.find("DFX1|event=DFX_PROBE|probe_id=controlled-001") != std::string::npos);
    assert(text.find("\"last_guest_stage\":\"DFX_MAIN_ENTER\"") == std::string::npos);
    assert(text.find("\"guest_protocol_rate_suppressed\":\"0\"") == std::string::npos);
    const auto accepted = Stats().accepted;
    GuestMessage(probe, "OutputDebugString"); assert(Stats().accepted == accepted);
}
