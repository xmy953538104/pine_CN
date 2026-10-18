// SPDX-License-Identifier: MPL-2.0
#include "romfs_detail.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <tuple>

namespace skyline::vfs::prototype {
    namespace {
        using namespace detail;
        constexpr uint32_t Empty{0xffffffff};
        void Require(bool condition, const char *code) { if (!condition) throw Invalid(code); }
        uint32_t Narrow(uint64_t value) { Require(value < Empty, "metadata_offset_overflow"); return static_cast<uint32_t>(value); }
        std::string Parent(const std::string &path) { auto at=path.rfind('/');return at==std::string::npos?"":path.substr(0,at); }
        std::string Name(const std::string &path) { auto at=path.rfind('/');return at==std::string::npos?path:path.substr(at+1); }
        void ReadOnly(const Backing &source) {
            Require(source.mode.read && !source.mode.write && !source.mode.append, "source_not_readonly");
            Require(source.HasReliableReadByteCount(), "source_byte_count_unreliable");
        }
        struct Extent { uint64_t start{}, end{}; };
        void NoOverlap(std::vector<Extent> regions) {
            std::sort(regions.begin(),regions.end(),[](auto a,auto b){return std::tie(a.start,a.end)<std::tie(b.start,b.end);});
            for(size_t i{1};i<regions.size();++i)Require(regions[i].start>=regions[i-1].end,"overlapping_romfs_regions");
        }
        struct Raw {
            uint32_t at{},parent{},sibling{},child{Empty},file{Empty},nextHash{};
            uint64_t data{},size{};
            std::string name,path;
        };
        struct File {
            uint64_t offset{},size{};
            std::shared_ptr<const PayloadSource> payload;
        };
        struct Image { std::set<std::string> dirs; std::map<std::string,File> files; uint64_t metadataBytes{}; };
        std::vector<Raw> Records(const std::vector<uint8_t> &table,bool directory,const Limits &limits,uint64_t &entryCount) {
            std::vector<Raw> result;
            const uint64_t fixed=directory?0x18:0x20;
            for(uint64_t at{};at<table.size();) {
                CheckRange(at,fixed,table.size());Require(++entryCount<=limits.maxEntries,"entry_budget");
                Raw item;item.at=Narrow(at);item.parent=static_cast<uint32_t>(Little(table,at,4));item.sibling=static_cast<uint32_t>(Little(table,at+4,4));
                auto length=Little(table,at+fixed-4,4);Require(length<=limits.maxNameBytes,"name_budget");
                auto size=Align(Add(fixed,length),4);CheckRange(at,size,table.size());
                item.name.assign(reinterpret_cast<const char*>(table.data()+at+fixed),static_cast<size_t>(length));
                if(directory) {item.child=static_cast<uint32_t>(Little(table,at+8,4));item.file=static_cast<uint32_t>(Little(table,at+12,4));item.nextHash=static_cast<uint32_t>(Little(table,at+16,4));}
                else {item.data=Little(table,at+8,8);item.size=Little(table,at+16,8);item.nextHash=static_cast<uint32_t>(Little(table,at+24,4));}
                if(!directory||at) {Require(!item.name.empty()&&item.name.find('/')==std::string::npos,"invalid_romfs_name");CanonicalPath(item.name,limits);}
                result.push_back(std::move(item));at=Add(at,size);
            }
            return result;
        }
        std::map<uint32_t,size_t> Index(const std::vector<Raw> &items) {
            std::map<uint32_t,size_t> result;for(size_t i{};i<items.size();++i)result.emplace(items[i].at,i);return result;
        }
        size_t Lookup(const std::map<uint32_t,size_t> &index,uint32_t offset) {
            auto found=index.find(offset);Require(found!=index.end(),"invalid_romfs_entry_offset");return found->second;
        }
        void HashIndex(const std::vector<Raw>&items,const std::vector<uint8_t>&buckets,const std::map<uint32_t,size_t>&index) {
            Require(!buckets.empty()&&buckets.size()%4==0,"invalid_romfs_hash_table");std::vector<bool> seen(items.size());size_t count{};
            for(size_t b{};b<buckets.size()/4;++b) {
                auto at=static_cast<uint32_t>(Little(buckets,b*4,4));
                while(at!=Empty) {
                    auto i=Lookup(index,at);Require(!seen[i],"romfs_hash_cycle_or_duplicate");seen[i]=true;++count;
                    Require(PathHash(items[i].parent,items[i].name)%(buckets.size()/4)==b,"wrong_romfs_hash_bucket");at=items[i].nextHash;
                }
            }
            Require(count==items.size(),"unindexed_romfs_entry");
        }
        Image Parse(Backing &base,const Limits &limits) {
            auto header=Read(base,0,0x50);Require(Little(header,0,8)==0x50,"unsupported_romfs_header");
            uint64_t total{};std::array<uint64_t,4> offsets{},sizes{};std::vector<Extent> extents{{0,0x50}};
            for(size_t i{};i<4;++i) {
                offsets[i]=Little(header,8+i*16,8);sizes[i]=Little(header,16+i*16,8);
                Require(offsets[i]>=0x50&&offsets[i]%4==0,"invalid_romfs_table_offset");CheckRange(offsets[i],sizes[i],base.size);
                total=Add(total,sizes[i]);Require(total<=limits.maxMetadataBytes,"metadata_budget");
                if(sizes[i])extents.push_back({offsets[i],Add(offsets[i],sizes[i])});
            }
            NoOverlap(extents);
            auto dataOffset=Little(header,0x48,8);Require(dataOffset>=0x50&&dataOffset<=base.size,"invalid_romfs_data_offset");
            auto dirHash=Read(base,offsets[0],sizes[0]),dirMeta=Read(base,offsets[1],sizes[1]);
            auto fileHash=Read(base,offsets[2],sizes[2]),fileMeta=Read(base,offsets[3],sizes[3]);
            uint64_t entryCount{};auto dirs=Records(dirMeta,true,limits,entryCount),files=Records(fileMeta,false,limits,entryCount);
            Require(!dirs.empty()&&dirs[0].at==0&&dirs[0].name.empty()&&dirs[0].parent==0&&dirs[0].sibling==Empty,"invalid_romfs_root");
            auto dirIndex=Index(dirs),fileIndex=Index(files);std::vector<bool> visitedDir(dirs.size()),visitedFile(files.size());visitedDir[0]=true;
            std::vector<size_t> queue{0};Image result;result.dirs.insert("");result.metadataBytes=Add(total,0x50);
            std::set<std::string> folded{ "" };uint64_t pathBytes{};
            auto path=[&](const std::string&parent,const std::string&name){auto p=parent.empty()?name:parent+"/"+name;CanonicalPath(p,limits);pathBytes=Add(pathBytes,p.size());Require(pathBytes<=limits.maxTotalPathBytes,"path_bytes_budget");Require(folded.insert(Fold(p)).second,"romfs_duplicate_or_case_collision");return p;};
            for(size_t q{};q<queue.size();++q) {
                auto parent=queue[q];auto at=dirs[parent].child;
                while(at!=Empty) {
                    auto i=Lookup(dirIndex,at);Require(!visitedDir[i],"romfs_tree_cycle_or_duplicate");visitedDir[i]=true;
                    Require(dirs[i].parent==dirs[parent].at,"romfs_parent_mismatch");dirs[i].path=path(dirs[parent].path,dirs[i].name);
                    result.dirs.insert(dirs[i].path);queue.push_back(i);at=dirs[i].sibling;
                }
                at=dirs[parent].file;
                while(at!=Empty) {
                    auto i=Lookup(fileIndex,at);Require(!visitedFile[i],"romfs_file_cycle_or_duplicate");visitedFile[i]=true;
                    Require(files[i].parent==dirs[parent].at,"romfs_parent_mismatch");files[i].path=path(dirs[parent].path,files[i].name);
                    auto absolute=Add(dataOffset,files[i].data);CheckRange(absolute,files[i].size,base.size);
                    if(files[i].size)extents.push_back({absolute,Add(absolute,files[i].size)});
                    result.files.emplace(files[i].path,File{absolute,files[i].size,{}});at=files[i].sibling;
                }
            }
            Require(std::all_of(visitedDir.begin(),visitedDir.end(),[](bool b){return b;})&&std::all_of(visitedFile.begin(),visitedFile.end(),[](bool b){return b;}),"orphan_romfs_entry");
            NoOverlap(extents);HashIndex(dirs,dirHash,dirIndex);HashIndex(files,fileHash,fileIndex);
            return result;
        }

