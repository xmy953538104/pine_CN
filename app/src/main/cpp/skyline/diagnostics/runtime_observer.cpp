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
std::atomic<uint64_t> threadSerial{};
std::atomic<uint64_t> optionEpoch{}; // odd is on; every configure advances, no ABA/reuse
thread_local ScopedLoad *currentLoad{};
struct Registry {std::mutex mutex;std::array<std::weak_ptr<Module>,16> modules;};
// One bounded process-lifetime registry; avoids static teardown racing the sink.
Registry &Get(){static Registry *registry=new Registry;return *registry;}
std::string Hex(uint64_t value){char text[19];std::snprintf(text,sizeof(text),"0x%016llx",static_cast<unsigned long long>(value));return text;}
std::string DigestText(const image::Digest &value){std::string text;for(auto byte:value){char b[3];std::snprintf(b,3,"%02x",byte);text+=b;}return text;}
bool DrainModules() noexcept {
    try {
        bool complete=true;
        std::array<std::shared_ptr<Module>,16> modules;
        {auto&r=Get();std::unique_lock lock(r.mutex,std::try_to_lock);if(!lock)return false;
         for(size_t i{};i<modules.size();++i)modules[i]=r.modules[i].lock();}
        for(auto&module:modules){
            if(!module||!module->publishedReady.load(std::memory_order_acquire))continue;
            if(ownerEpoch.load(std::memory_order_acquire)!=module->input.owner.runtimeOwnerEpoch||optionEpoch.load(std::memory_order_acquire)!=module->optionEpoch){
                module->active=0;module->stopReason=uint64_t(StopReason::OwnerChanged);
                module->identityExported=true;module->eventPublished=true;continue;
            }
            if(!module->identityExported){
                auto base=Hex(module->moduleBase),ro=Hex(module->roBase),build=DigestText(module->input.buildId);
                std::string preHashes,loadedHashes,layout;
                for(const auto&segment:module->input.segments){preHashes+=DigestText(segment.computedHash)+";";layout+=Hex(segment.va)+":"+Hex(segment.size)+";";}
                for(size_t i{};i<module->input.windowCount;++i){const auto&w=module->loadedWindows[i];loadedHashes+=Hex(w.va)+":"+DigestText(w.hash)+";";}
                const auto installedHash=module->observerInstalled?DigestText(module->observerPatch.stubHash):"unavailable_not_installed";
                const auto installedWord=module->observerInstalled?Hex(module->observerPatch.installed):"unavailable_not_installed";
                const std::pair<std::string_view,std::string_view> fields[]{
                    {"observation_phase","post_nce_pre_guest"},{"runtime_connected","false"},
                    {"source_domain",module->domain==Domain::Guest?"pine_loader_mapping":"controlled_synthetic"},
                    {"module","sdk.nso"},{"build_id",build},{"text_base",base},{"rodata_base",ro},
                    {"preimage_hash_scope","decompressed_unpadded_before_nce"},
                    {"loaded_window_scope","actual_loader_mapping_after_nce_before_observer_patch"},
                    {"loaded_windows_verified","true"},{"instruction_cache_published",module->cachePublished?"true":"false"},
                    {"guest_call_observed","false"},{"interception_status",module->observerInstalled?"controlled_only_installed":"execution_safety_gate_closed"},
                    {"live_got","unavailable_not_read"},{"preimage_segment_hashes",preHashes},{"preimage_layout_va_size",layout},
                    {"post_nce_preobserver_window_hashes",loadedHashes},{"installed_stub_hash",installedHash},{"installed_callsite_word",installedWord}};
                module->identityExported=EmitForModuleOwner(module->input.owner,"module_runtime","MODULE_RUNTIME_IDENTITY",fields,&optionEpoch,module->optionEpoch);
                if(!module->identityExported)complete=false;
            }
            if(module->eventPublished||!module->observerInstalled)continue;
            std::array<std::shared_ptr<GuestSlot>,256> slots;
            {std::unique_lock lock(module->threadMutex,std::try_to_lock);if(!lock){complete=false;continue;}slots=module->threads;}
            for(auto&slot:slots){
                if(!slot||slot->state.load(std::memory_order_acquire)!=2)continue;
                auto x0=Hex(slot->x0),lr=Hex(slot->lr),sp=Hex(slot->sp),pc=Hex(module->callPc),observer=Hex(slot->observerPc);
                auto tid=std::to_string(slot->guestThread),generation=std::to_string(slot->threadGeneration),build=DigestText(module->input.buildId);
                std::string_view classification="UNCLASSIFIED",reason="caller_or_assertion_mismatch";
                if(module->domain==Domain::ControlledSynthetic&&slot->lr==module->callPc+4){
                    if(slot->x0==module->noCallbackPc){classification="NO_CALLBACK";reason="controlled_verified_assertion_and_callsite";}
                    else if(slot->x0==module->insufficientPc){classification="INSUFFICIENT_AFTER_RETURN";reason="controlled_verified_assertion_and_callsite";}
                }
                if(module->domain==Domain::Guest)reason="continuous_runtime_identity_not_qualified";
                const std::pair<std::string_view,std::string_view> fields[]{
                    {"runtime_connected",module->domain==Domain::Guest?"true":"false"},
                    {"source_domain",module->domain==Domain::Guest?"pine_guest":"controlled_synthetic"},
                    {"capture_phase","post_callsite_bl_pre_original_plt"},{"outer_abort_entry_observed","false"},
                    {"classification",classification},{"classification_reason",reason},{"guest_x0",x0},{"guest_lr",lr},
                    {"guest_sp",sp},{"guest_callsite_pc",pc},{"observer_pc",observer},
                    {"guest_thread_id",tid},{"guest_thread_generation",generation},{"module_build_id",build},
                    {"identity_scope","post_nce_load_verified_not_continuous"},{"lr_source","hardware_redirected_bl"},
                    {"record_thread_role","diagnostics_consumer_not_guest"},{"capture_clock","unavailable"},
                    {"original_plt","traversal_preserved_target_not_read"}};
                if(EmitForModuleOwner(module->input.owner,"module_runtime","NVN_ABORT_CALLSITE_OBSERVED",fields,&optionEpoch,module->optionEpoch)){
                    module->eventPublished=true;slot->state.store(3,std::memory_order_release);break;
                }
                complete=false;break; // retry admission on worker, never on Guest
            }
        }
        return complete;
    }catch(...){return false;}
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
ProcessObservation::ProcessObservation(const Profile &p,Domain d,bool enabled) noexcept:profile(&p),domain(d),controlledExecution(enabled){}
#endif
ProcessObservation::~ProcessObservation(){Invalidate(StopReason::ProcessExit);}
void ProcessObservation::Invalidate(StopReason reason) noexcept {
    admission.exchange(2,std::memory_order_acq_rel);
    closed.store(true,std::memory_order_release);
    if(auto*m=published.load(std::memory_order_acquire)){m->stopReason.store(uint64_t(reason),std::memory_order_relaxed);m->active.store(0,std::memory_order_release);}
}
void ProcessObservation::BeforeMappingMutation(uint64_t start,uint64_t size) noexcept {
    if(auto*m=published.load(std::memory_order_acquire);m&&m->active.load(std::memory_order_acquire)==1)
        if(Overlap(start,size,m->moduleBase,m->textSize)||Overlap(start,size,m->roBase,m->roSize))Invalidate(StopReason::MappingChanged);
}
bool ProcessObservation::Validated()const noexcept {auto*m=published.load(std::memory_order_acquire);return !closed.load(std::memory_order_acquire)&&admission.load(std::memory_order_acquire)==1&&m&&m->active.load(std::memory_order_acquire)==1&&m->optionEpoch==optionEpoch.load(std::memory_order_acquire)&&m->input.owner.runtimeOwnerEpoch==ownerEpoch.load(std::memory_order_acquire);}
ScopedLoad::ScopedLoad(ProcessObservation &p,const image::Record *record,std::string_view name) noexcept {
    configurationEpoch=optionEpoch.load(std::memory_order_acquire);
    if(!(configurationEpoch&1))return;
    if(name.empty()&&record)name=record->moduleName.data();
    if(name!="sdk.nso")return;
    if(p.loadsSealed.load(std::memory_order_acquire)){p.Invalidate(StopReason::LateLoad);return;}
    if(p.closed.load(std::memory_order_acquire))return;
    if(!record){p.Invalidate(StopReason::IdentityMismatch);return;}
    try{loadLock=std::unique_lock<std::mutex>(p.loadMutex,std::try_to_lock);}catch(...){p.Invalidate(StopReason::Capacity);return;}
    if(!loadLock||p.published.load(std::memory_order_acquire)){p.Invalidate(StopReason::IdentityMismatch);return;}
    process=&p;input=record;previous=currentLoad;currentLoad=this;
}
ScopedLoad::~ScopedLoad(){if(process){currentLoad=previous;if(!observed)process->Invalidate(StopReason::IdentityMismatch);}}
bool ScopedLoad::Active() noexcept {return currentLoad!=nullptr;}
size_t ScopedLoad::ReservationBytes() noexcept {
    auto*load=currentLoad;if(!load||!load->process)return 0;auto&p=*load->process;
    // Production remains closed: actual NCE lifetime/integrity qualification
    // has not passed. Only the explicitly typed controlled test build may emit.
    if(!p.controlledExecution||p.domain!=Domain::ControlledSynthetic||load->configurationEpoch!=optionEpoch.load(std::memory_order_acquire)||!IdentityRequested()||p.loadsSealed.load()||p.closed.load())return 0;
    const auto&source=*load->input;const auto&expected=*p.profile;
    if(source.buildId!=expected.preimage.buildId||source.windowCount!=expected.preimage.windowCount)return 0;
    for(size_t i{};i<3;++i)if(source.segments[i].computedHash!=expected.preimage.segmentHashes[i]||
        source.segments[i].va!=expected.segments[i].first||source.segments[i].size!=expected.segments[i].second)return 0;
    for(size_t i{};i<source.windowCount;++i)if(source.windows[i].hash!=expected.preimage.windows[i].hash||
        source.windows[i].va!=expected.preimage.windows[i].va||source.windows[i].size!=expected.preimage.windows[i].size)return 0;
    return load->reservation=4096;
}
void ScopedLoad::RecordPatchUsage(size_t bytes) noexcept {if(currentLoad)currentLoad->patchUsage=bytes;}
std::shared_ptr<GuestSlot> ProcessObservation::AttachThread(uint64_t id) noexcept {
    try{
        if(!Validated()||!storage->observerInstalled)return {};
        auto&module=*storage;std::unique_lock lock(module.threadMutex,std::try_to_lock);if(!lock)return {};
        for(auto&entry:module.threads)if(!entry){
            const auto serial=threadSerial.fetch_add(1,std::memory_order_relaxed)+1;if(!serial)return {};
            auto slot=std::make_shared<GuestSlot>();slot->optionGate=&optionEpoch;slot->expectedOption=module.optionEpoch;
            slot->ownerGate=&ownerEpoch;slot->expectedOwner=module.input.owner.runtimeOwnerEpoch;slot->moduleActive=&module.active;
            slot->guestThread=id;slot->threadGeneration=serial;entry=slot;return slot;
        }
    }catch(...){}
    return {};
}
void ScopedLoad::Mapped(std::span<uint8_t> text,std::span<const uint8_t> ro,uint64_t base,uint64_t reservation,size_t reservationSize,bool cachePublished,std::span<uint8_t> patch) noexcept {
    auto *load=currentLoad;if(!load||!load->process)return;auto &p=*load->process;load->observed=true;
    try {
        if(load->configurationEpoch!=optionEpoch.load(std::memory_order_acquire)){p.Invalidate(StopReason::Disabled);return;}
        if(!IdentityRequested()||p.closed.load()||p.loadsSealed.load())return;
        if(p.published.load())throw StopReason::IdentityMismatch;
        const auto &expected=*p.profile;auto source=*load->input;
        ModuleOwner current;if(!TryModuleOwner(current)||current!=source.owner)throw StopReason::OwnerChanged;
        source.moduleBase=base;source.loadReservationBase=reservation;source.loadReservationSize=reservationSize;
        if(!cachePublished||image::VerifyPreimage(source,expected.preimage)!=image::PreimageMatch::MatchOnly)throw StopReason::IdentityMismatch;
        for(size_t i{};i<3;++i)if(source.segments[i].va!=expected.segments[i].first||source.segments[i].size!=expected.segments[i].second)throw StopReason::IdentityMismatch;
        if(!base||!reservationSize||UINT64_MAX-reservation<reservationSize||base<reservation||base>=reservation+reservationSize||text.size()<source.segments[0].size||ro.size()<source.segments[1].size)throw StopReason::IdentityMismatch;
        for(const auto&segment:source.segments){
            if(UINT64_MAX-base<segment.va)throw StopReason::IdentityMismatch;
            const auto start=base+segment.va,end=reservation+reservationSize;
            if(start> end||segment.size>end-start)throw StopReason::IdentityMismatch;
        }
        auto module=std::make_shared<Module>();module->input=source;module->moduleBase=base;module->textSize=source.segments[0].size;module->roBase=base+source.segments[1].va;module->roSize=source.segments[1].size;module->optionEpoch=load->configurationEpoch;module->domain=p.domain;module->cachePublished=cachePublished;
        for(size_t i{};i<expected.preimage.windowCount;++i){const auto&w=expected.preimage.windows[i];std::span<const uint8_t> bytes;
            if(w.va<module->textSize&&w.size<=module->textSize-w.va)bytes=text.subspan(w.va,w.size);
            else if(w.va>=source.segments[1].va&&w.va-source.segments[1].va<=module->roSize&&w.size<=module->roSize-(w.va-source.segments[1].va))bytes=ro.subspan(w.va-source.segments[1].va,w.size);
            else throw StopReason::IdentityMismatch;
            if(image::Sha256(bytes)!=w.hash)throw StopReason::IdentityMismatch;
            auto&copy=module->loadedWindows[i];copy.va=w.va;copy.size=w.size;copy.hash=w.hash;std::copy(bytes.begin(),bytes.end(),copy.bytes.begin());}
        if(p.closed.load()||p.loadsSealed.load()||module->optionEpoch!=optionEpoch.load())return;
        module->callPc=base+expected.callVa;module->pltPc=base+expected.pltVa;
        module->noCallbackPc=base+expected.noCallbackVa;module->insufficientPc=base+expected.insufficientVa;
        if(load->reservation){
            if(patch.size()<load->reservation||load->patchUsage>patch.size()-load->reservation)throw StopReason::Capacity;
            auto stub=patch.last(load->reservation);
            const auto stubPc=reservation+patch.size()-load->reservation;
            if(!nce::InstallCallsiteObservation(text,base,expected.callVa,expected.pltVa,expected.originalBl,stub,stubPc,
                  !p.loadsSealed.load()&&!p.closed.load()&&module->domain==Domain::ControlledSynthetic,module->observerPatch))throw StopReason::IdentityMismatch;
            __builtin___clear_cache(reinterpret_cast<char*>(stub.data()),reinterpret_cast<char*>(stub.data()+stub.size()));
            __builtin___clear_cache(reinterpret_cast<char*>(stubPc),reinterpret_cast<char*>(stubPc+stub.size()));
            __builtin___clear_cache(reinterpret_cast<char*>(text.data()),reinterpret_cast<char*>(text.data()+text.size()));
            __builtin___clear_cache(reinterpret_cast<char*>(base),reinterpret_cast<char*>(base+text.size()));
            module->observerInstalled=true;
        }
        if(!Register(module))throw StopReason::Capacity;
        p.storage=module;p.published.store(module.get(),std::memory_order_release);
        uint64_t pending=0;
        const bool admitted=p.admission.compare_exchange_strong(pending,1,std::memory_order_acq_rel);
#ifdef PINE_DIAG_RUNTIME_TEST
        if(beforeActivation)beforeActivation(p);
#endif
        uint64_t notYetActive=2;
        if(admitted)module->active.compare_exchange_strong(notYetActive,1,std::memory_order_acq_rel);
        else module->active.store(0,std::memory_order_release);
        if(p.closed.load(std::memory_order_acquire))module->active.store(0,std::memory_order_release);
        module->publishedReady.store(1,std::memory_order_release);
    }catch(StopReason reason){p.Invalidate(reason);}catch(...){p.Invalidate(StopReason::IdentityMismatch);}
}
}
