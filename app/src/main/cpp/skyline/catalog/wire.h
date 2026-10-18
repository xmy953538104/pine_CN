// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace skyline::catalog {
    // All offsets are unsigned wire offsets. Readers must return the actual byte
    // count, not a backing's total size. No read or region can escape its source.
    class Error : public std::runtime_error {
      public:
        std::string code;
        Error(std::string code, const std::string &message);
    };

    uint64_t Add(uint64_t a, uint64_t b);
    uint64_t Multiply(uint64_t a, uint64_t b);
    void Range(uint64_t offset, uint64_t size, uint64_t limit);

    class Reader {
      public:
        virtual ~Reader() = default;
        virtual uint64_t Size() const = 0;
        virtual size_t Read(uint64_t offset, uint8_t *out, size_t size) const = 0;
        virtual void CheckUnchanged() const {}
        virtual std::string SnapshotToken() const { return {}; }
        virtual std::string IdentityKind() const { return "unknown"; }
        virtual std::string IdentityScope() const { return "identity_unavailable; not_a_content_hash"; }
        void Exact(uint64_t offset, uint8_t *out, size_t size) const;
        std::vector<uint8_t> Bytes(uint64_t offset, size_t size) const;
    };
    using Source = std::shared_ptr<const Reader>;

    Source Region(Source parent, uint64_t offset, uint64_t size);
    Source OpenFile(const std::string &path);
    // Own a CLOEXEC duplicate of a borrowed readable regular-file descriptor.
    // Uses pread without changing its shared file offset. Stat checks bind only
    // the owned descriptor, not a named path/inode or immutable content hash.
    Source DuplicateFd(int borrowedFd);

    struct Limits {
        uint32_t maxPfsEntries{65536};
        uint32_t maxStringBytes{8 * 1024 * 1024};
        uint32_t maxNameBytes{1024};
        uint64_t maxTotalNameBytes{8 * 1024 * 1024};
        uint32_t maxCnmtFiles{4096};
        uint32_t maxContentReferences{65536};
        uint32_t maxMetaReferences{65536};
        uint64_t maxAssociationWork{1024 * 1024};
        uint32_t maxCnmtBytes{16 * 1024 * 1024};
        uint64_t maxMetadataBytes{128 * 1024 * 1024};
        uint64_t maxHashBytes{64ULL * 1024 * 1024 * 1024};
    };

    struct PfsEntry {
        std::string name;
        uint64_t offset{}; // relative to the PFS source, not the file-data area
        uint64_t size{};
        Source source;
    };
    std::vector<PfsEntry> ReadPfs(Source source, const Limits &limits);

    uint16_t U16(const std::vector<uint8_t> &bytes, size_t at);
    uint32_t U32(const std::vector<uint8_t> &bytes, size_t at);
    uint64_t U64(const std::vector<uint8_t> &bytes, size_t at);
    uint64_t Little(const std::vector<uint8_t> &bytes, size_t at, size_t count);
    std::string Hex(const uint8_t *bytes, size_t size);
    std::string Hex64(uint64_t number);
    std::string Json(const std::string &value);
    std::array<uint8_t, 32> Sha256(const uint8_t *bytes, size_t size);
    std::array<uint8_t, 32> Sha256(const Reader &source);
}