        class MemoryPayload final:public PayloadSource {
            const std::vector<uint8_t> bytes;
          public:
            explicit MemoryPayload(std::vector<uint8_t> bytes):bytes(std::move(bytes)){}
            uint64_t Size()const override{return bytes.size();}
            bool IsImmutableSnapshot()const noexcept override{return true;}
            void CheckUnchanged()const override{}
            size_t Read(uint64_t offset,uint8_t*out,size_t count)const override{CheckRange(offset,count,bytes.size());if(count)std::memcpy(out,bytes.data()+offset,count);return count;}
        };
        enum class Kind{Metadata,Zero,Base,Payload};
        struct Segment {uint64_t offset{},size{},sourceOffset{};Kind kind{};std::shared_ptr<const PayloadSource> payload;};
        class VirtualBacking final:public Backing {
            const std::shared_ptr<Backing> base;
            const size_t baseSize;
            const std::vector<uint8_t> metadata;
            const std::vector<Segment> segments;
            const std::shared_ptr<const SourceGuard> guard;
          protected:
            size_t ReadImpl(span<u8> output,size_t offset)override {
                guard->CheckUnchanged();
                CheckRange(offset,output.size(),size);if(output.empty()){guard->CheckUnchanged();return 0;}
                auto it=std::upper_bound(segments.begin(),segments.end(),offset,[](uint64_t at,const Segment&s){return at<s.offset;});
                Require(it!=segments.begin(),"virtual_segment_gap");--it;size_t done{};
                while(done<output.size()) {
                    Require(it!=segments.end(),"virtual_segment_gap");auto at=Add(offset,done);Require(at>=it->offset&&at-it->offset<it->size,"virtual_segment_gap");
                    auto within=at-it->offset;auto amount=static_cast<size_t>(std::min<uint64_t>(output.size()-done,it->size-within));auto*out=output.data()+done;
                    switch(it->kind) {
                        case Kind::Metadata:std::memcpy(out,metadata.data()+within,amount);break;
                        case Kind::Zero:std::fill_n(out,amount,uint8_t{});break;
                        case Kind::Base:if(base->size!=baseSize)throw std::runtime_error("virtual RomFS base size changed");Exact(*base,Add(it->sourceOffset,within),out,amount);break;
                        case Kind::Payload:it->payload->CheckUnchanged();if(it->payload->Size()!=it->size)throw std::runtime_error("virtual RomFS payload size changed");if(it->payload->Read(within,out,amount)!=amount)throw std::runtime_error("virtual RomFS payload short read");it->payload->CheckUnchanged();break;
                    }
                    done+=amount;++it;
                }
                guard->CheckUnchanged();return done;
            }
          public:
            VirtualBacking(std::shared_ptr<Backing> base,std::vector<uint8_t> metadata,std::vector<Segment> segments,size_t size,std::shared_ptr<const SourceGuard> guard)
                :Backing({true,false,false},size),base(std::move(base)),baseSize(this->base->size),metadata(std::move(metadata)),segments(std::move(segments)),guard(std::move(guard)){}
            bool HasReliableReadByteCount()const noexcept override{return true;}
        };
        uint32_t BucketCount(size_t count) {
            if(count<3)return 3;
            if(count<19)return static_cast<uint32_t>(count)|1;
            auto buckets=Narrow(count);while(buckets%2==0||buckets%3==0||buckets%5==0||buckets%7==0||buckets%11==0||buckets%13==0||buckets%17==0)++buckets;return buckets;
        }
        std::shared_ptr<Backing> Build(const std::shared_ptr<Backing>&base,const Image&image,const Limits&limits,Result&report,const std::shared_ptr<const SourceGuard>&guard) {
            Require(Add(image.dirs.size(),image.files.size())<=limits.maxEntries,"entry_budget");
            struct DirOut {uint32_t at{},parent{},sibling{Empty},child{Empty},file{Empty};std::string name;};
            struct FileOut {uint32_t at{},parent{},sibling{Empty};uint64_t relative{};std::string name;const File* source{};};
            std::vector<DirOut> dirs;std::vector<FileOut> files;std::map<std::string,size_t> dirByPath;uint64_t dirSize{},fileSize{},dataSize{},pathBytes{};
            for(const auto &path:image.dirs) {
                if(!path.empty())CanonicalPath(path,limits);
                pathBytes=Add(pathBytes,path.size());Require(pathBytes<=limits.maxTotalPathBytes,"path_bytes_budget");
                DirOut d;d.at=Narrow(dirSize);d.name=Name(path);if(!path.empty())d.parent=dirs.at(dirByPath.at(Parent(path))).at;
                dirByPath.emplace(path,dirs.size());dirs.push_back(d);dirSize=Add(dirSize,Align(Add(0x18,d.name.size()),4));
            }
            std::vector<std::vector<size_t>> childDirs(dirs.size()),childFiles(dirs.size());
            for(const auto &[path,index]:dirByPath)if(index)childDirs[dirByPath.at(Parent(path))].push_back(index);
            for(const auto &[path,file]:image.files) {
                CanonicalPath(path,limits);pathBytes=Add(pathBytes,path.size());Require(pathBytes<=limits.maxTotalPathBytes,"path_bytes_budget");
                FileOut f;f.at=Narrow(fileSize);f.name=Name(path);auto parent=dirByPath.at(Parent(path));f.parent=dirs[parent].at;
                dataSize=Align(dataSize,0x10);f.relative=dataSize;dataSize=Add(dataSize,file.size);f.source=&file;
                childFiles[parent].push_back(files.size());files.push_back(f);fileSize=Add(fileSize,Align(Add(0x20,f.name.size()),4));
            }
            for(size_t i{};i<dirs.size();++i) {
                auto &d=dirs[i];const auto &children=childDirs[i],&owned=childFiles[i];
                if(!children.empty())d.child=dirs[children[0]].at;
                if(!owned.empty())d.file=files[owned[0]].at;
                for(size_t j{1};j<children.size();++j)dirs[children[j-1]].sibling=dirs[children[j]].at;
                for(size_t j{1};j<owned.size();++j)files[owned[j-1]].sibling=files[owned[j]].at;
            }
            auto dhSize=uint64_t{BucketCount(dirs.size())}*4,fhSize=uint64_t{BucketCount(files.size())}*4;
            const uint64_t dh=0x50,dm=Add(dh,dhSize),fh=Add(dm,dirSize),fm=Add(fh,fhSize),end=Add(fm,fileSize),data=Align(end,0x200);
            Require(data<=limits.maxMetadataBytes&&data<=SIZE_MAX,"metadata_budget");auto virtualSize=Add(data,dataSize);Require(virtualSize<=limits.maxVirtualBytes&&virtualSize<=SIZE_MAX,"virtual_size_budget");
            std::vector<uint8_t> metadata(static_cast<size_t>(data));
            const uint64_t fields[]{0x50,dh,dhSize,dm,dirSize,fh,fhSize,fm,fileSize,data};for(size_t i{};i<10;++i)Put(metadata,i*8,fields[i],8);
            std::fill(metadata.begin()+dh,metadata.begin()+dm,0xff);std::fill(metadata.begin()+fh,metadata.begin()+fm,0xff);
            for(const auto &d:dirs) {
                auto bucket=dh+(PathHash(d.parent,d.name)%(dhSize/4))*4;const uint64_t values[]{d.parent,d.sibling,d.child,d.file,Little(metadata,bucket,4),d.name.size()};
                for(size_t i{};i<6;++i)Put(metadata,dm+d.at+i*4,values[i],4);
                Put(metadata,bucket,d.at,4);std::copy(d.name.begin(),d.name.end(),metadata.begin()+dm+d.at+0x18);
            }
            for(const auto &f:files) {
                auto bucket=fh+(PathHash(f.parent,f.name)%(fhSize/4))*4;auto at=fm+f.at;
                Put(metadata,at,f.parent,4);Put(metadata,at+4,f.sibling,4);Put(metadata,at+8,f.relative,8);Put(metadata,at+16,f.source->size,8);Put(metadata,at+24,Little(metadata,bucket,4),4);Put(metadata,at+28,f.name.size(),4);Put(metadata,bucket,f.at,4);std::copy(f.name.begin(),f.name.end(),metadata.begin()+at+0x20);
            }
            std::vector<Segment> segments{{0,data,0,Kind::Metadata,{}}};uint64_t cursor=data;
            for(const auto &f:files) {
                auto at=Add(data,f.relative);if(at>cursor)segments.push_back({cursor,at-cursor,0,Kind::Zero,{}});
                if(f.source->size)segments.push_back({at,f.source->size,f.source->offset,f.source->payload?Kind::Payload:Kind::Base,f.source->payload});
                cursor=Add(at,f.source->size);
            }
            report.metadataBytes=data;report.segmentCount=segments.size();report.virtualSize=virtualSize;
            return std::make_shared<VirtualBacking>(base,std::move(metadata),std::move(segments),static_cast<size_t>(virtualSize),guard);
        }
    }

