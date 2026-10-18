// Host platform declarations only. Production Backing/RomFileSystem/IStorage
// are compiled unchanged; no mock FileSystem or Storage implementation is used.
#pragma once
#include "../../diagnostics/composite_stubs/common.h"
#include <string>
#include <string_view>
#include <map>
#include <unordered_map>
namespace skyline {
template<class It, class End> span(It, End) -> span<typename std::iterator_traits<It>::value_type>;
template<class Container> span(Container &) -> span<typename Container::value_type>;
template<class Container> span(const Container &) -> span<const typename Container::value_type>;
}
