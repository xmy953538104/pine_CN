// SPDX-License-Identifier: MPL-2.0
// Executed content-free fixtures, not a Pine Guest or SDK integration.
#include "tap_plan.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#ifdef __aarch64__
#include <sys/mman.h>
#include <unistd.h>
#include "diagnostics.h"
#include "resource_observer.h"
#endif
#ifdef NDEBUG
#error Assertions must remain enabled.
#endif
using namespace abort_tap;
unsigned checks{};
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#value); std::abort(); } ++checks; } while (false)
std::string Hex(uint64_t value) { char buffer[19]; std::snprintf(buffer,sizeof(buffer),"0x%016llx",static_cast<unsigned long long>(value)); return buffer; }
Digest Hash(std::string_view value) { return Sha256({reinterpret_cast<const uint8_t *>(value.data()),value.size()}); }
std::string HashText(const Digest &value) { std::string result; for (auto byte:value) { char c[3];std::snprintf(c,sizeof(c),"%02x",byte);result+=c; } return result; }

void HostChecks() {
    CHECK(HashText(Hash(""))=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(HashText(Hash("abc"))=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(HashText(Hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))=="248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    for (uintptr_t base : {uintptr_t(0x10000000),uintptr_t(0x7100000000)}) {
        for (int64_t delta : {-0x8000000ll,-4ll,0ll,4ll,0x7fffffcll}) {
            uint32_t word{};uintptr_t target=uintptr_t(int64_t(base)+delta),decoded{};
            CHECK(Branch(base,target,true,word));CHECK(Target(word,base,true,decoded)&&decoded==target);
            CHECK(!Target(word,base,false,decoded));
        }
        uint32_t word{};
        CHECK(!Branch(base,base+0x8000000,true,word));CHECK(!Branch(base,base-0x8000004,true,word));CHECK(!Branch(base,base+2,true,word));
    }
    uintptr_t target{};CHECK(!Target(0x97ffffff,0,true,target));CHECK(!Target(0x94000001,UINTPTR_MAX-3,true,target));
    std::vector<uint8_t> text(256);for(size_t i{};i<text.size();i+=4)Put(text,i,0xd503201f);
    Evidence e; e.enabled=e.syntheticOwned=e.producersQuiescent=e.writable=true;e.base=0x10000000;e.generation=7;
    e.call=4;e.target=64;e.stub=128;e.tail=160;e.windowStart=0;e.windowSize=16;e.moduleId=Hash("owned-synthetic-module");
    CHECK(Branch(e.base+e.call,e.base+e.target,true,e.original));Put(text,e.call,e.original);
    uint32_t tail{};CHECK(Branch(e.base+e.tail,e.base+e.target,false,tail));Put(text,e.tail,tail);
    e.segmentHash=Sha256(text);e.windowHash=Sha256(std::span(text).subspan(e.windowStart,e.windowSize));
    Plan p; CHECK(Prepare(text,e.moduleId,e,p)==Stop::None);
    const auto original=text;
    auto reject=[&](Evidence bad,Digest actual,Stop expected){Plan unused;const auto before=text;CHECK(Prepare(text,actual,bad,unused)==expected);CHECK(text==before);};
    auto bad=e;bad.enabled=false;reject(bad,e.moduleId,Stop::Disabled);
    bad=e;bad.syntheticOwned=false;reject(bad,e.moduleId,Stop::Scope);
    for(unsigned mode{};mode<5;++mode){bad=e;if(mode==0)bad.producersQuiescent=false;if(mode==1)bad.writable=false;if(mode==2)bad.executable=true;if(mode==3)bad.base|=1;if(mode==4)bad.generation=0;reject(bad,e.moduleId,Stop::Mapping);}
    auto wrong=e.moduleId;wrong[0]^=1;reject(e,wrong,Stop::Identity);
    bad=e;bad.moduleId={};reject(bad,{},Stop::Identity);
    bad=e;bad.segmentHash[0]^=1;reject(bad,e.moduleId,Stop::SegmentHash);
    for(unsigned mode{};mode<5;++mode){bad=e;if(mode==0)bad.windowSize=0;if(mode==1)bad.windowStart=257;if(mode==2)bad.windowSize=SIZE_MAX;if(mode==3)bad.windowHash[0]^=1;if(mode==4){bad.windowStart=0;bad.windowSize=7;}reject(bad,e.moduleId,Stop::Window);}
    bad=e;bad.original^=1;reject(bad,e.moduleId,Stop::Instruction);
    bad=e;bad.call=SIZE_MAX;reject(bad,e.moduleId,Stop::Size);
    bad=e;bad.stub=bad.call;reject(bad,e.moduleId,Stop::Size);
    bad=e;bad.base=UINTPTR_MAX-3;reject(bad,e.moduleId,Stop::Size);
    CHECK(Apply(text,e,p)==Stop::None);CHECK(text!=original);CHECK(Word(text,e.call)==p.replacement);
    bad=e;bad.producersQuiescent=false;const auto installed=text;CHECK(Rollback(text,bad,p)==Stop::Mapping&&text==installed);
    text[220]^=1;const auto changed=text;CHECK(Rollback(text,e,p)==Stop::Conflict&&text==changed);
    text=installed;CHECK(Rollback(text,e,p)==Stop::None&&text==original);
    CHECK(Prepare(text,e.moduleId,e,p)==Stop::None);text[220]^=1;const auto changedBefore=text;CHECK(Apply(text,e,p)==Stop::Conflict&&text==changedBefore);
    text=original;Put(text,e.tail,0x94000000);bad=e;bad.segmentHash=Sha256(text);reject(bad,e.moduleId,Stop::Tail);
    text=original; e.site=Evidence::Site::EntrySub32; e.call=64; e.target=68; e.windowStart=64;
    e.original=0xd10083ffu; Put(text,e.call,e.original); Put(text,e.tail-4,e.original);
    CHECK(Branch(e.base+e.tail,e.base+e.target,false,tail));Put(text,e.tail,tail);
    e.segmentHash=Sha256(text);e.windowHash=Sha256(std::span(text).subspan(e.windowStart,e.windowSize));
    CHECK(Prepare(text,e.moduleId,e,p)==Stop::None);const auto entryOriginal=text;
    CHECK(Apply(text,e,p)==Stop::None);CHECK(Target(Word(text,e.call),e.base+e.call,false,target)&&target==e.base+e.stub);
    CHECK(Rollback(text,e,p)==Stop::None&&text==entryOriginal);
    bad=e;bad.target+=4;reject(bad,e.moduleId,Stop::Instruction);
    bad=e;bad.original=0xd503201f;reject(bad,e.moduleId,Stop::Instruction);
    std::printf("PASS host SHA256 known vectors, branch boundaries/relocations, identity/window/mapping/bytes fail-closed, staging and conflict-aware rollback (%u assertions)\n",checks);
}

#ifdef __aarch64__
extern "C" uint8_t fixture_begin[],fixture_end[],fixture_caller[],fixture_call[],fixture_after[],fixture_other_caller[],fixture_original[],fixture_resume[],fixture_proxy[],fixture_tap[],fixture_tail[],fixture_control_literal[];
struct alignas(16) FullState {
    uint64_t x[31]{},sp{},nzcv{},fpcr{},fpsr{},tls{},tlsro{},padding{};
    std::array<uint8_t,512> q{};
};
static_assert(offsetof(FullState,q)==304&&sizeof(FullState)==816);
struct alignas(64) Slot {
    std::atomic<uint32_t> state{0};uint32_t reserved{};
    uint64_t expectedLr{},generation{},x0{},lr{},sp{},pc{},tls{},route{};
};
static_assert(sizeof(Slot)==128&&offsetof(Slot,x0)==24&&offsetof(Slot,tls)==56);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
struct Control { Slot *slot{};FullState *image{};void(*callback)(){}; std::atomic<uint64_t> got{}; };
static_assert(offsetof(Control,image)==8&&offsetof(Control,callback)==16&&offsetof(Control,got)==24);
extern "C" void invoke_fixture(void *caller,const FullState *seed);
FullState seed,baselineImage,observedImage;
Slot slot;
Control control;
std::jmp_buf jumpTarget;
volatile int jumpCount{},lateReason{};
uint64_t lateHelperLr{},nextGeneration{};
uintptr_t modeledHostSp=0x123456780000;
extern "C" __attribute__((noinline,noreturn)) void fixture_later_boundary(uint64_t reason) { lateReason=int(reason);jumpCount=jumpCount+1;std::longjmp(jumpTarget,1); }
extern "C" [[noreturn]] void fixture_late_break() { lateHelperLr=uintptr_t(__builtin_return_address(0));fixture_later_boundary(674); }
uint64_t Tls(){uint64_t result;asm volatile("mrs %0,tpidr_el0":"=r"(result));return result;}
void RestoreFp(){asm volatile("msr fpcr,%0\nmsr fpsr,%1"::"r"(uint64_t(0)),"r"(uint64_t(0)));}
struct Mapping {
    uint8_t *data{};size_t size{},allocation{};bool writable{true},executable{},quiescent{true};
    Mapping(){size=size_t(uintptr_t(fixture_end)-uintptr_t(fixture_begin));allocation=(size+4095)&~size_t(4095);data=static_cast<uint8_t *>(mmap(nullptr,allocation,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));CHECK(data!=MAP_FAILED);std::memcpy(data,fixture_begin,size);auto pointer=uintptr_t(&control);std::memcpy(data+Offset(fixture_control_literal),&pointer,8);}
    size_t Offset(const uint8_t *label)const{return size_t(uintptr_t(label)-uintptr_t(fixture_begin));}
    void *Caller()const{return data+Offset(fixture_caller);}
    std::span<uint8_t> Bytes(){return {data,size};}
    void Seal(){CHECK(quiescent&&writable);__builtin___clear_cache(reinterpret_cast<char *>(data),reinterpret_cast<char *>(data+size));CHECK(mprotect(data,allocation,PROT_READ|PROT_EXEC)==0);writable=false;executable=true;}
    void Stage(){CHECK(quiescent&&executable);CHECK(mprotect(data,allocation,PROT_READ|PROT_WRITE)==0);writable=true;executable=false;}
    ~Mapping(){CHECK(quiescent);CHECK(munmap(data,allocation)==0);}
};
Evidence Describe(Mapping &m,uint64_t generation=1){Evidence e;e.site=Evidence::Site::EntrySub32;e.enabled=e.syntheticOwned=true;e.producersQuiescent=m.quiescent;e.writable=m.writable;e.executable=m.executable;e.base=uintptr_t(m.data);e.generation=generation;e.moduleId=Hash("owned-fixture-not-sdk");e.segmentHash=Sha256(m.Bytes());e.call=m.Offset(fixture_original);e.target=m.Offset(fixture_resume);e.stub=m.Offset(fixture_tap);e.tail=m.Offset(fixture_tail);e.original=Word(m.Bytes(),e.call);e.windowStart=e.call;e.windowSize=32;e.windowHash=Sha256(m.Bytes().subspan(e.windowStart,e.windowSize));return e;}
__attribute__((noinline)) void Invoke(void *caller){invoke_fixture(caller,&seed);}
void ResetSlot(uint64_t expected,uint64_t){slot.expectedLr=expected;slot.generation=++nextGeneration;slot.x0=slot.lr=slot.sp=slot.pc=slot.tls=0;slot.state.store(0,std::memory_order_release);}
void CompareState(){
    if (std::memcmp(&baselineImage,&observedImage,sizeof(FullState))!=0) {
        const auto *a=reinterpret_cast<const uint64_t *>(&baselineImage), *b=reinterpret_cast<const uint64_t *>(&observedImage);
        for(size_t i{};i<sizeof(FullState)/8;++i) if(a[i]!=b[i]) std::fprintf(stderr,"state offset%zu baseline=%s observed=%s\n",i*8,Hex(a[i]).c_str(),Hex(b[i]).c_str());
    }
    CHECK(std::memcmp(&baselineImage,&observedImage,sizeof(FullState))==0);CHECK(observedImage.x[0]==seed.x[0]);CHECK(observedImage.sp%16==0);}
void InvokeNoReturn(void *caller){if(setjmp(jumpTarget)==0){Invoke(caller);CHECK(false);}RestoreFp();CHECK(lateReason==674);CHECK(modeledHostSp==0x123456780000);}

void Aarch64Checks(const std::filesystem::path &output){
    for(unsigned i{};i<31;++i)seed.x[i]=0xa500000000000000ULL+i*0x10101;
    for(size_t i{};i<seed.q.size();++i)seed.q[i]=uint8_t(i*37+11);
    seed.nzcv=0xa0000000;seed.fpcr=0xc00000;seed.fpsr=0x800001f;
    control.slot=&slot;control.image=&baselineImage;control.callback=nullptr;
    for(unsigned placement{};placement<3;++placement){
        Mapping map;control.got.store(uintptr_t(map.data+map.Offset(fixture_original)));const auto before=std::vector<uint8_t>(map.Bytes().begin(),map.Bytes().end());auto evidence=Describe(map,placement+1);Plan plan;
        ResetSlot(uintptr_t(map.data+map.Offset(fixture_after)),placement+1);seed.x[0]=0xfedcba9876543210;
        CHECK(Prepare(map.Bytes(),evidence.moduleId,evidence,plan)==Stop::None);map.Seal();
        control.image=&baselineImage;Invoke(map.Caller());CHECK(slot.state.load()==0);
        CHECK(baselineImage.x[30]==slot.expectedLr);
        const auto returningBaseline=baselineImage;
        control.callback=fixture_late_break;InvokeNoReturn(map.Caller());
        const auto nonreturnBaseline=baselineImage;baselineImage=returningBaseline;
        control.callback=nullptr;map.Stage();CHECK(Apply(map.Bytes(),evidence,plan)==Stop::None);map.Seal();
        control.image=&observedImage;Invoke(map.Caller());CompareState();
        CHECK(slot.state.load(std::memory_order_acquire)==2&&slot.x0==seed.x[0]&&slot.lr==baselineImage.x[30]&&slot.sp==baselineImage.sp+32&&slot.pc==uintptr_t(map.data+map.Offset(fixture_original))&&slot.tls==Tls()&&slot.route==1);
        const auto first=slot.x0;seed.x[0]=0x7777777777777777;Invoke(map.Caller());CHECK(slot.x0==first);seed.x[0]=0xfedcba9876543210;
        for(uint32_t state : {1u,2u,3u,4u}){slot.state=state;slot.x0=0x1122334455667788;control.image=&observedImage;Invoke(map.Caller());CompareState();CHECK(slot.x0==0x1122334455667788&&slot.state==state);}
        ResetSlot(slot.expectedLr,placement+1);control.slot=nullptr;Invoke(map.Caller());CompareState();CHECK(slot.state==0);control.slot=&slot;
        auto expected=slot.expectedLr;
        slot.state=0;Invoke(map.data+map.Offset(fixture_other_caller));CHECK(slot.state==0);
        slot.expectedLr+=4;Invoke(map.Caller());CompareState();CHECK(slot.state==0);slot.expectedLr=expected;
        control.callback=fixture_late_break;control.image=&observedImage;baselineImage=nonreturnBaseline;
        for(unsigned i{};i<40;++i){ResetSlot(expected,placement+1);const auto count=jumpCount;InvokeNoReturn(map.Caller());CHECK(jumpCount==count+1&&lateHelperLr!=expected&&slot.state==2&&slot.x0==seed.x[0]&&slot.lr==expected);CompareState();}
        control.callback=nullptr;baselineImage=returningBaseline;map.Stage();CHECK(Rollback(map.Bytes(),evidence,plan)==Stop::None);CHECK(std::equal(before.begin(),before.end(),map.data));map.Seal();
        ResetSlot(expected,placement+1);control.image=&observedImage;Invoke(map.Caller());CompareState();CHECK(slot.state==0);
    }
    // Contending normal Linux fixture threads: original body performs no shared image writes.
    {Mapping map;control.got.store(uintptr_t(map.data+map.Offset(fixture_original)));auto evidence=Describe(map,4);Plan plan;ResetSlot(uintptr_t(map.data+map.Offset(fixture_after)),4);CHECK(Prepare(map.Bytes(),evidence.moduleId,evidence,plan)==Stop::None);CHECK(Apply(map.Bytes(),evidence,plan)==Stop::None);map.Seal();control.image=nullptr;control.callback=nullptr;
     std::array<uint64_t,8> tls{};std::atomic<unsigned> returned{};std::vector<std::thread> workers;map.quiescent=false;
     for(unsigned i{};i<8;++i)workers.emplace_back([&,i]{auto local=seed;local.x[0]=0x100+i;tls[i]=Tls();for(unsigned j{};j<100;++j){invoke_fixture(map.Caller(),&local);++returned;}});
     for(auto &worker:workers) { worker.join(); }
     map.quiescent=true;CHECK(returned==800&&slot.state.load(std::memory_order_acquire)==2);CHECK(slot.x0>=0x100&&slot.x0<0x108&&slot.tls==tls[slot.x0-0x100]);}
    // Compare elapsed time only inside this QEMU/Linux fixture, never game FPS.
    {Mapping map;control.got.store(uintptr_t(map.data+map.Offset(fixture_original)));auto evidence=Describe(map,4);Plan plan;
     ResetSlot(uintptr_t(map.data+map.Offset(fixture_after)),4);CHECK(Prepare(map.Bytes(),evidence.moduleId,evidence,plan)==Stop::None);
     map.Seal();control.image=nullptr;control.callback=nullptr;
     auto measure=[&]{std::array<double,5> samples{};for(auto &sample:samples){const auto begin=std::chrono::steady_clock::now();for(unsigned i{};i<10000;++i)Invoke(map.Caller());sample=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-begin).count()/10000;}std::sort(samples.begin(),samples.end());return samples;};
     const auto baseline=measure();map.Stage();CHECK(Apply(map.Bytes(),evidence,plan)==Stop::None);map.Seal();
     control.slot=nullptr;const auto disabled=measure();control.slot=&slot;slot.state=2;const auto full=measure();
     std::ofstream perf(output/"fixture-performance.json");
     perf<<"{\"scope\":\"qemu_linux_owned_fixture_only_not_FPS_or_device_cost\",\"loops_per_sample\":10000,\"samples\":5,\"baseline_ns\":["<<baseline[0]<<','<<baseline[2]<<','<<baseline[4]<<"],\"detour_disabled_ns\":["<<disabled[0]<<','<<disabled[2]<<','<<disabled[4]<<"],\"detour_full_ns\":["<<full[0]<<','<<full[2]<<','<<full[4]<<"]}\n";
    }
    // A real unchanged Diagnostics writer receives one entry record on a normal bridge thread.
    {Mapping map;control.got.store(uintptr_t(map.data+map.Offset(fixture_original)));auto evidence=Describe(map,5);Plan plan;ResetSlot(uintptr_t(map.data+map.Offset(fixture_after)),5);CHECK(Prepare(map.Bytes(),evidence.moduleId,evidence,plan)==Stop::None);CHECK(Apply(map.Bytes(),evidence,plan)==Stop::None);map.Seal();control.image=&observedImage;control.callback=fixture_late_break;seed.x[0]=0xfedcba9876543210;
     skyline::diagnostics::Config config;config.directory=(output/"diagnostics").string();config.sessionId="synthetic-entry-session";config.processRunId="synthetic-process";CHECK(skyline::diagnostics::Start(config));skyline::diagnostics::SetLaunchId("synthetic-launch",true);skyline::diagnostics::ConfigureResourceObservation(true,"owned_synthetic_fixture");
     const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);bool windowReady{};
     while(std::chrono::steady_clock::now()<deadline){skyline::diagnostics::Flush(5000);std::ifstream file(output/"diagnostics/diagnostics-0.jsonl");std::string bytes{std::istreambuf_iterator<char>(file),{}};if(bytes.find("RESOURCE_GPU_SAMPLE")!=std::string::npos){windowReady=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(2));}CHECK(windowReady);
     std::atomic<bool> consumed{},accepted{};std::thread bridge([&]{while(slot.state.load(std::memory_order_acquire)!=2)std::this_thread::yield();auto x0=Hex(slot.x0),lr=Hex(slot.lr),sp=Hex(slot.sp),pc=Hex(slot.pc),tls=Hex(slot.tls),generation=std::to_string(slot.generation);const std::pair<std::string_view,std::string_view> fields[]{{"observation_scope","executed_aarch64_fixture_not_game"},{"entry_x0",x0},{"original_caller_lr",lr},{"entry_sp",sp},{"authenticated_entry_site",pc},{"producer_tls",tls},{"generation",generation},{"thread_scope","linux_fixture_tls_not_pine_guest_thread"}};accepted=skyline::diagnostics::EmitFields("resource","SYNTHETIC_ABORT_ENTRY",fields);consumed=true;});
     InvokeNoReturn(map.Caller());CHECK(lateReason==674&&slot.x0!=uint64_t(lateReason));while(!consumed.load())std::this_thread::yield();bridge.join();CHECK(accepted);CHECK(skyline::diagnostics::Flush(5000));skyline::diagnostics::Stop();control.callback=nullptr;
    }
    std::printf("PASS executed AArch64 fixture: raw entry X0/original LR, all X0-X30/Q0-Q31/SP/NZCV/FPCR/FPSR/TLS equivalence; returning,120 longjmps, disabled/full/reentrant-state, selected LR,3 mapped bases,800 contending returns,unchanged Diagnostics normal-thread bridge (%u assertions)\n",checks);
}
#endif
int main(int argc,char **argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    HostChecks();
#ifdef __aarch64__
    CHECK(argc==2);std::filesystem::create_directories(argv[1]);Aarch64Checks(argv[1]);
    std::printf("{\"architecture\":\"aarch64\",\"scope\":\"executed_owned_synthetic_fixture_not_android_or_game\",\"sdk_authenticated\":false,\"production_connected\":false,\"checks\":%u}\n",checks);
#else
    (void)argc;(void)argv;std::printf("{\"architecture\":\"host\",\"aarch64_execution\":false,\"production_connected\":false,\"checks\":%u}\n",checks);
#endif
}
