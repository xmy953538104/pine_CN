// SPDX-License-Identifier: MPL-2.0
#include "nso_image.h"
#include <algorithm>
#include <cstring>

namespace skyline::diagnostics::image {
namespace {
std::atomic<bool> enabled{};
struct Entry { Record record; std::weak_ptr<const void> process; bool used{}; };
struct Registry {
    std::mutex mutex;
    uint64_t epoch{};
    std::array<WindowRequest, MaxWindows> requests{};
    size_t requestCount{};
    std::array<Entry, MaxModules> entries;
};
Registry &Get() { static Registry registry; return registry; }
bool SameProcess(const std::weak_ptr<const void> &a, const std::shared_ptr<const void> &b) {
    return !a.owner_before(b) && !b.owner_before(a);
}
bool NameValid(std::string_view name) {
    if (name.empty() || name.size() >= 40) return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}
}
bool Enabled() noexcept { return enabled.load(std::memory_order_relaxed); }
PreimageMatch VerifyPreimage(const Record &record, const ExpectedPreimage &expected) noexcept {
    const auto known = [](const Digest &hash) {
        return std::any_of(hash.begin(), hash.end(), [](uint8_t byte) { return byte != 0; });
    };
    if (!known(expected.buildId) || !expected.windowCount || expected.windowCount > MaxWindows)
        return PreimageMatch::UnknownExpected;
    for (const auto &hash : expected.segmentHashes)
        if (!known(hash)) return PreimageMatch::UnknownExpected;
    for (size_t i{}; i < expected.windowCount; ++i)
        if (!known(expected.windows[i].hash) || !expected.windows[i].size || expected.windows[i].size > MaxWindowBytes)
            return PreimageMatch::UnknownExpected;
    if (record.buildId != expected.buildId) return PreimageMatch::BuildIdMismatch;
    for (size_t i{}; i < 3; ++i)
        if (record.segments[i].computedHash != expected.segmentHashes[i]) return PreimageMatch::SegmentMismatch;
    if (record.windowCount != expected.windowCount) return PreimageMatch::WindowMismatch;
    for (size_t i{}; i < expected.windowCount; ++i) {
        const auto &actual = record.windows[i];
        const auto &target = expected.windows[i];
        if (actual.va != target.va || actual.size != target.size || actual.hash != target.hash ||
            Sha256(std::span(actual.bytes).first(actual.size)) != target.hash)
            return PreimageMatch::WindowMismatch;
    }
    if (!record.moduleBase || !record.loadReservationSize) return PreimageMatch::Unpublished;
    return PreimageMatch::MatchOnly;
}
Result Configure(bool value, std::span<const WindowRequest> windows) noexcept {
    if (windows.size() > MaxWindows) return Result::WindowBounds;
    for (auto request : windows)
        if (!request.size || request.size > MaxWindowBytes || UINT32_MAX - request.va < request.size)
            return Result::WindowBounds;
    try {
        auto &registry = Get();
        std::unique_lock lock(registry.mutex, std::try_to_lock);
        if (!lock) return Result::Busy;
        enabled.store(false, std::memory_order_relaxed);
        ++registry.epoch;
        for (auto &entry : registry.entries) entry = {};
        registry.requestCount = windows.size();
        std::copy(windows.begin(), windows.end(), registry.requests.begin());
        enabled.store(value, std::memory_order_release);
        return Result::Ok;
    } catch (...) { return Result::Busy; }
}
std::unique_ptr<Pending> Capture(const Input &input, Result *status) noexcept {
    const auto fail = [&](Result result) -> std::unique_ptr<Pending> {
        if (status) *status = result;
        return {};
    };
    if (!Enabled()) return fail(Result::Disabled);
    try {
        ModuleOwner owner;
        if (!TryModuleOwner(owner)) return fail(Result::OwnerUnavailable);
        if (!NameValid(input.name)) return fail(Result::InvalidName);
        std::array<WindowRequest, MaxWindows> requests;
        size_t count{};
        uint64_t epoch{};
        {
            auto &registry = Get();
            std::unique_lock lock(registry.mutex, std::try_to_lock);
            if (!lock) return fail(Result::Busy);
            if (!Enabled()) return fail(Result::Disabled);
            count = registry.requestCount; requests = registry.requests; epoch = registry.epoch;
        }
        size_t budget = MaxHashBytes;
        for (const auto &segment : input.segments) {
            if (segment.size > segment.contents.size() || UINT32_MAX - segment.va < segment.size)
                return fail(Result::SegmentBounds);
            if (segment.size > budget) return fail(Result::HashBudget);
            budget -= segment.size;
        }
        auto pending = std::make_unique<Pending>();
        pending->configurationEpoch = epoch;
        auto &record = pending->record;
        record.owner = owner; record.buildId = input.buildId;
        std::copy(input.name.begin(), input.name.end(), record.moduleName.begin());
        for (size_t i{}; i < 3; ++i) {
            const auto &source = input.segments[i];
            auto &segment = record.segments[i];
            segment.va = source.va; segment.size = source.size;
            segment.computedHash = Sha256(source.contents.first(source.size));
            segment.declaredHash = source.declaredHash; segment.hashDeclared = source.hashDeclared;
            segment.hashMatches = source.hashDeclared && source.declaredHash == segment.computedHash;
        }
        for (size_t i{}; i < count; ++i) {
            const auto request = requests[i];
            const SegmentInput *match{};
            for (const auto &segment : input.segments) {
                if (request.va >= segment.va && request.va - segment.va <= segment.size &&
                    request.size <= segment.size - (request.va - segment.va)) {
                    if (match) return fail(Result::WindowBounds); // overlapping segments
                    match = &segment;
                }
            }
            if (!match) return fail(Result::WindowBounds);
            auto bytes = match->contents.subspan(request.va - match->va, request.size);
            auto &window = record.windows[record.windowCount++];
            window.va = request.va; window.size = request.size; window.hash = Sha256(bytes);
            std::copy(bytes.begin(), bytes.end(), window.bytes.begin());
        }
        if (status) *status = Result::Ok;
        return pending;
    } catch (...) { return fail(Result::AllocationFailure); }
}
Result Publish(std::unique_ptr<Pending> pending, const std::shared_ptr<const void> &processOwner,
               uint64_t reservationBase, uint64_t reservationSize, uint64_t moduleBase) noexcept {
    if (!Enabled()) return Result::Disabled;
    if (!pending || !processOwner) return Result::OwnerUnavailable;
    try {
        ModuleOwner owner;
        if (!TryModuleOwner(owner)) return Result::OwnerUnavailable;
        if (owner != pending->record.owner) return Result::StaleOwner;
        if (!moduleBase || !reservationSize || UINT64_MAX - reservationBase < reservationSize ||
            moduleBase < reservationBase || moduleBase >= reservationBase + reservationSize)
            return Result::MappingBounds;
        for (auto &segment : pending->record.segments) {
            if (UINT64_MAX - moduleBase < segment.va) return Result::MappingBounds;
            segment.mappedStart = moduleBase + segment.va;
            if (segment.mappedStart < reservationBase || segment.mappedStart > reservationBase + reservationSize ||
                segment.size > reservationBase + reservationSize - segment.mappedStart) return Result::MappingBounds;
        }
        pending->record.moduleBase = moduleBase;
        pending->record.loadReservationBase = reservationBase;
        pending->record.loadReservationSize = reservationSize;
        auto &registry = Get();
        std::unique_lock lock(registry.mutex, std::try_to_lock);
        if (!lock) return Result::Busy;
        if (!Enabled() || registry.epoch != pending->configurationEpoch) return Result::StaleOwner;
        for (auto &entry : registry.entries) {
            if (!entry.used || entry.process.expired() || entry.record.owner != owner) {
                entry.record = pending->record; entry.process = processOwner; entry.used = true;
                return Result::Ok;
            }
        }
        return Result::Full; // bounded, no eviction of another module of this launch
    } catch (...) { return Result::Busy; }
}
Result Snapshot(const ModuleOwner &owner, const std::shared_ptr<const void> &processOwner,
                std::string_view moduleName, Record &output) noexcept {
    if (!Enabled()) return Result::Disabled;
    try {
        ModuleOwner current;
        if (!processOwner || !TryModuleOwner(current) || current != owner) return Result::StaleOwner;
        auto &registry = Get();
        std::unique_lock lock(registry.mutex, std::try_to_lock);
        if (!lock) return Result::Busy;
        if (!Enabled()) return Result::Disabled;
        const Record *match{};
        for (const auto &entry : registry.entries) {
            if (entry.used && entry.record.owner == owner && !entry.process.expired() &&
                SameProcess(entry.process, processOwner) &&
                std::string_view(entry.record.moduleName.data()) == moduleName) {
                if (match) return Result::Ambiguous;
                match = &entry.record;
            }
        }
        if (match) { output = *match; return Result::Ok; }
        return Result::NotFound;
    } catch (...) { return Result::Busy; }
}
}
