// Platform stand-ins for compiling the real BKTR/Region/CTR/OsBacking implementations.
#pragma once
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <sys/types.h>
#include <vector>
namespace skyline {
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
struct exception : std::runtime_error {
    template<class... T> exception(const char *message, T&&...) : std::runtime_error(message) {}
};
inline int backingWarnings{};
// Match the platform span's conversions/copy API required by OsBacking's EFAULT path.
template<class T> struct span : std::span<T> {
    using std::span<T>::span;
    span(std::span<T> value) : std::span<T>(value) {}
    span<T> subspan(size_t offset, size_t count = std::dynamic_extent) const {
        return std::span<T>::subspan(offset, count);
    }
    template<class In> void copy_from(const In &input) {
        const auto amount = input.size() * sizeof(typename In::value_type);
        if (amount > this->size_bytes()) throw exception("Data being copied is larger than this span");
        std::memmove(this->data(), input.data(), amount);
    }
    template<class Out> span<Out> cast() {
        return {reinterpret_cast<Out *>(this->data()), this->size_bytes() / sizeof(Out)};
    }
};
struct DeviceState {};
struct Result {
    unsigned module, description;
    constexpr Result(unsigned module = 0, unsigned description = 0) : module(module), description(description) {}
    bool operator==(const Result &) const = default;
};
namespace util {
template<class T> T SwapEndianness(T value) {
    T result{};
    for (size_t i = 0; i < sizeof(T); ++i, value >>= 8)
        result = static_cast<T>((result << 8) | (value & 0xFF));
    return result;
}
}
}
#define LOGW(...) (++::skyline::backingWarnings)
#define LOGE(...) ((void)0)
