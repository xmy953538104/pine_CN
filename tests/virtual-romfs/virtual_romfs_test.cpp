// SPDX-License-Identifier: MPL-2.0
// Real Backing + RomFileSystem + IStorage; only platform/IPC declarations are stubbed.
#include <vfs/prototype/virtual_romfs.h>
#include <vfs/prototype/romfs_detail.h>
#include <vfs/rom_filesystem.h>
#include <services/fssrv/IStorage.h>
#include <diagnostics/content.h>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <new>
#include <thread>

static std::atomic<size_t> allocationCount{};
void *operator new(size_t size) { ++allocationCount; if(auto*p=std::malloc(size?size:1))return p;throw std::bad_alloc{}; }
void operator delete(void*p)noexcept{std::free(p);}
void operator delete(void*p,size_t)noexcept{std::free(p);}
void *operator new[](size_t size){return ::operator new(size);}
void operator delete[](void*p)noexcept{std::free(p);}
void operator delete[](void*p,size_t)noexcept{std::free(p);}

namespace skyline::diagnostics {
bool Enabled()noexcept{return false;}
bool Verbose()noexcept{return false;}
void Emit(std::string_view,std::string_view,std::initializer_list<std::pair<std::string_view,std::string_view>>,std::string_view)noexcept{}
}
using namespace skyline;
using namespace skyline::vfs::prototype;
using OverlayResult=skyline::vfs::prototype::Result;
namespace d=skyline::vfs::prototype::detail;
struct IoFailure:std::runtime_error{IoFailure():std::runtime_error("controlled source IO failure") {}};
struct Changed:std::runtime_error{Changed():std::runtime_error("controlled source generation changed") {}};
struct Memory: vfs::Backing {
    std::vector<uint8_t> bytes;
    std::atomic<uint64_t> generation{},reads{},readBytes{};
    bool reliable{true},fail{},shortRead{},mutateAfterRead{};
    explicit Memory(std::vector<uint8_t> bytes):Backing({true,false,false},bytes.size()),bytes(std::move(bytes)){}
    bool HasReliableReadByteCount()const noexcept override{return reliable;}
    size_t ReadImpl(span<u8>out,size_t at)override {
        ++reads;if(fail)throw IoFailure{};d::CheckRange(at,out.size(),bytes.size());
        auto n=out.size()-(shortRead&&!out.empty()?1:0);if(n)std::memcpy(out.data(),bytes.data()+at,n);readBytes+=n;
        if(mutateAfterRead&&n){mutateAfterRead=false;Change(at);}return n;
    }
    void Change(size_t at){bytes.at(at)^=1;++generation;}
};
struct Guard:SourceGuard {
    std::shared_ptr<Memory> r,p;
    uint64_t rg,pg;
    mutable std::atomic<uint64_t> checks{};
    Guard(const std::shared_ptr<Memory>&r,const std::shared_ptr<Memory>&p):SourceGuard(r,p),r(r),p(p),rg(r->generation),pg(p->generation){}
    Guard(const std::shared_ptr<vfs::Backing>&view,const std::shared_ptr<Memory>&r,const std::shared_ptr<Memory>&p):SourceGuard(view,p),r(r),p(p),rg(r->generation),pg(p->generation){}
    void CheckUnchanged()const override{++checks;if(r->generation!=rg||p->generation!=pg)throw Changed{};}
};
struct AlreadyResolved: vfs::Backing {
    std::shared_ptr<Memory> source;
    explicit AlreadyResolved(std::shared_ptr<Memory>source):Backing({true,false,false},source->size),source(std::move(source)){}
    bool HasReliableReadByteCount()const noexcept override{return source->reliable;}
    size_t ReadImpl(span<u8>output,size_t offset)override{return source->ReadUnchecked(output,offset);}
};
struct Policy:IdentityVerifier {
    Binding approved;
    IdentityDecision decision{IdentityDecision::Approved};
    mutable uint64_t calls{};
    std::function<void()> duringVerify;
    IdentityDecision Verify(const IdentityObservation&o)const override {
        ++calls;
        assert(o.applicationId==approved.applicationId&&o.programId==approved.programId);
        assert(o.computedProgramSha256==approved.programSha256&&o.computedRomfsSha256==approved.romfsSha256);
        if(duringVerify)duringVerify();
        return decision;
    }
};
struct MutablePayload:PayloadSource {
    mutable std::vector<uint8_t> bytes;
    mutable uint64_t generation{};
    uint64_t fixedGeneration{};
    mutable uint64_t reads{};
    bool fail{},shortRead{},snapshot{true};
    mutable bool mutateAfterRead{};
    explicit MutablePayload(std::string text):bytes(text.begin(),text.end()){}
    uint64_t Size()const override{return bytes.size();}
    bool IsImmutableSnapshot()const noexcept override{return snapshot;}
    void CheckUnchanged()const override{if(generation!=fixedGeneration)throw Changed{};}
    size_t Read(uint64_t at,uint8_t*out,size_t count)const override{++reads;if(fail)throw IoFailure{};d::CheckRange(at,count,bytes.size());auto n=count-(shortRead&&count?1:0);if(n)std::memcpy(out,bytes.data()+at,n);if(mutateAfterRead&&n){mutateAfterRead=false;bytes[0]^=1;++generation;}return n;}
    void Change(){bytes.at(0)^=1;++generation;}
};
static std::vector<uint8_t> Load(const std::string&path) {
    std::ifstream in(path,std::ios::binary);assert(in);std::vector<uint8_t> result{std::istreambuf_iterator<char>(in),{}};assert(result.size()<=1024*1024);return result;
}
static std::string Hash(const std::vector<uint8_t>&bytes){return BytesSha256(bytes.data(),bytes.size());}
struct State {
    std::shared_ptr<Memory> base,program;
    std::shared_ptr<Guard> guard;
    Policy policy;
    Manifest manifest;
    Options options;
    State(std::vector<uint8_t>bytes,const std::vector<uint8_t>&programBytes):base(std::make_shared<Memory>(std::move(bytes))),program(std::make_shared<Memory>(programBytes)),guard(std::make_shared<Guard>(base,program)) {
        manifest.target={0x1000,0x1002,Hash(program->bytes),Hash(base->bytes)};policy.approved=manifest.target;
        options.enabled=true;options.applicationId=0x1000;options.programId=0x1002;options.effectiveProgram=program;options.identityVerifier=&policy;options.sourceGuard=guard;
    }
    void SetPayload(std::string path,std::string text,Operation operation=Operation::Replace){std::vector<uint8_t>bytes(text.begin(),text.end());manifest.entries.push_back({std::move(path),operation,bytes.size(),Hash(bytes),OwnPayload(std::move(bytes))});}
    OverlayResult Activate(){try{options.expectedManifestSha256=ManifestSha256(manifest,options.limits);}catch(const d::Invalid&){options.expectedManifestSha256=std::string(64,'a');}return CreateVirtualRomfs(base,manifest,options);}
};
static void Rejected(State&state,const std::string&reason) {
    auto before=state.base->bytes;auto result=state.Activate();assert(result.status==Status::Rejected&&result.view==state.base&&result.reason==reason);assert(state.base->bytes==before);
}
static void Throws(const std::function<void()>&f){try{f();assert(false);}catch(const std::exception&) {}}
static std::string Text(const std::shared_ptr<vfs::Backing>&file){std::string text(file->size,'\0');assert(file->ReadUnchecked({reinterpret_cast<uint8_t*>(text.data()),text.size()},0)==text.size());return text;}
static void Save(const std::string&path,const std::vector<uint8_t>&bytes){std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());assert(out);}

