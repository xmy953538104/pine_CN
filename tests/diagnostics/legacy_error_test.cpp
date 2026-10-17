// SPDX-License-Identifier: MPL-2.0
#include "diagnostics.h"
#include "legacy_error.h"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
using namespace skyline::diagnostics;
static std::string Read(const std::filesystem::path &path) { std::ifstream file(path); return {std::istreambuf_iterator<char>(file), {}}; }
int main(int argc, char **argv) {
    assert(argc == 2);
    LegacyErrorBudget budget;
    uint64_t suppressed{};
    for (int i = 0; i < 4; ++i) assert(budget.Take(1, suppressed) && suppressed == 0);
    for (int i = 0; i < 1000; ++i) assert(!budget.Take(1, suppressed));
    assert(budget.Take(2, suppressed) && suppressed == 1000);
    budget.Reset(); assert(budget.Take(2, suppressed) && suppressed == 0);
    const std::filesystem::path root(argv[1]);
    EmitLegacyError("audio_core", "disabled"); assert(!std::filesystem::exists(root));
    Config config{root.string(), "legacy-test", 1024, 65536, 2, false, {}};
    assert(Start(config));
    // Wait for the initial writer lock, then call the same owned-message bridge used by adapters.
    for (;;) { const auto before = Stats().accepted; Emit("test", "READY"); if (Stats().accepted != before) break; }
    assert(Flush(2000));
    std::string audio = "minimum latency error -4: /data/private/owner/audio-file";
    auto accepted = [](auto operation) {
        const auto before = Stats().accepted;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (Stats().accepted == before) { assert(std::chrono::steady_clock::now() < deadline); operation(); std::this_thread::yield(); }
    };
    accepted([&] { EmitLegacyError("audio_core", audio); });
    std::string legacyMoved = std::move(audio);
    assert(legacyMoved.find("minimum latency error -4") == 0);
    accepted([] { EmitLegacyError("shader_compiler", "synthetic shader compiler failure"); });
    for (int i = 0; i < 10000; ++i) EmitLegacyError("audio_core", "repeated error");
    Stop();
    const auto text = Read(root / "diagnostics-0.jsonl");
    assert(text.find("AUDIO_BACKEND_ERROR") != std::string::npos && text.find("SHADER_COMPILER_ERROR") != std::string::npos);
    assert(text.find("owner") == std::string::npos && text.find("[REDACTED]") != std::string::npos);
    assert(text.find("\"fatality\":\"unknown\"") != std::string::npos);
    assert(text.find("legacy_error_rate_suppressed") != std::string::npos);
    assert(Stats().accepted < 100); // Finite rate budget even if the host crosses a second boundary.
    assert(Start(config)); // Reconfiguration must reset rate admission.
    accepted([] { EmitLegacyError("audio_core", "next session error"); });
    Stop();
}
