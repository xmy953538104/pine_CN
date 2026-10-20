// SPDX-License-Identifier: MPL-2.0
#include "diagnostics.h"
#include "resource_observer.h"
#include "test_hooks.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <new>
#include <thread>
using namespace skyline::diagnostics;
std::atomic<bool> injected{};
void Fault(testing::Point point, std::string_view) {
    if (point == testing::Point::BeforeResourceSample && !injected.exchange(true)) throw std::bad_alloc();
}
int main(int argc, char **argv) {
    assert(argc == 2);
    testing::hook = Fault;
    Config config; config.directory = argv[1]; config.sessionId = "resource-failure-session";
    assert(Start(config)); SetLaunchId("resource-failure-launch", true);
    ConfigureResourceObservation(true, "golden_mod");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!injected && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(injected && Enabled());
    Emit("lifecycle", "AFTER_RESOURCE_ALLOCATION_FAILURE", {{"result", "original_sink_remains_alive"}});
    assert(Flush() && Stats().snapshotErrors >= 1);
    testing::hook = nullptr;
    Stop();
    std::cout << "PASS resource-sampling bad_alloc isolated; original sink accepts and flushes later records\n";
}
