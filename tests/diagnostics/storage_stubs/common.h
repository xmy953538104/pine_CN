// Minimal platform dependencies; Backing and IStorage themselves remain production code.
#pragma once
#include "../backing_stubs/common.h"
#include <array>
#include <memory>
#include <vector>
namespace skyline {
using u64 = uint64_t;
struct DeviceState {};
struct Result {
    unsigned module, description;
    constexpr Result(unsigned module = 0, unsigned description = 0) : module(module), description(description) {}
    bool operator==(const Result &) const = default;
};
}