    std::shared_ptr<const PayloadSource> OwnPayload(std::vector<uint8_t> bytes){return std::make_shared<MemoryPayload>(std::move(bytes));}

    std::string ManifestSha256(const Manifest &manifest,const Limits &limits) {
        Require(manifest.version==1,"unsupported_manifest_version");Require(manifest.target.applicationId&&manifest.target.programId,"missing_target_identity");
        Require(DigestValid(manifest.target.programSha256)&&DigestValid(manifest.target.romfsSha256),"invalid_target_digest");Require(manifest.entries.size()<=limits.maxManifestEntries,"manifest_entry_budget");
        std::vector<const Entry*> entries;uint64_t pathBytes{};
        for(const auto &entry:manifest.entries){CanonicalPath(entry.path,limits);pathBytes=Add(pathBytes,entry.path.size());Require(pathBytes<=limits.maxTotalPathBytes,"path_bytes_budget");Require(DigestValid(entry.sha256),"invalid_payload_digest");Require(entry.operation==Operation::Add||entry.operation==Operation::Replace,"unsupported_operation");entries.push_back(&entry);}
        std::sort(entries.begin(),entries.end(),[](auto*a,auto*b){return a->path<b->path;});
        std::vector<uint8_t> descriptor;const std::string domain="pine-virtual-romfs-manifest-v1";descriptor.insert(descriptor.end(),domain.begin(),domain.end());descriptor.push_back(0);
        auto number=[&](uint64_t value,size_t width){auto at=descriptor.size();descriptor.resize(at+width);Put(descriptor,at,value,width);};
        auto text=[&](const std::string&value){number(value.size(),4);descriptor.insert(descriptor.end(),value.begin(),value.end());};
        number(manifest.version,4);number(manifest.target.applicationId,8);number(manifest.target.programId,8);text(manifest.target.programSha256);text(manifest.target.romfsSha256);number(entries.size(),4);
        for(const auto *entry:entries){text(entry->path);number(static_cast<uint8_t>(entry->operation),1);number(entry->size,8);text(entry->sha256);}
        return BytesSha256(descriptor.data(),descriptor.size());
    }

