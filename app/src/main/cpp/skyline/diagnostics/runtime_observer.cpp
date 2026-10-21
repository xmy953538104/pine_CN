// SPDX-License-Identifier: MPL-2.0
#include "runtime_observer.h"
#include "runtime_bridge.h"
#include "diagnostics.h"
#include <algorithm>
#include <mutex>
#include <cstdio>

namespace skyline::diagnostics::runtime {
namespace {
constexpr image::Digest D(const char *hex) {
    image::Digest out{};
    auto nibble=[](char c){return c<='9'?c-'0':c-'a'+10;};
    for(size_t i{};i<32;++i)out[i]=uint8_t(nibble(hex[i*2])*16+nibble(hex[i*2+1]));
    return out;
}
std::atomic<uint64_t> optionEpoch{}; // odd is on; every configure advances, no ABA/reuse
thread_local ScopedLoad *currentLoad{};
struct Registry {std::mutex mutex;std::array<std::weak_ptr<Module>,16> modules;};
// One bounded process-lifetime registry; avoids static teardown racing the sink.
Registry &Get(){static Registry *registry=new Registry;return *registry;}
std::string Hex(uint64_t value){char text[19];std::snprintf(text,sizeof(text),"0x%016llx",static_cast<unsigned long long>(value));return text;}
std::string DigestText(const image::Digest &value){std::string text;for(auto byte:value){char b[3];std::snprintf(b,3,"%02x",byte);text+=b;}return text;}
void DrainModules() noexcept {
    try {
        std::array<std::shared_ptr<Module>,16> modules;
        {auto &r=Get();std::unique_lock lock(r.mutex,std::try_to_lock);if(!lock)return;
         for(size_t i{};i<modules.size();++i)modules[i]=r.modules[i].lock();}
        for(auto &module:modules)if(module&&module->publishedReady.load(std::memory_order_acquire)&&!module->identityExported){
            ModuleOwner current;
            if(!TryModuleOwner(current))continue;
            if(current!=module->input.owner){module->active=0;module->stopReason=uint64_t(StopReason::OwnerChanged);module->identityExported=true;continue;}
            auto base=Hex(module->moduleBase),ro=Hex(module->roBase),build=DigestText(module->input.buildId);
            const std::pair<std::string_view,std::string_view> fields[]{
                {"observation_phase","post_nce_pre_guest"},{"runtime_connected","false"},
                {"source_domain",module->domain==Domain::Guest?"pine_loader_mapping":"controlled_synthetic"},
                {"module","sdk.nso"},{"build_id",build},{"text_base",base},{"rodata_base",ro},
                {"preimage_hash_scope","decompressed_unpadded_before_nce"},
                {"loaded_window_scope","actual_loader_host_mapping_after_nce_before_guest"},
                {"loaded_windows_verified","true"},{"instruction_cache_published",module->cachePublished?"true":"false"},
                {"guest_call_observed","false"},{"interception_status","execution_safety_gate_closed"},
                {"live_got","unavailable_not_read"}};
            if(EmitForModuleOwner(module->input.owner,"module_runtime","MODULE_RUNTIME_IDENTITY",fields))module->identityExported=true;
        }
    }catch(...){}
}
bool Register(const std::shared_ptr<Module> &module){auto&r=Get();std::unique_lock lock(r.mutex,std::try_to_lock);if(!lock)return false;for(auto&w:r.modules)if(w.expired()){w=module;return true;}return false;}
bool Overlap(uint64_t a,uint64_t n,uint64_t b,uint64_t m){return n&&m&&(UINT64_MAX-a<n||UINT64_MAX-b<m||(a<b+m&&b<a+n));}
}
const Profile &SdkProfile() noexcept {
    static constexpr Profile profile{
      {.buildId=D("d95547528dcd684e2813ec83d338a47223c76865000000000000000000000000"),
       .segmentHashes={D("8eaaf1f5fb047cc835f6b4dffe360aa71f8ccef2a11386527a89a92a9baed65c"),D("b59bde31bd7efcb02b486a7fea6a56c3a38dd3decec45b5f2777b08da993653b"),D("82402de62d8ab964daa514da1d6c62d50bb4ca0589b59c69ffdb7b3180bdae2c")},
       .windows={image::ExpectedWindow{0x2d6464,128,D("85bb3248ca4704e51b7e426ee830e91aaac33d4bb438fc0e1d064acdb84a7c82")},
        {0x2d64c0,64,D("4a521e0341b9bb60ccf003f19ea1164af9cef67dc92b1b546dcd154534d06d06")},
        {0x3e97e0,16,D("8d8b48e4b8a60248361e5ef8fc447fb327a79d8e25338edafb85cb91512b2f4a")},
        {0x445d0,32,D("8a973a71259256589fb95889e9cb191c0ccf06098da41a17d00a9f653931bdd3")},
        {0x7a0371,76,D("998c383b4dc357dc4aacc5487ac3dc08d0a76c1991ded5a5ef13ac4bf6f7d957")},
        {0x7a032a,71,D("1c39331be628798a892dc35d6aa3e30facc4e1c8780e09e958493a28e1640c85")}},.windowCount=6},
      {{{0x2d6464,128},{0x2d64c0,64},{0x3e97e0,16},{0x445d0,32},{0x7a0371,76},{0x7a032a,71}}},
      {{{0,0x40e870},{0x40f000,0x532155},{0x942000,0x60760}}},0x2d64e0,0x3e97e0,0x7a0371,0x7a032a};
    return profile;
}
void ConfigureIdentity(bool enabled) noexcept {
    auto old=optionEpoch.load(std::memory_order_relaxed);
    while(!optionEpoch.compare_exchange_weak(old,((old+2)&~uint64_t(1))|(enabled?1:0),std::memory_order_acq_rel)) {}
    const auto &profile=SdkProfile();
    image::Configure(enabled,{profile.requests.data(),profile.preimage.windowCount});
    drainCallback.store(enabled?&DrainModules:nullptr,std::memory_order_release);
}
bool IdentityRequested() noexcept{return optionEpoch.load(std::memory_order_acquire)&1;}
ProcessObservation::ProcessObservation() noexcept:profile(&SdkProfile()){}
#ifdef PINE_DIAG_RUNTIME_TEST
ProcessObservation::ProcessObservation(const Profile &p,Domain d) noexcept:profile(&p),domain(d){}
#endif
ProcessObservation::~ProcessObservation(){Invalidate(StopReason::ProcessExit);}
void ProcessObservation::Invalidate(StopReason reason) noexcept {
    closed.store(true,std::memory_order_release);
    if(auto*m=published.load(std::memory_order_acquire)){m->stopReason.store(uint64_t(reason),std::memory_order_relaxed);m->active.store(0,std::memory_order_release);}
}
void ProcessObservation::BeforeMappingMutation(uint64_t start,uint64_t size) noexcept {
    if(auto*m=published.load(std::memory_order_acquire);m&&m->active.load(std::memory_order_acquire))
        if(Overlap(start,size,m->moduleBase,m->textSize)||Overlap(start,size,m->roBase,m->roSize))Invalidate(StopReason::MappingChanged);
}
bool ProcessObservation::Validated()const noexcept {auto*m=published.load(std::memory_order_acquire);return m&&m->active.load(std::memory_order_acquire)&&m->optionEpoch==optionEpoch.load(std::memory_order_acquire)&&m->input.owner.runtimeOwnerEpoch==ownerEpoch.load(std::memory_order_acquire);}
ScopedLoad::ScopedLoad(ProcessObservation &p,const image::Record *record) noexcept {
    if(!IdentityRequested()||!record||std::string_view(record->moduleName.data())!="sdk.nso")return;
    if(p.loadsSealed.load(std::memory_order_acquire)||p.closed.load(std::memory_order_acquire))return;
    process=&p;input=record;previous=currentLoad;currentLoad=this;
}
ScopedLoad::~ScopedLoad(){if(process){currentLoad=previous;if(!observed)process->Invalidate(StopReason::IdentityMismatch);}}
bool ScopedLoad::Active() noexcept {return currentLoad!=nullptr;}
void ScopedLoad::Mapped(std::span<const uint8_t> text,std::span<const uint8_t> ro,uint64_t base,uint64_t reservation,size_t reservationSize,bool cachePublished) noexcept {
    auto *load=currentLoad;if(!load||!load->process)return;auto &p=*load->process;load->observed=true;
    try {
        if(!IdentityRequested()||p.closed.load()||p.loadsSealed.load())return;
        if(p.published.load())throw StopReason::IdentityMismatch;
        const auto &expected=*p.profile;auto source=*load->input;
        ModuleOwner current;if(!TryModuleOwner(current)||current!=source.owner)throw StopReason::OwnerChanged;
        source.moduleBase=base;source.loadReservationBase=reservation;source.loadReservationSize=reservationSize;
        if(!cachePublished||image::VerifyPreimage(source,expected.preimage)!=image::PreimageMatch::MatchOnly)throw StopReason::IdentityMismatch;
        for(size_t i{};i<3;++i)if(source.segments[i].va!=expected.segments[i].first||source.segments[i].size!=expected.segments[i].second)throw StopReason::IdentityMismatch;
        if(!base||UINT64_MAX-base<source.segments[1].va||text.size()<source.segments[0].size||ro.size()<source.segments[1].size)throw StopReason::IdentityMismatch;
        auto module=std::make_shared<Module>();module->input=source;module->moduleBase=base;module->textSize=source.segments[0].size;module->roBase=base+source.segments[1].va;module->roSize=source.segments[1].size;module->optionEpoch=optionEpoch.load();module->domain=p.domain;module->cachePublished=cachePublished;
        for(size_t i{};i<expected.preimage.windowCount;++i){const auto&w=expected.preimage.windows[i];std::span<const uint8_t> bytes;
            if(w.va<module->textSize&&w.size<=module->textSize-w.va)bytes=text.subspan(w.va,w.size);
            else if(w.va>=source.segments[1].va&&w.va-source.segments[1].va<=module->roSize&&w.size<=module->roSize-(w.va-source.segments[1].va))bytes=ro.subspan(w.va-source.segments[1].va,w.size);
            else throw StopReason::IdentityMismatch;
            if(image::Sha256(bytes)!=w.hash)throw StopReason::IdentityMismatch;
            auto&copy=module->loadedWindows[i];copy.va=w.va;copy.size=w.size;copy.hash=w.hash;std::copy(bytes.begin(),bytes.end(),copy.bytes.begin());}
        if(p.closed.load()||p.loadsSealed.load()||module->optionEpoch!=optionEpoch.load())return;
        if(!Register(module))throw StopReason::Capacity;
        p.storage=module;p.published.store(module.get(),std::memory_order_release);
        module->active.store(p.closed.load()?0:1,std::memory_order_release);
        module->publishedReady.store(1,std::memory_order_release);
    }catch(StopReason reason){p.Invalidate(reason);}catch(...){p.Invalidate(StopReason::IdentityMismatch);}
}
}
