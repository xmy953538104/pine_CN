// Minimal platform dependencies for compiling the production Backing in isolation.
#pragma once
#include <cstdint>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <sys/types.h>
namespace skyline {
using u8 = uint8_t;
using u32 = uint32_t;
template<class T> using span = std::span<T>;
struct exception : std::runtime_error {
    template<class... T> exception(const char *message, T&&...) : std::runtime_error(message) {}
};
inline int backingWarnings{};
}
#define LOGW(...) (++::skyline::backingWarnings)
