// Host platform stand-ins. Production Backing/NCA/PFS control flow is compiled.
#pragma once
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/types.h>
namespace skyline {
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
struct exception : std::runtime_error {
    template<class... Args> exception(const char *message, Args&&...) : std::runtime_error(message) {}
};
template<class T> struct span : std::span<T> {
    using std::span<T>::span;
    span(std::span<T> value) : std::span<T>(value) {}
    span<T> first(size_t count) const { return std::span<T>::first(count); }
    span<T> subspan(size_t offset, size_t count = std::dynamic_extent) const { return std::span<T>::subspan(offset, count); }
    template<class In> void copy_from(const In &input) {
        const auto size = input.size() * sizeof(typename In::value_type);
        if (size > this->size_bytes()) throw exception("span copy exceeds output");
        std::memmove(this->data(), input.data(), size);
    }
    template<class Out> span<Out> cast() { return {reinterpret_cast<Out *>(this->data()), this->size_bytes() / sizeof(Out)}; }
};
template<class T> span(std::vector<T> &) -> span<T>;
template<class T> span(T *, size_t) -> span<T>;
namespace util {
template<class T> constexpr T MakeMagic(const char *text) {
    T value{};
    for (size_t i{}; i < sizeof(T); ++i) value |= static_cast<T>(static_cast<uint8_t>(text[i])) << (8 * i);
    return value;
}
template<class T> T SwapEndianness(T value) {
    T result{};
    for (size_t i{}; i < sizeof(T); ++i, value >>= 8) result = static_cast<T>((result << 8) | (value & 0xff));
    return result;
}
}
}
#define LOGW(...) ((void)0)
#define LOGE(...) ((void)0)
