#pragma once
#include <common/base.h>
#include <array>
#include <vector>
#include <span>
#include <memory>
#include <string>
#include <algorithm>
#include <cstring>
#include <cassert>
#include <cstddef>
namespace skyline {
namespace util {
template<class T>constexpr T MakeMagic(const char*text){T value{};for(size_t i{};i<sizeof(T)&&text[i];++i)value|=T(uint8_t(text[i]))<<(8*i);return value;}
inline constexpr uint64_t ClockFrequency=19200000;
inline size_t AlignUp(size_t n,size_t alignment){return (n+alignment-1)&~(alignment-1);}
inline bool IsPageAligned(uint64_t n){return !(n&4095);}
}
namespace constant {inline constexpr size_t PageSize=4096;}
}
namespace skyline {
template<class T>struct span:std::span<T>{using std::span<T>::span;template<class V>span<V>cast(){return{reinterpret_cast<V*>(this->data()),this->size_bytes()/sizeof(V)};}};
template<class T>span(T*,size_t)->span<T>;
}
#define LOGD(...) ((void)0)