int main(int argc,char**argv) {
    assert(argc==3);std::string root=argv[1],evidence=argv[2];auto baseBytes=Load(root+"/base.romfs"),programBytes=Load(root+"/program.bin");
    assert(BytesSha256(nullptr,0)=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const uint8_t abc[]{'a','b','c'};assert(BytesSha256(abc,3)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::vector<uint8_t> million(1000000,'a');assert(Hash(million)=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    {State s(baseBytes,programBytes);s.options.enabled=false;auto reads=s.base->reads.load(),checks=s.guard->checks.load(),allocations=allocationCount.load();auto result=CreateVirtualRomfs(s.base,s.manifest,s.options);assert(result.status==Status::Disabled&&result.view==s.base);assert(s.base->reads==reads&&s.program->reads==0&&s.guard->checks==checks&&allocationCount==allocations);}
    {State s(Load(root+"/root-nested.romfs"),programBytes);assert(d::PathHash(0,"")==0x075bcd15&&d::PathHash(0,"c")==0xa83ade0b&&d::PathHash(24,"a")==0x683ade09&&d::PathHash(24,"d")==0x683ade0c);auto r=s.Activate();assert(r.status==Status::Activated);vfs::RomFileSystem fs(r.view);assert(Text(fs.OpenFile("c/a"))=="OK"&&Text(fs.OpenFile("c/d"))=="!");}
    {State s(Load(root+"/empty.romfs"),programBytes);s.SetPayload("a/b","",Operation::Add);auto r=s.Activate();assert(r.status==Status::Activated);vfs::RomFileSystem fs(r.view);assert(fs.DirectoryExists("a")&&fs.FileExists("a/b")&&Text(fs.OpenFile("a/b")).empty());}
    {State s(baseBytes,programBytes);auto resolved=std::make_shared<AlreadyResolved>(s.base);s.options.sourceGuard=std::make_shared<Guard>(resolved,s.base,s.program);s.options.expectedManifestSha256=ManifestSha256(s.manifest);auto r=CreateVirtualRomfs(resolved,s.manifest,s.options);assert(r.status==Status::Activated);vfs::RomFileSystem fs(r.view);assert(Text(fs.OpenFile("keep.txt"))=="original kept bytes");s.base->reliable=false;r=CreateVirtualRomfs(resolved,s.manifest,s.options);assert(r.status==Status::Rejected&&r.view==resolved&&r.reason=="source_byte_count_unreliable");}
    {State s(baseBytes,programBytes);s.options.identityVerifier=nullptr;Rejected(s,"trusted_identity_verifier_missing");assert(s.base->reads==0&&s.program->reads==0);}
    {State s(baseBytes,programBytes);s.options.sourceGuard.reset();Rejected(s,"same_source_guard_missing");assert(s.base->reads==0);}
    {State s(baseBytes,programBytes);auto other=std::make_shared<Memory>(baseBytes);s.options.sourceGuard=std::make_shared<Guard>(other,s.program);Rejected(s,"same_source_guard_missing");}
    {State s(baseBytes,programBytes);s.base->reliable=false;Rejected(s,"source_byte_count_unreliable");assert(s.base->reads==0);}
    {State s(baseBytes,programBytes);s.base->mode.write=true;Rejected(s,"source_not_readonly");}
    for(auto change:{0,1}){State s(baseBytes,programBytes);if(change)s.options.programId^=1;else s.options.applicationId^=1;Rejected(s,"target_identity_mismatch");assert(s.base->reads==0);}
    {State s(baseBytes,programBytes);s.manifest.target.programSha256=std::string(64,'0');Rejected(s,"program_hash_mismatch");assert(s.base->reads==0);}
    {State s(baseBytes,programBytes);s.manifest.target.romfsSha256=std::string(64,'0');Rejected(s,"romfs_hash_mismatch");}
    {State s(baseBytes,programBytes);s.options.expectedManifestSha256=std::string(64,'0');auto r=CreateVirtualRomfs(s.base,s.manifest,s.options);assert(r.reason=="manifest_hash_mismatch"&&s.base->reads==0);}
    for(auto decision:{IdentityDecision::Unknown,IdentityDecision::Rejected}){State s(baseBytes,programBytes);s.policy.decision=decision;Rejected(s,"identity_not_attested");}
    {State s(baseBytes,programBytes);s.policy.duringVerify=[&]{s.base->Change(0x200);};auto r=s.Activate();assert(r.status==Status::Rejected&&r.reason=="source_generation_changed"&&r.view==s.base);}
    {State s(baseBytes,programBytes);s.base->fail=true;bool caught{};try{s.Activate();}catch(const IoFailure&){caught=true;}assert(caught);}
    {State s(baseBytes,programBytes);s.base->shortRead=true;Throws([&]{s.Activate();});}

    for(const auto &replacement:{std::string("same size"),std::string("x"),std::string(4097,'L'),std::string()}) {
        State s(baseBytes,programBytes);s.SetPayload("replace.bin",replacement);s.SetPayload("new/deep/added.txt","new nested bytes",Operation::Add);s.SetPayload("folder/extra.txt","extra",Operation::Add);
        auto original=s.base->bytes;auto result=s.Activate();assert(result.status==Status::Activated&&result.externalIdentityAttested&&!result.runtimeIdentityIntegrationImplemented);
        assert(result.preservedFiles==4&&result.replacedFiles==1&&result.addedFiles==2);assert(result.metadataBytes<result.view->size||replacement.empty());
        assert(result.view->HasReliableReadByteCount());vfs::RomFileSystem fs(result.view);
        assert(Text(fs.OpenFile("replace.bin"))==replacement);assert(Text(fs.OpenFile("keep.txt"))=="original kept bytes");assert(Text(fs.OpenFile("folder/existing.txt"))=="nested original");assert(Text(fs.OpenFile("new/deep/added.txt"))=="new nested bytes");
        assert(fs.DirectoryExists("empty")&&fs.DirectoryExists("new/deep")&&fs.FileExists("zero")&&fs.OpenFile("zero")->size==0);
        auto entries=fs.OpenDirectory("")->Read();assert(!entries.empty());auto leaves=fs.OpenDirectory("new/deep",{false,true})->Read();assert(leaves.size()==1&&leaves[0].name=="added.txt"&&leaves[0].size==16);
        // Validate rebuilt indexes again through a second no-op virtual image.
        auto rebuilt=std::make_shared<Memory>(std::vector<uint8_t>(result.view->size));assert(result.view->ReadUnchecked(rebuilt->bytes,0)==rebuilt->size);State second(rebuilt->bytes,programBytes);assert(second.Activate().status==Status::Activated);
        std::vector<uint8_t> all(result.view->size);assert(result.view->ReadUnchecked(all,0)==all.size());
        std::reverse(s.manifest.entries.begin(),s.manifest.entries.end());auto reordered=s.Activate();assert(reordered.status==Status::Activated&&reordered.computedManifestSha256==result.computedManifestSha256);std::vector<uint8_t> reorderedBytes(reordered.view->size);assert(reordered.view->ReadUnchecked(reorderedBytes,0)==reorderedBytes.size()&&reorderedBytes==all);
        for(size_t at{};at<=all.size();++at)for(size_t width:{size_t{0},size_t{1},size_t{7},size_t{31},size_t{129}}){auto count=std::min(width,all.size()-at);std::vector<uint8_t>part(count);assert(result.view->ReadUnchecked(part,at)==count);assert(std::equal(part.begin(),part.end(),all.begin()+at));}
        std::array<uint8_t,1> byte{};Throws([&]{result.view->ReadUnchecked(byte,result.view->size);});Throws([&]{result.view->ReadUnchecked(byte,SIZE_MAX);});
        std::array<uint8_t,23> noAlloc{};auto before=allocationCount.load();assert(result.view->ReadUnchecked(noAlloc,0)==noAlloc.size());assert(allocationCount==before);
        std::vector<std::thread> readers;for(size_t i{};i<4;++i)readers.emplace_back([&]{for(size_t j{};j<100;++j){std::array<uint8_t,23>out{};assert(result.view->ReadUnchecked(out,j)==out.size());assert(std::equal(out.begin(),out.end(),all.begin()+j));}});for(auto&thread:readers)thread.join();
        DeviceState state;service::ServiceManager manager;kernel::type::KSession session;kernel::ipc::IpcResponse response;service::fssrv::IStorage storage(result.view,state,manager);
        kernel::ipc::IpcRequest empty{{0,0},{}};storage.GetSize(session,empty,response);assert(response.value==result.view->size);
        auto entry=fs.fileMap.at("new/deep/added.txt");std::array<uint8_t,16> data{};kernel::ipc::IpcRequest request{{fs.header.dataOffset+entry.offset,999},{data}};assert(storage.Read(session,request,response)==skyline::Result{});assert(std::string(data.begin(),data.end())=="new nested bytes");
        s.base->shortRead=true;auto keep=fs.OpenFile("keep.txt");Throws([&]{Text(keep);});s.base->shortRead=false;s.base->fail=true;bool caught{};try{Text(keep);}catch(const IoFailure&){caught=true;}assert(caught);s.base->fail=false;
        s.base->Change(0x200);Throws([&]{result.view->ReadUnchecked(byte,0);});s.base->bytes=original;--s.base->generation;
        s.program->Change(0);Throws([&]{result.view->ReadUnchecked(byte,0);});s.program->Change(0);s.program->generation-=2;
        assert(s.base->bytes==original);
        if(replacement.size()==4097){Save(evidence+"/virtual.romfs",all);std::ofstream out(evidence+"/result.json");out<<"{\"status\":\"activated\",\"application_id\":\"0000000000001000\",\"program_id\":\"0000000000001002\",\"identity_evidence\":\"test_external_policy_attestation\",\"runtime_identity_integration\":\"not_implemented\",\"source_guard\":\"test_same_source_generation\",\"program_sha256\":\""<<result.computedProgramSha256<<"\",\"base_romfs_sha256\":\""<<result.computedRomfsSha256<<"\",\"manifest_sha256\":\""<<result.computedManifestSha256<<"\",\"virtual_sha256\":\""<<Hash(all)<<"\",\"metadata_bytes\":"<<result.metadataBytes<<",\"virtual_size\":"<<result.virtualSize<<",\"segments\":"<<result.segmentCount<<",\"signature_verification\":\"not_performed\",\"real_ctr_bktr_validation\":\"not_performed\"}\n";}
    }

    {State s(baseBytes,programBytes);auto payload=std::make_shared<MutablePayload>("mutating");s.manifest.entries={{"replace.bin",Operation::Replace,payload->Size(),Hash(payload->bytes),payload}};auto result=s.Activate();assert(result.status==Status::Activated);vfs::RomFileSystem fs(result.view);payload->Change();Throws([&]{Text(fs.OpenFile("replace.bin"));});}
    {State s(baseBytes,programBytes);auto r=s.Activate();assert(r.status==Status::Activated);vfs::RomFileSystem fs(r.view);s.base->mutateAfterRead=true;Throws([&]{Text(fs.OpenFile("keep.txt"));});}
    {State s(baseBytes,programBytes);auto p=std::make_shared<MutablePayload>("during read");s.manifest.entries={{"replace.bin",Operation::Replace,p->Size(),Hash(p->bytes),p}};auto r=s.Activate();assert(r.status==Status::Activated);vfs::RomFileSystem fs(r.view);p->mutateAfterRead=true;Throws([&]{Text(fs.OpenFile("replace.bin"));});}
    for(bool shortRead:{false,true}){State s(baseBytes,programBytes);auto p=std::make_shared<MutablePayload>("failing");s.manifest.entries={{"replace.bin",Operation::Replace,p->Size(),Hash(p->bytes),p}};auto result=s.Activate();assert(result.status==Status::Activated);vfs::RomFileSystem fs(result.view);p->fail=!shortRead;p->shortRead=shortRead;Throws([&]{Text(fs.OpenFile("replace.bin"));});}
    {State s(baseBytes,programBytes);auto p=std::make_shared<MutablePayload>("failing whole read");s.manifest.entries={{"replace.bin",Operation::Replace,p->Size(),Hash(p->bytes),p}};auto r=s.Activate();assert(r.status==Status::Activated);p->fail=true;DeviceState state;service::ServiceManager manager;kernel::type::KSession session;kernel::ipc::IpcResponse response;service::fssrv::IStorage storage(r.view,state,manager);std::vector<uint8_t>out(r.view->size);kernel::ipc::IpcRequest request{{0,out.size()},{out}};Throws([&]{storage.Read(session,request,response);});assert(s.base->bytes==baseBytes);}
    {State s(baseBytes,programBytes);auto p=std::make_shared<MutablePayload>("unknown");p->snapshot=false;s.manifest.entries={{"replace.bin",Operation::Replace,p->Size(),Hash(p->bytes),p}};Rejected(s,"payload_snapshot_contract_missing");assert(p->reads==0);}
    {State s(baseBytes,programBytes);auto p=std::make_shared<MutablePayload>("fail");p->fail=true;s.manifest.entries={{"replace.bin",Operation::Replace,p->Size(),Hash(p->bytes),p}};Rejected(s,"payload_validation_read_failed");}
    for(const auto*path:{"/absolute","../escape","a/../b","a//b","a/./b","back\\slash","x:","trailing/","name.","space "}){State s(baseBytes,programBytes);s.SetPayload(path,"x",Operation::Add);auto r=s.Activate();assert(r.status==Status::Rejected&&r.view==s.base);}
    for(const auto&path:{std::string("nul\0byte",8),std::string("\xc3\xa9")}){State s(baseBytes,programBytes);s.SetPayload(path,"x",Operation::Add);Rejected(s,"unsupported_path_encoding");assert(s.base->reads==0);}
    {State s(baseBytes,programBytes);s.manifest.version=2;Rejected(s,"unsupported_manifest_version");}
    {State s(baseBytes,programBytes);s.SetPayload("replace.bin","x");s.manifest.entries[0].operation=static_cast<Operation>(99);Rejected(s,"unsupported_operation");}
    {State s(baseBytes,programBytes);s.program->size=SIZE_MAX;Rejected(s,"integer_overflow");assert(s.base->reads==0&&s.program->reads==0);}
    {State s(baseBytes,programBytes);s.SetPayload("keep.txt","x",Operation::Add);Rejected(s,"addition_path_conflict");}
    {State s(baseBytes,programBytes);s.SetPayload("missing","x");Rejected(s,"replacement_file_missing");}
    {State s(baseBytes,programBytes);s.SetPayload("keep.txt/child","x",Operation::Add);Rejected(s,"manifest_parent_conflict");}
    {State s(baseBytes,programBytes);s.SetPayload("new/x","x",Operation::Add);s.SetPayload("NEW/y","y",Operation::Add);Rejected(s,"manifest_parent_conflict");}
    {State s(baseBytes,programBytes);s.SetPayload("replace.bin","x");s.SetPayload("REPLACE.BIN","y");Rejected(s,"duplicate_manifest_path");}
    {State s(baseBytes,programBytes);s.SetPayload("replace.bin","x");s.manifest.entries[0].size=2;Rejected(s,"payload_size_mismatch");}
    {State s(baseBytes,programBytes);s.SetPayload("replace.bin","x");s.manifest.entries[0].sha256=std::string(64,'0');Rejected(s,"payload_hash_mismatch");}
    for(int which{};which<7;++which){State s(baseBytes,programBytes);s.SetPayload("replace.bin","x");switch(which){case 0:s.options.limits.maxMetadataBytes=1;break;case 1:s.options.limits.maxEntries=1;break;case 2:s.options.limits.maxHashBytes=1;break;case 3:s.options.limits.maxPayloadBytes=0;break;case 4:s.options.limits.maxVirtualBytes=1;break;case 5:s.options.limits.maxDepth=1;break;case 6:s.options.limits.maxTotalPathBytes=1;break;}auto r=s.Activate();assert(r.status==Status::Rejected&&r.view==s.base);}

    auto dm=d::Little(baseBytes,0x18,8),fm=d::Little(baseBytes,0x38,8),dh=d::Little(baseBytes,8,8),fh=d::Little(baseBytes,0x28,8);
    const std::vector<std::tuple<uint64_t,uint64_t,size_t>> corrupt{
        {0,0,8},{0x18,UINT64_MAX,8},{0x28,dh,8},{dm+4,0,4},{dm+8,0,4},{dm+20,0xffff,4},
        {fm+4,0,4},{fm+8,UINT64_MAX,8},{fm+16,UINT64_MAX,8},{dh,0xabcdef,4},{dm+16,0,4},{fh,0xabcdef,4}
    };
    for(auto [at,value,width]:corrupt){auto bytes=baseBytes;d::Put(bytes,at,value,width);State s(bytes,programBytes);s.SetPayload("replace.bin","x");auto r=s.Activate();assert(r.status==Status::Rejected&&r.view==s.base);}
    for(const auto &name:{"case-collision.romfs","path-conflict.romfs"}){State s(Load(root+"/"+name),programBytes);auto r=s.Activate();assert(r.status==Status::Rejected&&r.view==s.base);}
    Throws([]{d::Add(UINT64_MAX,1);});Throws([]{d::Align(UINT64_MAX,16);});Throws([]{d::CheckRange(UINT64_MAX,1,UINT64_MAX);});
    std::ofstream(evidence+"/tests.txt")<<"PASS: production RomFileSystem/IStorage, rebuilt tree/hash indexes, segments, explicit identity/generation gates, fallback, malformed metadata, budgets, short reads, immutable lifetime and SHA vectors. No commercial content, Android, true CTR/BKTR or game execution tested.\n";
}
