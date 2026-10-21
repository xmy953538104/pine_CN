// SPDX-License-Identifier: MPL-2.0
#include "nso_image.h"
#include "diagnostics.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <new>
#include <thread>
#include <vector>
using namespace skyline::diagnostics;
using namespace skyline::diagnostics::image;
std::atomic<size_t> allocations{};
std::atomic<bool> failNext{};
void *operator new(size_t size) {
    ++allocations;
    if (failNext.exchange(false)) throw std::bad_alloc();
    if (auto p=std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept {std::free(p);}
void operator delete(void *p,size_t) noexcept {std::free(p);}
unsigned checks{};
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);std::abort();}++checks;}while(false)
int main(int argc,char **argv) {
    CHECK(argc==2);
    Config config;config.directory=argv[1];config.sessionId="owned-module-session";config.processRunId="owned-process";
    CHECK(Start(config));SetLaunchId("owned-launch",true);
    ModuleOwner owner;CHECK(TryModuleOwner(owner));CHECK(OwnerText(owner.session)==config.sessionId);
    auto process=std::make_shared<int>(42);
    std::array<std::vector<uint8_t>,3> storage{std::vector<uint8_t>(4096,0xa1),std::vector<uint8_t>(4096,0xb2),std::vector<uint8_t>(128,0xc3)};
    Input input; input.name="sdk";
    for(size_t i{};i<32;++i)input.buildId[i]=uint8_t(i+1);
    input.segments[0]={0,117,storage[0]};input.segments[1]={4096,101,storage[1]};input.segments[2]={8192,128,storage[2]};
    for(auto &part:input.segments){part.hashDeclared=true;part.declaredHash=Sha256(part.contents.first(part.size));}
    const auto noAllocBefore=allocations.load();Result result;
    CHECK(!Capture(input,&result)&&result==Result::Disabled&&allocations==noAllocBefore);
    WindowRequest windows[]{{4,16},{4100,20}};
    CHECK(Configure(true,windows)==Result::Ok);
    auto pending=Capture(input,&result);CHECK(pending&&result==Result::Ok);
    const auto copied=pending->record;
    CHECK(copied.buildId==input.buildId&&copied.windowCount==2&&copied.segments[0].size==117);
    CHECK(copied.segments[0].hashMatches&&copied.segments[1].hashMatches);
    CHECK(copied.segments[0].computedHash!=Sha256(storage[0])); // unpadded only
    CHECK(!copied.liveBytesVerified&&!copied.decompressionResultObserved);
    const auto saved=storage[0];std::fill(storage[0].begin(),storage[0].end(),0xdd); // ordinary later patching
    CHECK(pending->record.windows[0].bytes[0]==0xa1&&pending->record.segments[0].computedHash==copied.segments[0].computedHash);
    CHECK(Publish(std::move(pending),process,0x7100000000,0x4000,0x7100001000)==Result::Ok);
    Record record;CHECK(Snapshot(owner,process,"sdk",record)==Result::Ok);
    CHECK(record.moduleBase==0x7100001000&&record.segments[1].mappedStart==0x7100002000);
    ExpectedPreimage expected;expected.buildId=input.buildId;expected.windowCount=record.windowCount;
    for(size_t i{};i<3;++i)expected.segmentHashes[i]=copied.segments[i].computedHash;
    for(size_t i{};i<record.windowCount;++i)expected.windows[i]={record.windows[i].va,record.windows[i].size,record.windows[i].hash};
    CHECK(VerifyPreimage(record,expected)==PreimageMatch::MatchOnly);
    auto changed=expected;changed.buildId={};CHECK(VerifyPreimage(record,changed)==PreimageMatch::UnknownExpected);
    changed=expected;changed.buildId[0]^=1;CHECK(VerifyPreimage(record,changed)==PreimageMatch::BuildIdMismatch);
    changed=expected;changed.segmentHashes[1][0]^=1;CHECK(VerifyPreimage(record,changed)==PreimageMatch::SegmentMismatch);
    changed=expected;changed.windows[0].va+=4;CHECK(VerifyPreimage(record,changed)==PreimageMatch::WindowMismatch);
    changed=expected;changed.windows[0].hash[0]^=1;CHECK(VerifyPreimage(record,changed)==PreimageMatch::WindowMismatch);
    CHECK(VerifyPreimage(copied,expected)==PreimageMatch::Unpublished);
    CHECK(LiveGotCapability()==Result::ResolverProtocolUnavailable);
    auto otherProcess=std::make_shared<int>(42);CHECK(Snapshot(owner,otherProcess,"sdk",record)==Result::NotFound);
    storage[0]=saved; // pending spans remain caller-owned; only the record owns evidence
    input.segments[0].contents=storage[0];
    auto bad=input;bad.name="content://private/rom";CHECK(!Capture(bad,&result)&&result==Result::InvalidName);
    bad=input;bad.segments[0].size=4097;CHECK(!Capture(bad,&result)&&result==Result::SegmentBounds);
    bad=input;bad.segments[0].va=UINT32_MAX-4;CHECK(!Capture(bad,&result)&&result==Result::SegmentBounds);
    CHECK(Configure(true,std::array<WindowRequest,1>{{{4090,16}}})==Result::Ok);
    CHECK(!Capture(input,&result)&&result==Result::WindowBounds);
    CHECK(Configure(true,windows)==Result::Ok);
    failNext=true;CHECK(!Capture(input,&result)&&result==Result::AllocationFailure&&!failNext);
    pending=Capture(input);CHECK(bool(pending));SetLaunchId("next-launch",true);
    CHECK(Publish(std::move(pending),process,0x7100000000,0x4000,0x7100001000)==Result::StaleOwner);
    CHECK(Snapshot(owner,process,"sdk",record)==Result::StaleOwner);CHECK(TryModuleOwner(owner));
    pending=Capture(input);CHECK(bool(pending));CHECK(Configure(true,windows)==Result::Ok);
    CHECK(Publish(std::move(pending),process,0x7100000000,0x4000,0x7100001000)==Result::StaleOwner);
    pending=Capture(input);CHECK(Publish(std::move(pending),process,UINT64_MAX-3,99,UINT64_MAX-2)==Result::MappingBounds);
    pending=Capture(input);CHECK(Publish(std::move(pending),process,0x7100000000,100,0x7100000010)==Result::MappingBounds);
    for(unsigned i{};i<MaxModules;++i){pending=Capture(input);CHECK(bool(pending));CHECK(Publish(std::move(pending),process,0x7100000000+i*0x10000,0x4000,0x7100001000+i*0x10000)==Result::Ok);}
    CHECK(Snapshot(owner,process,"sdk",record)==Result::Ambiguous);
    pending=Capture(input);CHECK(Publish(std::move(pending),process,0x7200000000,0x4000,0x7200001000)==Result::Full);
    auto weak=std::weak_ptr<int>(process);process.reset();CHECK(weak.expired()); // registry never pins process
    process=std::make_shared<int>(99);CHECK(Snapshot(owner,process,"sdk",record)==Result::NotFound);
    pending=Capture(input);CHECK(Publish(std::move(pending),process,0x7300000000,0x4000,0x7300001000)==Result::Ok);
    CHECK(Snapshot(owner,process,"sdk",record)==Result::Ok);
    // Same visible IDs after a sink restart must not revive the old token.
    pending=Capture(input);Stop();CHECK(Start(config));SetLaunchId("next-launch",true);
    CHECK(Publish(std::move(pending),process,0x7300000000,0x4000,0x7300001000)==Result::StaleOwner);
    CHECK(Configure(false)==Result::Ok);CHECK(Snapshot(owner,process,"sdk",record)==Result::Disabled);
    Stop();CHECK(!TryModuleOwner(owner));
    std::printf("PASS owned NSO input metadata/default-off/lifetime/bounds/hash/allocation/stale-generation/weak-process/budget (%u assertions)\n",checks);
}
