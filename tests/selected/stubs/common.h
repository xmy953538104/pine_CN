// Host platform/process types only. Real Loader, NCA, CNMT, NACP and NPDM are compiled.
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <sys/types.h>
namespace skyline {
using u8=uint8_t; using u16=uint16_t; using u32=uint32_t; using u64=uint64_t;
using i8=int8_t; using i16=int16_t; using i32=int32_t; using i64=int64_t;
struct exception : std::runtime_error {
    template<class... Args> exception(const char *message, Args...) : std::runtime_error(message) {}
};
template<class T> struct span : std::span<T> {
    using std::span<T>::span;
    span(std::span<T> value):std::span<T>(value){}
    span<T> first(size_t count) const { return std::span<T>::first(count); }
    span<T> subspan(size_t offset,size_t count=std::dynamic_extent) const{return std::span<T>::subspan(offset,count);}
    template<class In> void copy_from(const In &input){const auto count=input.size()*sizeof(typename In::value_type); if(count>this->size_bytes())throw exception("span copy");std::memmove(this->data(),input.data(),count);}
    template<class Out> span<Out> cast(){if(this->size_bytes()%sizeof(Out))throw exception("span cast size");return{reinterpret_cast<Out*>(this->data()),this->size_bytes()/sizeof(Out)};}
    std::string_view as_string(bool nul)const{size_t count=this->size(); if(nul){auto end=std::find(this->begin(),this->end(),0);count=static_cast<size_t>(end-this->begin());}return{reinterpret_cast<const char*>(this->data()),count};}
};
template<class T>span(std::vector<T>&)->span<T>;
template<class T,size_t N>span(std::array<T,N>&)->span<T>;
template<class T>span(T*,size_t)->span<T>;
namespace util {
template<class T>constexpr T MakeMagic(const char*text){T value{};for(size_t i{};i<sizeof(T);++i)value|=static_cast<T>(static_cast<uint8_t>(text[i]))<<(8*i);return value;}
template<class T>T SwapEndianness(T value){T result{};for(size_t i{};i<sizeof(T);++i,value>>=8)result=static_cast<T>((result<<8)|(value&0xff));return result;}
inline bool IsPageAligned(uint64_t value){return value%0x1000==0;}
}
namespace loader {class Loader;}
namespace kernel {class OS; namespace type {class KProcess;}}
struct DeviceState {
    kernel::OS *os{};
    std::shared_ptr<loader::Loader> loader,updateLoader;
    std::vector<std::shared_ptr<loader::Loader>>dlcLoaders;
    std::shared_ptr<kernel::type::KProcess>process;
};
}
namespace fmt {
template<class T>std::string format(const char *pattern,T value){std::ostringstream output;if(std::string_view(pattern)=="{:016X}")output<<std::uppercase<<std::hex<<std::setw(16)<<std::setfill('0');output<<value;return output.str();}
}
#define LOGW(...) ((void)0)
#define LOGE(...) ((void)0)
#define LOGINF(...) ((void)0)
