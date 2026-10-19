// In-memory test keys only. No key files or commercial material are read.
#pragma once
#include <common.h>
namespace skyline::crypto {
class KeyStore {
  public:
    using Key128 = std::array<u8, 16>;
    using Key256 = std::array<u8, 32>;
    using IndexedKeys128 = std::array<std::optional<Key128>, 20>;
    std::optional<Key256> headerKey;
    IndexedKeys128 titleKek{}, areaKeyApplication{}, areaKeyOcean{}, areaKeySystem{};
    std::optional<Key128> titleKey;
    std::optional<Key128> GetTitleKey(const Key128 &) { return titleKey; }
    bool ContainsTitleKey(const Key128 &) const noexcept { return titleKey.has_value(); }
};
}
