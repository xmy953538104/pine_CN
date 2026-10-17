// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <string>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include "diagnostics/exception.h"
#include "diagnostics/guest.h"

int main(int argc, char **argv) {
    using skyline::diagnostics::GuestMarker;
    for (auto marker : {"DFX_MAIN_ENTER", "DFX_MODINDEX_READY", "DFX_MODMAIN_OK", "DFX_HAMLET_ENTER", "DFX_LUA_ERROR"}) {
        assert(GuestMarker(marker) == marker);
        assert(GuestMarker(std::string(marker) + ": payload") == marker);
        assert(GuestMarker(std::string(marker) + " payload") == marker);
        assert(GuestMarker(std::string(marker) + "\n") == marker);
        assert(GuestMarker(std::string(marker) + "_FAKE").empty());
    }
    assert(GuestMarker("").empty());
    assert(GuestMarker("error mentions DFX_LUA_ERROR").empty());
    assert(GuestMarker("DFX_UNKNOWN").empty());
    assert(GuestMarker("[DFX_MAIN_ENTER]").empty());
    assert(GuestMarker(std::string("DFX_MAIN_ENTER\0spoof", 20)).empty());
    assert(GuestMarker("普通 UTF-8 文本").empty());
    if (argc < 2)
        return 0;
    using namespace skyline::diagnostics;
    Config config;
    config.directory = argv[1];
    config.sessionId = "guest-test";
    assert(Start(config));
    for (int i = 0; i < 20; ++i) {
        GuestMessage("DFX_LUA_ERROR: 错误\n\"quoted\"", "LogManager", "ERROR", "main.lua", "42", "main", "GuestThread");
        GuestMessage("mentions DFX_HAMLET_ENTER", "OutputDebugString");
        SignalException(11, "test-safe-catch", "TestSVC");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Stop();
    const auto accepted{Stats().accepted};
    GuestMessage("DFX_MAIN_ENTER", "OutputDebugString");
    assert(Stats().accepted == accepted);
    std::ifstream file(std::filesystem::path(argv[1]) / "diagnostics-0.jsonl");
    std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    assert(text.find("\"event\":\"DFX_LUA_ERROR\"") != std::string::npos);
    assert(text.find("\"event\":\"GUEST_RUNTIME_ERROR\"") != std::string::npos);
    assert(text.find("\"event\":\"GUEST_DEBUG_MESSAGE\"") != std::string::npos);
    assert(text.find("\"event\":\"NATIVE_SIGNAL_EXCEPTION\"") != std::string::npos);
    assert(text.find("\"guest_line\":\"42\"") != std::string::npos);
    assert(text.find("\"guest_file\":\"main.lua\"") != std::string::npos);
    assert(text.find("\"event\":\"DFX_HAMLET_ENTER\"") == std::string::npos);
    assert(text.find("\"event\":\"DFX_MAIN_ENTER\"") == std::string::npos);
    assert(text.find("\"last_guest_stage\":\"guest_message_observed\"") != std::string::npos);
    assert(text.find("\"last_guest_stage\":\"DFX_HAMLET_ENTER\"") == std::string::npos);
    assert(text.find("\"last_guest_stage\":\"DFX_MAIN_ENTER\"") == std::string::npos);
    // A real allowlisted prefix changes only guest progress; loading alone never does.
    config.sessionId = "guest-marker-test";
    assert(Start(config));
    for (int i = 0; i < 20; ++i) {
        GuestMessage("DFX_MAIN_ENTER: controlled fixture", "OutputDebugString");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Stop();
    std::ifstream markers(std::filesystem::path(argv[1]) / "diagnostics-0.jsonl");
    const std::string progress{std::istreambuf_iterator<char>(markers), {}};
    assert(progress.find("\"last_guest_stage\":\"DFX_MAIN_ENTER\"") != std::string::npos);
    SetLaunchId("new-launch", true);
    config.sessionId = "guest-test"; // Restore the test folder's schema/session fixture for parser checks.
    assert(Start(config)); Stop();
    // Earlier sessions are intentionally retained; parser checks their actual per-file IDs separately.
}
