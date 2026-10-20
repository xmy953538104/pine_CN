// SPDX-License-Identifier: MPL-2.0
#include "diagnostics.h"
#include "resource_observer.h"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <fstream>
#include <thread>
static thread_local size_t allocations{};
void *operator new(size_t size) { ++allocations; if (auto p = std::malloc(size ? size : 1)) return p; throw std::bad_alloc(); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
int main(int argc, char **argv) {
    assert(argc == 2);
    using namespace skyline::diagnostics;
    constexpr size_t iterations = 1000000;
    static_assert(SvcEnteredMetric(1) == ResourceMetric::GuestMainSvcEntered);
    static_assert(SvcReturnedMetric(1) == ResourceMetric::GuestMainSvcReturned);
    static_assert(SvcEnteredMetric(2) == ResourceMetric::GuestOtherSvcEntered);
    static_assert(SvcReturnedMetric(2) == ResourceMetric::GuestOtherSvcReturned);
    const auto before = allocations;
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) ResourceAdd(ResourceMetric::HostSubmitAccepted);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    assert(allocations == before && !ResourceObservationEnabled());
    std::cout << "disabled_ns_per_call=" << std::chrono::duration<double, std::nano>(elapsed).count() / iterations << '\n';
    Config config; config.directory = argv[1]; config.sessionId = "resource-session";
    assert(Start(config)); SetLaunchId("resource-launch", true);
    SetContentIdentity("0100000000001000", "0100000000001000", "0100000000001000", std::string(64, 'A'), "observed_cnmt");
    SetEffectiveExeFs("0100000000001800", std::string(64, 'B'), "update");
    ConfigureResourceObservation(true, "official");
    assert(ResourceObservationEnabled());
    const auto enabledBefore = allocations;
    const auto enabledStart = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) ResourceAdd(ResourceMetric::HostSubmitAccepted);
    const auto enabledElapsed = std::chrono::steady_clock::now() - enabledStart;
    assert(allocations == enabledBefore);
    std::cout << "enabled_ns_per_call=" << std::chrono::duration<double, std::nano>(enabledElapsed).count() / iterations << '\n';
    // Wait for the real background sample, not a fabricated counter mirror.
    bool sampled{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream file(std::string(argv[1]) + "/diagnostics-0.jsonl");
        const std::string contents((std::istreambuf_iterator<char>(file)), {});
        if (contents.find("\"host_submit_accepted\":\"1000000\"") != std::string::npos) { sampled = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(sampled);
    const std::pair<std::string_view, std::string_view> fields[]{{"android_total_pss_kib", "123"}};
    assert(!EmitHostResourceFields("stale-session", "resource-launch", fields));
    assert(!EmitHostResourceFields("resource-session", "stale-launch", fields));
    assert(EmitHostResourceFields("resource-session", "resource-launch", fields));
    ConfigureResourceObservation(false, "unspecified");
    assert(!EmitHostResourceFields("resource-session", "resource-launch", fields));
    assert(Flush()); Stop();
    assert(!ResourceObservationEnabled());
    std::cout << "PASS resource gates, zero producer allocations and exact session/launch admission\n";
}
