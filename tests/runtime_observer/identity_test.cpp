// SPDX-License-Identifier: MPL-2.0
#include "runtime_observer.h"
#include "runtime_bridge.h"
#include "diagnostics.h"
#include <cassert>
#include <filesystem>
#include <thread>
#include <vector>
#include <cstdio>
using namespace skyline::diagnostics;
namespace r=skyline::diagnostics::runtime;
namespace i=skyline::diagnostics::image;
int main(int argc,char **argv){
 assert(argc==2);Config config;config.directory=argv[1];config.sessionId="runtime-identity-test";config.processRunId="synthetic-process";
 assert(Start(config));SetLaunchId("synthetic-launch",true);r::ConfigureIdentity(true);
 std::array<std::vector<uint8_t>,3> bytes{std::vector<uint8_t>(128,0x11),std::vector<uint8_t>(128,0x22),std::vector<uint8_t>(128,0x33)};
 r::Profile profile;profile.preimage.buildId[0]=0x42;profile.preimage.windowCount=2;
 profile.requests[0]={0,8};profile.requests[1]={4096,8};
 for(size_t n{};n<3;++n){profile.segments[n]={uint32_t(n*4096),128};profile.preimage.segmentHashes[n]=i::Sha256(bytes[n]);}
 for(size_t n{};n<2;++n)profile.preimage.windows[n]={uint32_t(n*4096),8,i::Sha256(std::span(bytes[n]).first(8))};
 i::Configure(true,{profile.requests.data(),2});i::Input input;input.name="sdk.nso";input.buildId=profile.preimage.buildId;
 for(size_t n{};n<3;++n)input.segments[n]={uint32_t(n*4096),128,bytes[n]};
 auto source=i::Capture(input);assert(source);
 {
 r::ProcessObservation process(profile,r::Domain::ControlledSynthetic);
 {r::ScopedLoad load(process,&source->record);r::ScopedLoad::Mapped(bytes[0],bytes[1],0x7100000000,0x7100000000,0x3000,true);}
 assert(process.Validated());auto module=process.Binding();assert(module&&module->domain==r::Domain::ControlledSynthetic&&module->cachePublished);
 // Input and loaded provenance are independently owned, not borrowed spans.
 bytes[0][0]^=1;assert(module->loadedWindows[0].bytes[0]==0x11);bytes[0][0]^=1;
 process.BeforeMappingMutation(0x7200000000,4096);assert(process.Validated());
 process.BeforeMappingMutation(0x7100000000,4);assert(!process.Validated());assert(module->stopReason==uint64_t(r::StopReason::MappingChanged));
 }
 {r::ProcessObservation p(profile,r::Domain::ControlledSynthetic);auto changed=bytes[0];changed[0]^=1;{r::ScopedLoad load(p,&source->record);r::ScopedLoad::Mapped(changed,bytes[1],0x7100000000,0x7100000000,0x3000,true);}assert(!p.Validated()&&!p.Binding());}
 {r::ProcessObservation p(profile,r::Domain::ControlledSynthetic);{r::ScopedLoad load(p,&source->record);r::ScopedLoad::Mapped(bytes[0],bytes[1],0x7100000000,0x7100000000,0x3000,false);}assert(!p.Validated());}
 {r::ProcessObservation p(profile,r::Domain::ControlledSynthetic);p.SealLoads();{r::ScopedLoad load(p,&source->record);assert(!r::ScopedLoad::Active());}assert(!p.Validated());}
 {r::ProcessObservation p(profile,r::Domain::ControlledSynthetic);auto bad=source->record;bad.segments[1].va+=16;{r::ScopedLoad load(p,&bad);r::ScopedLoad::Mapped(bytes[0],bytes[1],0x7100000000,0x7100000000,0x3000,true);}assert(!p.Validated());}
 {r::ProcessObservation p(profile,r::Domain::ControlledSynthetic);{r::ScopedLoad load(p,&source->record);r::ScopedLoad::Mapped(bytes[0],bytes[1],0x7100000000,0x7100000000,0x3000,true);}assert(p.Validated());r::ConfigureIdentity(false);assert(!p.Validated());}
 r::ConfigureIdentity(true);i::Configure(true,{profile.requests.data(),2});source=i::Capture(input);assert(source);
 {r::ProcessObservation p(profile,r::Domain::ControlledSynthetic);SetLaunchId("other-launch",true);{r::ScopedLoad load(p,&source->record);r::ScopedLoad::Mapped(bytes[0],bytes[1],0x7100000000,0x7100000000,0x3000,true);}assert(!p.Validated());}
 r::ConfigureIdentity(false);Stop();std::puts("PASS production runtime identity: loaded-vs-input windows, layout, cache flag, invalidation, sealed lifecycle, disabled and stale owner; controlled host dependencies, no Guest execution");
}
