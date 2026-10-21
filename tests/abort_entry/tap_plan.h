// SPDX-License-Identifier: MPL-2.0
// Content-free synthetic staging only. Not linked into Pine or an SDK loader.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace abort_tap {
using Digest = std::array<uint8_t, 32>;
inline uint32_t Ror(uint32_t v, unsigned n) { return (v >> n) | (v << (32 - n)); }
inline Digest Sha256(std::span<const uint8_t> input) {
    constexpr uint32_t k[64]{
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t h[8]{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const size_t blocks = input.size() / 64 + ((input.size() % 64 < 56) ? 1 : 2);
    for (size_t block{}; block < blocks; ++block) {
        uint32_t w[64]{};
        for (size_t i{}; i < 64; ++i) {
            const size_t p = block * 64 + i;
            uint8_t byte{};
            if (p < input.size()) byte = input[p];
            else if (p == input.size()) byte = 0x80;
            else if (block == blocks - 1 && i >= 56) byte = static_cast<uint8_t>((uint64_t(input.size()) * 8) >> ((63 - i) * 8));
            w[i / 4] |= uint32_t(byte) << ((3 - i % 4) * 8);
        }
        for (size_t i{16}; i < 64; ++i) {
            auto a = w[i-15], b = w[i-2];
            w[i] = w[i-16] + (Ror(a,7)^Ror(a,18)^(a>>3)) + w[i-7] + (Ror(b,17)^Ror(b,19)^(b>>10));
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],z=h[7];
        for (size_t i{}; i < 64; ++i) {
            uint32_t t1=z+(Ror(e,6)^Ror(e,11)^Ror(e,25))+((e&f)^(~e&g))+k[i]+w[i];
            uint32_t t2=(Ror(a,2)^Ror(a,13)^Ror(a,22))+((a&b)^(a&c)^(b&c));
            z=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=z;
    }
    Digest result{};
    for (size_t i{}; i < 32; ++i) result[i] = static_cast<uint8_t>(h[i/4] >> ((3-i%4)*8));
    return result;
}
inline uint32_t Word(std::span<const uint8_t> bytes, size_t offset) {
    return uint32_t(bytes[offset]) | uint32_t(bytes[offset+1])<<8 | uint32_t(bytes[offset+2])<<16 | uint32_t(bytes[offset+3])<<24;
}
inline void Put(std::span<uint8_t> bytes, size_t offset, uint32_t value) {
    for (unsigned i{}; i<4; ++i) bytes[offset+i] = uint8_t(value >> (8*i));
}
inline bool Branch(uintptr_t from, uintptr_t to, bool link, uint32_t &word) {
    if ((from | to) & 3) return false;
    int64_t displacement{};
    if (to >= from) {
        if (to-from > 0x7fffffc) return false;
        displacement = int64_t(to-from);
    } else {
        if (from-to > 0x8000000) return false;
        displacement = -int64_t(from-to);
    }
    word = (link ? 0x94000000u : 0x14000000u) | (uint32_t(displacement/4)&0x3ffffffu);
    return true;
}
inline bool Target(uint32_t word, uintptr_t pc, bool link, uintptr_t &target) {
    if ((pc&3) || (word&0xfc000000u)!=(link?0x94000000u:0x14000000u)) return false;
    int64_t immediate = word & 0x3ffffffu;
    if (immediate & 0x2000000) immediate -= 0x4000000;
    const int64_t offset = immediate*4;
    if (offset >= 0) { if (UINTPTR_MAX-pc<uint64_t(offset)) return false; target=pc+uintptr_t(offset); }
    else { if (pc<uint64_t(-offset)) return false; target=pc-uintptr_t(-offset); }
    return true;
}
struct Evidence {
    enum class Site { BlCall, EntrySub32 } site{Site::BlCall};
    bool enabled{}, syntheticOwned{}, producersQuiescent{}, writable{}, executable{};
    Digest moduleId{}, segmentHash{}, windowHash{};
    uintptr_t base{};
    size_t call{}, target{}, stub{}, tail{}, windowStart{}, windowSize{};
    uint32_t original{};
    uint64_t generation{};
};
struct Plan { size_t call{}; uint32_t original{}, replacement{}; Digest before{}, after{}; uintptr_t base{}; uint64_t generation{}; };
enum class Stop { None, Disabled, Scope, Mapping, Size, Identity, SegmentHash, Window, Instruction, Tail, Range, Conflict };
inline std::string_view Name(Stop stop) {
    constexpr std::string_view names[]{"ok","disabled","synthetic_scope_required","mapping_or_quiescence","bounded_text","module_identity","segment_hash","exact_window","original_bl","tail_b","branch_range","rollback_or_staging_conflict"};
    return names[static_cast<unsigned>(stop)];
}
inline Stop Prepare(std::span<const uint8_t> text, const Digest &actualId, const Evidence &e, Plan &plan) {
    if (!e.enabled) return Stop::Disabled;
    if (!e.syntheticOwned) return Stop::Scope;
    if (!e.producersQuiescent || !e.writable || e.executable || (e.base&3) || !e.generation) return Stop::Mapping;
    if (text.size()<4 || text.size()>65536 || UINTPTR_MAX-e.base<text.size()) return Stop::Size;
    const auto wordFits=[&](size_t p){return !(p&3) && p<=text.size()-4;};
    if (!wordFits(e.call)||!wordFits(e.target)||!wordFits(e.stub)||!wordFits(e.tail)||e.call==e.target||e.call==e.stub||e.target==e.stub||e.tail<e.stub) return Stop::Size;
    bool known{}; for (auto byte : actualId) known = known || byte!=0;
    if (!known || actualId!=e.moduleId) return Stop::Identity;
    if (Sha256(text)!=e.segmentHash) return Stop::SegmentHash;
    if (!e.windowSize || e.windowStart>text.size() || e.windowSize>text.size()-e.windowStart || e.windowStart>e.call || e.call-e.windowStart>e.windowSize || e.windowSize-(e.call-e.windowStart)<4 || Sha256(text.subspan(e.windowStart,e.windowSize))!=e.windowHash) return Stop::Window;
    uintptr_t target{};
    if (Word(text,e.call)!=e.original) return Stop::Instruction;
    if (e.site==Evidence::Site::BlCall) {
        if (!Target(e.original,e.base+e.call,true,target) || target!=e.base+e.target) return Stop::Instruction;
    } else {
        // Exactly one authenticated non-PC-relative instruction. No generic relocator.
        if (e.original!=0xd10083ffu || e.target!=e.call+4 || e.tail<e.stub+4 || Word(text,e.tail-4)!=0xd10083ffu) return Stop::Instruction;
    }
    if (!Target(Word(text,e.tail),e.base+e.tail,false,target) || target!=e.base+e.target) return Stop::Tail;
    uint32_t replacement{};
    if (!Branch(e.base+e.call,e.base+e.stub,e.site==Evidence::Site::BlCall,replacement)) return Stop::Range;
    plan={e.call,e.original,replacement,e.segmentHash,{},e.base,e.generation};
    return Stop::None;
}
inline Stop Apply(std::span<uint8_t> text, const Evidence &e, Plan &plan) {
    if (!e.syntheticOwned || !e.producersQuiescent || !e.writable || e.executable || e.base!=plan.base || e.generation!=plan.generation || plan.call>text.size() || text.size()-plan.call<4) return Stop::Mapping;
    if (Sha256(text)!=plan.before || Word(text,plan.call)!=plan.original) return Stop::Conflict;
    Put(text,plan.call,plan.replacement); plan.after=Sha256(text); return Stop::None;
}
inline Stop Rollback(std::span<uint8_t> text, const Evidence &e, const Plan &plan) {
    if (!e.syntheticOwned || !e.producersQuiescent || !e.writable || e.executable || e.base!=plan.base || e.generation!=plan.generation || plan.call>text.size() || text.size()-plan.call<4) return Stop::Mapping;
    if (Sha256(text)!=plan.after || Word(text,plan.call)!=plan.replacement) return Stop::Conflict;
    Put(text,plan.call,plan.original); return Sha256(text)==plan.before ? Stop::None : Stop::Conflict;
}
}
