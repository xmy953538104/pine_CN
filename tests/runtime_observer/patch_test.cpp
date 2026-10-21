// SPDX-License-Identifier: MPL-2.0
#include "observation.h"
#include <array>
#include <cassert>
#include <cstring>
#include <cstdio>
using namespace skyline::nce;
static void Put(auto &a,size_t p,uint32_t v){std::memcpy(a.data()+p,&v,4);}
int main(){
 std::array<uint8_t,128> text{};Put(text,0,0x94000010);Put(text,64,0x90000010);Put(text,68,0xf9400211);Put(text,72,0x91000210);Put(text,76,0xd61f0220);
 const auto original=text;std::array<uint8_t,4096> stub{};ObservationPatch patch;
 auto reject=[&](uint64_t base,uint64_t stubPc,bool quiescent){auto t=original;auto s=stub;assert(!InstallCallsiteObservation(t,base,0,64,0x94000010,s,stubPc,quiescent,patch));assert(t==original&&s==stub);};
 reject(0x10000000,0x10001000,false);reject(0x10000001,0x10001000,true);reject(0x10000000,0x20000000,true);reject(UINT64_MAX-63,0x10001000,true);
 auto t=original;auto s=stub;Put(t,0,0x94000011);const auto bad=t;assert(!InstallCallsiteObservation(t,0x10000000,0,64,0x94000010,s,0x10001000,true,patch)&&t==bad&&s==stub);
 for(size_t offset:{64,68,72,76}){t=original;t[offset]^=1;const auto before=t;assert(!InstallCallsiteObservation(t,0x10000000,0,64,0x94000010,s,0x10001000,true,patch)&&t==before&&s==stub);}
 for(uint64_t base:{0x10000000ULL,0x7100000000ULL,0x7ffffffff000ULL}){
 t=original;s=stub;assert(InstallCallsiteObservation(t,base,0,64,0x94000010,s,base+4096,true,patch));assert(t!=original);
 auto installed=t;assert(!RollbackCallsiteObservation(t,0,s,false,patch)&&t==installed);
 s[0]^=1;assert(!RollbackCallsiteObservation(t,0,s,true,patch)&&t==installed);s[0]^=1;
 assert(RollbackCallsiteObservation(t,0,s,true,patch)&&t==original);
 }
 puts("PASS production callsite emitter: quiescent/range/alignment/overflow/exact original BL/PLT rejection leaves text and stub untouched; three explicit relocated address calculations; conflict-aware quiescent rollback (no Guest execution)");
}
