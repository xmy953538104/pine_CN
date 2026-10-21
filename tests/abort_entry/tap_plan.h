// SPDX-License-Identifier: MPL-2.0
// Content-free synthetic staging only. Not linked into Pine or an SDK loader.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include "image_digest.h"

namespace abort_tap {
using skyline::diagnostics::image::Digest;
using skyline::diagnostics::image::Sha256;
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
