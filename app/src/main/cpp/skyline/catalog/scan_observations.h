// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace skyline::catalog { class Reader; }
namespace skyline::catalog::observed {
enum class FilePresence { NotChecked, Missing, Present, CheckFailed };
enum class FileRead { NotAttempted, Complete, Short, Failed, Unknown };
enum class FileLoad { NotAttempted, Skipped, Loaded, Failed };
enum class KeyLoad { NotStarted, Loading, Loaded, Failed };
struct KeyFileState {
    FilePresence presence{FilePresence::NotChecked};
    FileRead read{FileRead::NotAttempted};
    FileLoad load{FileLoad::NotAttempted};
};
struct KeyState {
    KeyLoad status{KeyLoad::NotStarted};
    KeyFileState titleKeys, prodKeys;
    std::optional<uint64_t> loadedUniqueTitleKeys, recognizedProdSlots;
    bool countsPartial{true};
};
enum class TitleKeyLookup { NotObserved, NotRequired, Matched, NotMatched };
class NativeKeyObserver {
  public:
    virtual ~NativeKeyObserver() = default;
    // Carries only a source identity and the outcome of a read-only map probe.
    // No Rights ID, key, hash or header is retained by this interface.
    virtual void Observe(const Reader *source, TitleKeyLookup state) noexcept = 0;
};
class KeyLookups final : public NativeKeyObserver {
    struct Entry { const Reader *source{}; TitleKeyLookup state{TitleKeyLookup::NotObserved}; };
    std::array<Entry, 32> entries{};
    size_t count{};
  public:
    void Observe(const Reader *source, TitleKeyLookup state) noexcept override {
        for (size_t i{}; i < count; ++i) if (entries[i].source == source) { entries[i].state = state; return; }
        // Only the first 32 member rows can ever be emitted. Keeping the first
        // 32 decoded-header probes covers every displayed decoded member even
        // when preceding members were skipped or had unreadable headers.
        if (count < entries.size()) entries[count++] = {source, state};
    }
    TitleKeyLookup For(const Reader *source) const noexcept {
        for (size_t i{}; i < count; ++i) if (entries[i].source == source) return entries[i].state;
        return TitleKeyLookup::NotObserved;
    }
};
}
