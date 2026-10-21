// SPDX-License-Identifier: MPL-2.0
#include "runtime_observer.h"
#include "runtime_bridge.h"
#include "diagnostics.h"
#include <cassert>
#include <filesystem>
#include <thread>
#include <vector>
#include <cstdio>
#include <fstream>
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
 assert(process.Validated());assert(Flush(5000));
 std::ifstream events(std::filesystem::path(argv[1])/"diagnostics-0.jsonl");
 const std::string emitted{std::istreambuf_iterator<char>(events),{}};
 assert(emitted.find("MODULE_RUNTIME_IDENTITY")!=std::string::npos&&emitted.find("controlled_synthetic")!=std::string::npos);
 auto module=process.Binding();assert(module&&module->domain==r::Domain::ControlledSynthetic&&module->cachePublished);
 // Consumer-only controlled injection, NOT executed Guest capture. Exercise
 // actual slot admission/worker/Flush with interrupted and duplicate records.
 module->observerInstalled=true;
 auto first=process.AttachThread(37),second=process.AttachThread(38);assert(first&&second);
 first->x0=0xfedcba9876543210ULL;first->lr=module->callPc+4;first->sp=0x70001000;first->observerPc=0x70002000;
 first->state.store(1,std::memory_order_release);assert(Flush(5000));
 {std::ifstream file(std::filesystem::path(argv[1])/"diagnostics-0.jsonl");std::string text{std::istreambuf_iterator<char>(file),{}};assert(text.find("NVN_ABORT_CALLSITE_OBSERVED")==std::string::npos);}
 second->x0=first->x0;second->lr=first->lr;second->sp=first->sp;second->observerPc=first->observerPc;
 first->state.store(2,std::memory_order_release);second->state.store(2,std::memory_order_release);assert(Flush(5000));
 {std::ifstream file(std::filesystem::path(argv[1])/"diagnostics-0.jsonl");std::string text{std::istreambuf_iterator<char>(file),{}};
 auto at=text.find("NVN_ABORT_CALLSITE_OBSERVED");assert(at!=std::string::npos&&text.find("NVN_ABORT_CALLSITE_OBSERVED",at+1)==std::string::npos);
 assert(text.find("0xfedcba9876543210")!=std::string::npos&&text.find("post_callsite_bl_pre_original_plt")!=std::string::npos);}
 assert(module->eventPublished&&first->state==3&&second->state==2);assert(Flush(5000));
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
 // Reservation containment/overflow is checked independently of image hashes.
 SetLaunchId("reservation-cases",true);source=i::Capture(input);assert(source);
 for(unsigned bad{};bad<3;++bad){r::ProcessObservation p(profile,r::Domain::ControlledSynthetic);
 {r::ScopedLoad load(p,&source->record);r::ScopedLoad::Mapped(bytes[0],bytes[1],0x7100000000,
 bad==0?0x7100001000:bad==2?UINT64_MAX-4:0x7100000000,bad==1?4096:0x3000,true);}assert(!p.Validated());}
 // Forced activation/close interleaving: close wins irreversibly.
 SetLaunchId("forced-activation-close",true);source=i::Capture(input);assert(source);
 {r::ProcessObservation p(profile,r::Domain::ControlledSynthetic);
 r::beforeActivation=[](r::ProcessObservation &current){current.Invalidate(r::StopReason::MappingChanged);};
 {r::ScopedLoad load(p,&source->record);r::ScopedLoad::Mapped(bytes[0],bytes[1],0x7100000000,0x7100000000,0x3000,true);}
 r::beforeActivation=nullptr;assert(!p.Validated());assert(p.Binding()&&p.Binding()->active==0);}
 {r::ProcessObservation p(profile,r::Domain::ControlledSynthetic);
 {r::ScopedLoad load(p,&source->record);r::ScopedLoad::Mapped(bytes[0],bytes[1],0x7100000000,0x7100000000,0x3000,true);}assert(p.Validated());
 p.SealLoads();{r::ScopedLoad late(p,&source->record);assert(!r::ScopedLoad::Active());}
 assert(!p.Validated()&&p.Binding()->stopReason==uint64_t(r::StopReason::LateLoad));}
 {r::ProcessObservation p(profile,r::Domain::ControlledSynthetic);
 {r::ScopedLoad load(p,&source->record);r::ScopedLoad::Mapped(bytes[0],bytes[1],0x7100000000,0x7100000000,0x3000,true);}assert(p.Validated());
 p.SealLoads();{r::ScopedLoad late(p,nullptr,"sdk.nso");assert(!r::ScopedLoad::Active());}
 assert(!p.Validated()&&p.Binding()->stopReason==uint64_t(r::StopReason::LateLoad));}
 // A disable/re-enable inside the same real load scope cannot re-admit old work.
 {r::ProcessObservation p(profile,r::Domain::ControlledSynthetic);
 {r::ScopedLoad load(p,&source->record);r::ConfigureIdentity(false);r::ConfigureIdentity(true);
 r::ScopedLoad::Mapped(bytes[0],bytes[1],0x7100000000,0x7100000000,0x3000,true);}assert(!p.Validated()&&!p.Binding());}
 ModuleOwner owned;assert(TryModuleOwner(owned));std::atomic<uint64_t> gate{7};
 const std::pair<std::string_view,std::string_view> fields[]{{"source_domain","controlled_synthetic"}};
 assert(!EmitForModuleOwner(owned,"module_runtime","REJECT_DISABLED",fields,&gate,8));
 assert(EmitForModuleOwner(owned,"module_runtime","OWNER_ACCEPTED",fields,&gate,7));
 auto wrongProcess=owned;wrongProcess.process[0]^=1;assert(!EmitForModuleOwner(wrongProcess,"module_runtime","REJECT_PROCESS",fields));
 Stop();assert(Start(config));SetLaunchId("forced-activation-close",true);
 assert(!EmitForModuleOwner(owned,"module_runtime","REJECT_OLD_SINK",fields));
 r::ConfigureIdentity(false);Stop();std::puts("PASS production runtime identity: loaded-vs-input windows, layout, cache flag, invalidation, sealed lifecycle, disabled and stale owner; controlled host dependencies, no Guest execution");
}
