// SPDX-License-Identifier: MPL-2.0
#include "observation.h"
#include "observation_words.h"
#include "../diagnostics/image_digest.h"
#include <cstring>
namespace skyline::nce {
namespace {
uint32_t Word(std::span<const uint8_t> bytes,size_t p){return uint32_t(bytes[p])|uint32_t(bytes[p+1])<<8|uint32_t(bytes[p+2])<<16|uint32_t(bytes[p+3])<<24;}
void Put(std::span<uint8_t> bytes,size_t p,uint32_t word){for(unsigned i{};i<4;++i)bytes[p+i]=uint8_t(word>>(8*i));}
bool Branch(uint64_t pc,uint64_t target,bool link,uint32_t &word){
    if((pc|target)&3)return false;
    int64_t distance{};
    if(target>=pc){if(target-pc>0x7fffffc)return false;distance=int64_t(target-pc);}
    else{if(pc-target>0x8000000)return false;distance=-int64_t(pc-target);}
    word=(link?0x94000000u:0x14000000u)|(uint32_t(distance/4)&0x3ffffffu);return true;
}
}
bool InstallCallsiteObservation(std::span<uint8_t> text,uint64_t base,uint32_t callVa,uint32_t pltVa,uint32_t expectedBl,
    std::span<uint8_t> stub,uint64_t stubPc,bool quiescent,ObservationPatch &out) noexcept {
    constexpr size_t bytes=ObservationWords.size()*4;
    if(!quiescent||(base|stubPc|callVa|pltVa)&3||text.size()<16||callVa>text.size()-4||pltVa>text.size()-16||stub.size()<bytes||UINT64_MAX-base<text.size()||UINT64_MAX-stubPc<bytes)return false;
    // Identity/window hashes are independently checked by the load owner. This
    // additional semantic check is what permits scratch X16/X17, never others.
    const auto a=Word(text,pltVa),b=Word(text,pltVa+4),c=Word(text,pltVa+8),d=Word(text,pltVa+12);
    if((a&0x9f00001fu)!=0x90000010u||(b&0xffc003ffu)!=0xf9400211u||(c&0xffc003ffu)!=0x91000210u||d!=0xd61f0220u)return false;
    uint32_t original{},redirect{},tail{};
    if(!Branch(base+callVa,base+pltVa,true,original)||original!=expectedBl||Word(text,callVa)!=original||
       !Branch(base+callVa,stubPc,true,redirect)||!Branch(stubPc+bytes-4,base+pltVa,false,tail))return false;
    for(size_t i{};i<ObservationWords.size();++i)Put(stub,i*4,ObservationWords[i]);
    Put(stub,bytes-4,tail);
    out={original,redirect,base+callVa,stubPc,base+pltVa,diagnostics::image::Sha256(stub.first(bytes))};
    Put(text,callVa,redirect);return true;
}
bool RollbackCallsiteObservation(std::span<uint8_t> text,uint32_t callVa,std::span<const uint8_t> stub,bool quiescent,const ObservationPatch &patch) noexcept {
    constexpr size_t bytes=ObservationWords.size()*4;
    if(!quiescent||text.size()<4||callVa>text.size()-4||stub.size()<bytes||Word(text,callVa)!=patch.installed||diagnostics::image::Sha256(stub.first(bytes))!=patch.stubHash)return false;
    Put(text,callVa,patch.original);return true;
}
}
