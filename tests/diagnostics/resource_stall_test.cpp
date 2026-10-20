// SPDX-License-Identifier: MPL-2.0
// Actual sink integration; only ResourceTick's sampling clock is controlled.
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
#include <vector>

using namespace skyline::diagnostics;
namespace {
std::filesystem::path directory;
std::string Read(const std::filesystem::path &path) {
    std::ifstream file(path);
    return {(std::istreambuf_iterator<char>(file)), {}};
}
std::vector<std::string> Samples() {
    const auto text = Read(directory / "diagnostics-0.jsonl");
    std::vector<std::string> rows;
    size_t start{};
    for (auto end = text.find('\n'); end != text.npos; end = text.find('\n', start)) {
        auto line = text.substr(start, end - start);
        if (line.find("\"event\":\"RESOURCE_GPU_SAMPLE\"") != line.npos) rows.push_back(std::move(line));
        start = end + 1;
    }
    return rows;
}
void Require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Predicate> void Until(Predicate predicate, const std::string &description) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return;
        // Bounded predicate polling, never a blind delay waiting for an event.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto samples = Samples();
    throw std::runtime_error("Deadline: " + description + "\nLatest actual sample: " + (samples.empty() ? "none" : samples.back()) + "\nA 2ms budget backoff is a failure, never waived by this test.");
}
std::string SampleAt(uint64_t time, std::string_view state, uint64_t quiet) {
    const auto count = Samples().size();
    testing::resourceClockMs.store(time);
    std::string observed;
    Until([&] {
        const auto rows = Samples();
        if (rows.size() <= count) return false;
        observed = rows.back();
        Require(observed.find("\"sampling_over_budget\":\"0\"") != observed.npos, "Actual resource sampler exceeded 2ms budget: " + observed);
        Require(observed.find("\"observation_state\":\"" + std::string(state) + "\"") != observed.npos, "Unexpected resource state: " + observed);
        Require(observed.find("\"observed_quiet_ms\":\"" + std::to_string(quiet) + "\"") != observed.npos, "Unexpected quiet interval: " + observed);
        return true;
    }, "sample at controlled resource clock " + std::to_string(time));
    return observed;
}
void SaveWindow(std::string_view reason, std::string_view event, std::string_view filename) {
    std::string snapshot;
    Until([&] {
        snapshot = Read(directory / "resource-window.jsonl");
        return snapshot.ends_with('\n') && snapshot.find("\"reason\":\"" + std::string(reason) + "\"") != snapshot.npos
            && snapshot.find("\"event\":\"" + std::string(event) + "\"") != snapshot.npos;
    }, "actual persisted window reason " + std::string(reason));
    std::ofstream(directory / filename) << snapshot;
}
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    directory = argv[1];
    try {
        testing::resourceClockMs.store(1000);
        Config config; config.directory = directory.string(); config.sessionId = "resource-stall-session";
        Require(Start(config), "Start failed");
        SetLaunchId("resource-stall-launch", true);
        ConfigureResourceObservation(true, "official");
        Until([] { return !Samples().empty(); }, "initial RESOURCE_GPU_SAMPLE");
        for (int i{}; i < 3; ++i) PresentResult(0);
        SampleAt(2000, "observing", 0);
        for (uint64_t time = 3000; time <= 32000; time += 1000)
            SampleAt(time, time == 32000 ? "post_progress_no_new_present" : "observing", time - 2000);
        SaveWindow("post_progress_no_new_present", "RESOURCE_POST_PROGRESS_NO_NEW_PRESENT", "stall-window.jsonl");
        PresentResult(0);
        SampleAt(33000, "observing", 0);
        SaveWindow("recovered", "RESOURCE_PRESENT_RECOVERED", "recovered-window.jsonl");

        ResourcePresentationPaused(true);
        for (uint64_t time = 34000; time <= 65000; time += 1000)
            SampleAt(time, "presentation_paused", 0);
        ResourcePresentationPaused(false);
        ResourceProcessExited();
        for (uint64_t time = 66000; time <= 97000; time += 1000)
            SampleAt(time, "guest_exit_process_requested", 0);

        ConfigureResourceObservation(false, "official");
        Require(!ResourceObservationEnabled(), "Resource disable failed");
        Require(Flush(1000), "Pre-off flush failed");
        const auto count = Samples().size();
        ResourceAdd(ResourceMetric::HostSubmitAccepted, 999);
        ResourceSet(ResourceMetric::DriverFifoCapacityEntries, 12345);
        testing::resourceClockMs.store(98000);
        Emit("lifecycle", "RESOURCE_OFF_BARRIER");
        Until([] { return Read(directory / "diagnostics-0.jsonl").find("\"event\":\"RESOURCE_OFF_BARRIER\"") != std::string::npos; }, "off barrier through actual writer");
        Require(Flush(1000), "Off flush failed");
        Require(Samples().size() == count, "Resource samples continued after disable");
        Stop();
        std::cout << "PASS actual sink stall/recovery/pause/exit/off; resource clock controlled, no APK or 30s real-time claim\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        Stop();
        return 1;
    }
}