    Result CreateVirtualRomfs(const std::shared_ptr<Backing>&base,const Manifest&manifest,const Options&options) {
        Result result;result.view=base;
        if(!options.enabled)return result;
        result.status=Status::Rejected;
        try {
            Require(base!=nullptr,"missing_effective_romfs");ReadOnly(*base);
            Require(options.identityVerifier!=nullptr,"trusted_identity_verifier_missing");
            Require(options.applicationId&&options.programId,"missing_active_identity");
            Require(options.applicationId==manifest.target.applicationId&&options.programId==manifest.target.programId,"target_identity_mismatch");
            Require(options.effectiveProgram!=nullptr,"effective_program_source_missing");ReadOnly(*options.effectiveProgram);
            Require(options.sourceGuard&&options.sourceGuard->Covers(base,options.effectiveProgram),"same_source_guard_missing");
            auto checkGuard=[&]{try{options.sourceGuard->CheckUnchanged();}catch(const std::exception&){throw Invalid("source_generation_changed");}};
            checkGuard();
            Require(DigestValid(options.expectedManifestSha256),"expected_manifest_digest_missing");
            result.computedManifestSha256=ManifestSha256(manifest,options.limits);Require(result.computedManifestSha256==options.expectedManifestSha256,"manifest_hash_mismatch");
            uint64_t hashBytes=Add(base->size,options.effectiveProgram->size),payloadBytes{};std::set<std::string> paths;
            for(const auto &entry:manifest.entries) {
                Require(paths.insert(Fold(entry.path)).second,"duplicate_manifest_path");Require(entry.source!=nullptr,"payload_source_missing");
                Require(entry.source->IsImmutableSnapshot(),"payload_snapshot_contract_missing");Require(entry.source->Size()==entry.size,"payload_size_mismatch");
                payloadBytes=Add(payloadBytes,entry.size);Require(payloadBytes<=options.limits.maxPayloadBytes,"payload_budget");
            }
            hashBytes=Add(hashBytes,payloadBytes);Require(hashBytes<=options.limits.maxHashBytes,"hash_budget");
            result.computedProgramSha256=HashBacking(*options.effectiveProgram);result.hashBytes=options.effectiveProgram->size;
            Require(result.computedProgramSha256==manifest.target.programSha256,"program_hash_mismatch");
            result.computedRomfsSha256=HashBacking(*base);result.hashBytes=Add(result.hashBytes,base->size);
            Require(result.computedRomfsSha256==manifest.target.romfsSha256,"romfs_hash_mismatch");
            IdentityDecision decision{IdentityDecision::Unknown};
            try {decision=options.identityVerifier->Verify({options.applicationId,options.programId,result.computedProgramSha256,result.computedRomfsSha256});}
            catch(const std::exception&){throw Invalid("identity_verifier_failed");}
            Require(decision==IdentityDecision::Approved,"identity_not_attested");result.externalIdentityAttested=true;
            checkGuard();
            auto image=Parse(*base,options.limits);result.preservedFiles=image.files.size();
            std::map<std::string,std::string> folded;
            for(const auto &path:image.dirs)folded.emplace(Fold(path),path);
            for(const auto &[path,file]:image.files){(void)file;folded.emplace(Fold(path),path);}
            for(const auto &entry:manifest.entries) {
                auto collision=folded.find(Fold(entry.path));
                if(entry.operation==Operation::Replace)Require(image.files.count(entry.path)!=0,"replacement_file_missing");
                else Require(collision==folded.end(),"addition_path_conflict");
                Require(collision==folded.end()||collision->second==entry.path,"manifest_case_collision");
                for(auto parent=Parent(entry.path);!parent.empty();parent=Parent(parent)) {
                    auto found=folded.find(Fold(parent));Require(found==folded.end()||(found->second==parent&&image.dirs.count(parent)),"manifest_parent_conflict");
                    image.dirs.insert(parent);folded.emplace(Fold(parent),parent);
                }
                try {auto digest=HashPayload(*entry.source);result.hashBytes=Add(result.hashBytes,entry.size);Require(digest==entry.sha256,"payload_hash_mismatch");}
                catch(const Invalid&){throw;}
                catch(const std::exception&){throw Invalid("payload_validation_read_failed");}
                image.files[entry.path]={0,entry.size,entry.source};folded[Fold(entry.path)]=entry.path;
                if(entry.operation==Operation::Replace){++result.replacedFiles;--result.preservedFiles;}else ++result.addedFiles;
            }
            auto view=Build(base,image,options.limits,result,options.sourceGuard);
            for(const auto &entry:manifest.entries){try{entry.source->CheckUnchanged();}catch(const std::exception&){throw Invalid("payload_generation_changed");}}
            checkGuard();result.view=std::move(view);result.status=Status::Activated;
        } catch(const Invalid&error) {result.reason=error.code;}
        return result;
    }
}
