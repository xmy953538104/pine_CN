// SPDX-License-Identifier: MPL-2.0
#include "runtime_observer.h"
#include "diagnostics.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <type_traits>

using namespace skyline::diagnostics;
namespace r = skyline::diagnostics::runtime;
namespace i = skyline::diagnostics::image;

#ifdef PINE_DIAG_RUNTIME_TEST
#error Production gate regression must compile without the controlled test constructor
#endif
static_assert(std::is_default_constructible_v<r::ProcessObservation>);
static_assert(!std::is_constructible_v<r::ProcessObservation, const r::Profile &, r::Domain, bool>);

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(!r::IdentityRequested());
    Config config;
    config.directory = argv[1];
    config.sessionId = "production-gate-synthetic-host";
    config.processRunId = "production-gate-synthetic-host";
    assert(Start(config));
    SetLaunchId("production-gate-synthetic-host", true);

    // Expected metadata values only, not a captured or authenticated game NSO.
    // The production constructor must prevent reservation before image checks,
    // including when readonly identity was explicitly selected.
    i::Record metadata;
    std::memcpy(metadata.moduleName.data(), "sdk.nso", 8);
    const auto &profile = r::SdkProfile();
    metadata.buildId = profile.preimage.buildId;
    metadata.windowCount = profile.preimage.windowCount;
    for (size_t n{}; n < metadata.segments.size(); ++n) {
        metadata.segments[n].va = profile.segments[n].first;
        metadata.segments[n].size = profile.segments[n].second;
        metadata.segments[n].computedHash = profile.preimage.segmentHashes[n];
    }
    for (size_t n{}; n < metadata.windowCount; ++n) {
        metadata.windows[n].va = profile.requests[n].va;
        metadata.windows[n].size = profile.requests[n].size;
        metadata.windows[n].hash = profile.preimage.windows[n].hash;
    }
    for (bool enabled : {false, true}) {
        r::ConfigureIdentity(enabled);
        r::ProcessObservation process;
        {
            r::ScopedLoad load(process, &metadata);
            assert(r::ScopedLoad::Active() == enabled);
            assert(r::ScopedLoad::ReservationBytes() == 0);
            assert(!process.AttachThread(17));
            assert(!process.Binding());
        }
        assert(!process.Validated());
        assert(!process.AttachThread(17));
    }
    r::ConfigureIdentity(false);
    Stop();
    std::puts("PASS production build gate: no controlled constructor, default-off identity, zero observer reservation and no Guest slots even with readonly identity selected; host execution only");
}
