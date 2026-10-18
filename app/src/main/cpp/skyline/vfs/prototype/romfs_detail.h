// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "virtual_romfs.h"
#include <stdexcept>

namespace skyline::vfs::prototype::detail {
    struct Invalid : std::runtime_error {
        std::string code;
        explicit Invalid(const std::string &code) : std::runtime_error(code), code(code) {}
    };
    uint64_t Add(uint64_t a, uint64_t b);
    uint64_t Align(uint64_t value, uint64_t alignment);
    void CheckRange(uint64_t offset, uint64_t size, uint64_t limit);
    void Exact(Backing &backing, uint64_t offset, uint8_t *out, size_t size);
    std::vector<uint8_t> Read(Backing &backing, uint64_t offset, uint64_t size);
    uint64_t Little(const std::vector<uint8_t> &bytes, size_t offset, size_t width);
    void Put(std::vector<uint8_t> &bytes, uint64_t offset, uint64_t value, size_t width);
    bool DigestValid(const std::string &digest);
    std::string HashBacking(Backing &backing);
    std::string HashPayload(const PayloadSource &source);
    std::string CanonicalPath(const std::string &path, const Limits &limits);
    std::string Fold(const std::string &name);
    uint32_t PathHash(uint32_t parent, const std::string &name);
}
