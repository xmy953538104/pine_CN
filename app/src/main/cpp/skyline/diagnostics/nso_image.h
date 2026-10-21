// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "image_digest.h"
#include "module_identity.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <span>

namespace skyline::diagnostics::image {
constexpr size_t MaxModules = 16, MaxWindows = 8, MaxWindowBytes = 256;
constexpr size_t MaxHashBytes = 256 * 1024 * 1024;
struct WindowRequest { uint32_t va{}, size{}; };
struct SegmentInput {
    uint32_t va{}, size{};
    std::span<const uint8_t> contents; // transient, never retained
    Digest declaredHash{};
    bool hashDeclared{};
};
struct Input {
    Digest buildId{}; // raw 32 header bytes, no u64 endian formatting
    std::array<SegmentInput, 3> segments;
    std::string_view name;
};
struct Segment {
    uint32_t va{}, size{};
    uint64_t mappedStart{};
    Digest computedHash{}, declaredHash{};
    bool hashDeclared{}, hashMatches{};
};
struct Window {
    uint32_t va{}, size{};
    std::array<uint8_t, MaxWindowBytes> bytes{};
    Digest hash{};
};
struct Record {
    ModuleOwner owner;
    std::array<char, 40> moduleName{};
    Digest buildId{};
    std::array<Segment, 3> segments;
    std::array<Window, MaxWindows> windows;
    size_t windowCount{};
    uint64_t moduleBase{}, loadReservationBase{}, loadReservationSize{};
    // Source is the decompressed, unpadded LoadNso input BEFORE LoadExecutable.
    // Existing decompressor return/Backing short reads are not validated here.
    // These bytes are not proof of the later patched/relocated Guest mapping.
    bool decompressionResultObserved{false}, liveBytesVerified{false};
};
struct ExpectedWindow { uint32_t va{}, size{}; Digest hash{}; };
struct ExpectedPreimage {
    Digest buildId{};
    std::array<Digest, 3> segmentHashes;
    std::array<ExpectedWindow, MaxWindows> windows;
    size_t windowCount{};
};
enum class PreimageMatch { MatchOnly, UnknownExpected, BuildIdMismatch, SegmentMismatch, WindowMismatch, Unpublished };
// MatchOnly authenticates the retained input against caller-supplied evidence,
// never the live relocated text/GOT or an actual function traversal. It is NOT
// a runtime patch authorization. No expected game identity is bundled here.
PreimageMatch VerifyPreimage(const Record &record, const ExpectedPreimage &expected) noexcept;
enum class Result {
    Ok, Disabled, Busy, OwnerUnavailable, InvalidName, HashBudget,
    SegmentBounds, WindowBounds, StaleOwner, MappingBounds, Full, NotFound,
    ResolverProtocolUnavailable, AllocationFailure, Ambiguous
};
struct Pending {
    Record record;
    uint64_t configurationEpoch{};
};
// Default false. No JNI/UI or game interception enables this experiment.
// Configuration and capture belong to ordinary loader/testing threads.
bool Enabled() noexcept;
Result Configure(bool enabled, std::span<const WindowRequest> windows = {}) noexcept;
std::unique_ptr<Pending> Capture(const Input &input, Result *status = nullptr) noexcept;
Result Publish(std::unique_ptr<Pending> pending, const std::shared_ptr<const void> &processOwner,
               uint64_t reservationBase, uint64_t reservationSize, uint64_t moduleBase) noexcept;
// Copies owned metadata only. Retains a weak process lifetime identity, never a
// host pointer or Guest span. A successful copy does not authorize dereferencing.
Result Snapshot(const ModuleOwner &owner, const std::shared_ptr<const void> &processOwner,
                std::string_view moduleName, Record &output) noexcept;
// There is no proven resolver-write/lifecycle protocol in current Pine.
// Never dereference GetChunk/GetHostSpan or an arbitrary Guest GOT address.
inline Result LiveGotCapability() noexcept { return Result::ResolverProtocolUnavailable; }
}
