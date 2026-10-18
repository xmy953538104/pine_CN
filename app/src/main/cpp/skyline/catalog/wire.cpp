// SPDX-License-Identifier: MPL-2.0
#include "wire.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace skyline::catalog {
    Error::Error(std::string pCode, const std::string &message) : std::runtime_error(message), code(std::move(pCode)) {}
    uint64_t Add(uint64_t a, uint64_t b) {
        if (b > std::numeric_limits<uint64_t>::max() - a) throw Error("integer_overflow", "offset addition overflow");
        return a + b;
    }
    uint64_t Multiply(uint64_t a, uint64_t b) {
        if (a && b > std::numeric_limits<uint64_t>::max() / a) throw Error("integer_overflow", "count multiplication overflow");
        return a * b;
    }
    void Range(uint64_t offset, uint64_t size, uint64_t limit) {
        if (offset > limit || size > limit - offset) throw Error("out_of_range", "wire range exceeds its source");
    }
    void Reader::Exact(uint64_t offset, uint8_t *out, size_t size) const {
        Range(offset, size, Size());
        if (Read(offset, out, size) != size) throw Error("short_read", "source returned fewer bytes than requested");
    }
    std::vector<uint8_t> Reader::Bytes(uint64_t offset, size_t size) const {
        Range(offset, size, Size());
        std::vector<uint8_t> result(size);
        Exact(offset, result.data(), size);
        return result;
    }

    namespace {
        class RegionReader final : public Reader {
            Source parent;
            uint64_t offset, size;
          public:
            RegionReader(Source p, uint64_t o, uint64_t s) : parent(std::move(p)), offset(o), size(s) { Range(o, s, parent->Size()); }
            uint64_t Size() const override { return size; }
            size_t Read(uint64_t at, uint8_t *out, size_t count) const override {
                Range(at, count, size);
                return parent->Read(Add(offset, at), out, count);
            }
            void CheckUnchanged() const override { parent->CheckUnchanged(); }
            std::string SnapshotToken() const override { return parent->SnapshotToken(); }
        };

        class FileReader final : public Reader {
            int fd{-1};
            struct stat initial{};
            std::string path;
          public:
            explicit FileReader(const std::string &pPath) : path(std::filesystem::absolute(pPath).string()) {
                // A FIFO must not block before fstat can reject non-regular
                // input. O_NONBLOCK has no effect on ordinary file reads.
                fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
                if (fd < 0) throw Error("input_open_failed", "cannot open input read-only");
                if (fstat(fd, &initial) || !S_ISREG(initial.st_mode) || initial.st_size < 0) {
                    close(fd); fd = -1;
                    throw Error("input_not_regular", "input must be a regular file");
                }
            }
            ~FileReader() override { if (fd >= 0) close(fd); }
            uint64_t Size() const override { return static_cast<uint64_t>(initial.st_size); }
            size_t Read(uint64_t offset, uint8_t *out, size_t count) const override {
                Range(offset, count, Size());
                Range(offset, count, static_cast<uint64_t>(std::numeric_limits<off_t>::max()));
                size_t done{};
                while (done < count) {
                    auto amount = pread(fd, out + done, std::min(count - done, size_t{1024 * 1024}), static_cast<off_t>(offset + done));
                    if (amount < 0 && errno == EINTR) continue;
                    if (amount < 0) throw Error("input_read_failed", "read-only input read failed");
                    if (!amount) break;
                    done += static_cast<size_t>(amount);
                }
                return done;
            }
            void CheckUnchanged() const override {
                struct stat now{}, named{};
                if (fstat(fd, &now) || now.st_dev != initial.st_dev || now.st_ino != initial.st_ino || now.st_size != initial.st_size ||
                    now.st_mtim.tv_sec != initial.st_mtim.tv_sec || now.st_mtim.tv_nsec != initial.st_mtim.tv_nsec ||
                    now.st_ctim.tv_sec != initial.st_ctim.tv_sec || now.st_ctim.tv_nsec != initial.st_ctim.tv_nsec ||
                    lstat(path.c_str(), &named) || named.st_dev != initial.st_dev || named.st_ino != initial.st_ino)
                    throw Error("input_changed", "input changed during catalog scan");
            }
            std::string SnapshotToken() const override {
                return "posix-stat:" + std::to_string(initial.st_dev) + ":" + std::to_string(initial.st_ino) + ":" +
                    std::to_string(initial.st_size) + ":" + std::to_string(initial.st_mtim.tv_sec) + ":" + std::to_string(initial.st_mtim.tv_nsec) + ":" +
                    std::to_string(initial.st_ctim.tv_sec) + ":" + std::to_string(initial.st_ctim.tv_nsec);
            }
        };

        class Hash {
            std::array<uint32_t, 8> state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
            std::array<uint8_t, 64> block{};
            size_t used{};
            uint64_t length{};
            static uint32_t Rotate(uint32_t a, unsigned b) { return (a >> b) | (a << (32 - b)); }
            void Transform() {
                static constexpr uint32_t k[]{
                    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
                    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
                    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
                    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
                    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
                    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
                    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
                    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
                uint32_t w[64]{};
                for (size_t i{}; i < 16; ++i)
                    for (size_t j{}; j < 4; ++j) w[i] = (w[i] << 8) | block[i * 4 + j];
                for (size_t i{16}; i < 64; ++i) {
                    auto x = w[i - 15], y = w[i - 2];
                    w[i] = w[i - 16] + (Rotate(x,7) ^ Rotate(x,18) ^ (x >> 3)) + w[i - 7] + (Rotate(y,17) ^ Rotate(y,19) ^ (y >> 10));
                }
                auto a=state[0], b=state[1], c=state[2], d=state[3], e=state[4], f=state[5], g=state[6], h=state[7];
                for (size_t i{}; i < 64; ++i) {
                    auto t1 = h + (Rotate(e,6)^Rotate(e,11)^Rotate(e,25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
                    auto t2 = (Rotate(a,2)^Rotate(a,13)^Rotate(a,22)) + ((a & b) ^ (a & c) ^ (b & c));
                    h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
                }
                state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d; state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
            }
          public:
            void Update(const uint8_t *data, size_t size) {
                length = Add(length, size);
                while (size) {
                    auto count = std::min(size, block.size() - used);
                    std::copy_n(data, count, block.data() + used);
                    data += count; size -= count; used += count;
                    if (used == block.size()) { Transform(); used = 0; }
                }
            }
            std::array<uint8_t, 32> Finish() {
                if (length > UINT64_MAX / 8) throw Error("integer_overflow", "SHA256 bit length overflow");
                auto bits = length * 8;
                const uint8_t marker{0x80}, zero{};
                Update(&marker, 1);
                while (used != 56) Update(&zero, 1);
                uint8_t tail[8]{};
                for (size_t i{}; i < 8; ++i) tail[7 - i] = static_cast<uint8_t>(bits >> (i * 8));
                Update(tail, 8);
                std::array<uint8_t, 32> result{};
                for (size_t i{}; i < 32; ++i) result[i] = static_cast<uint8_t>(state[i / 4] >> (24 - 8 * (i % 4)));
                return result;
            }
        };
    }

    Source Region(Source parent, uint64_t offset, uint64_t size) { return std::make_shared<RegionReader>(std::move(parent), offset, size); }
    Source OpenFile(const std::string &path) { return std::make_shared<FileReader>(path); }

    uint64_t Little(const std::vector<uint8_t> &bytes, size_t at, size_t count) {
        if (count > 8) throw Error("invalid_integer_width", "wire integer is wider than 64 bits");
        Range(at, count, bytes.size());
        uint64_t result{};
        for (size_t i{}; i < count; ++i) result |= uint64_t{bytes[at + i]} << (i * 8);
        return result;
    }
    uint16_t U16(const std::vector<uint8_t> &b, size_t at) { return static_cast<uint16_t>(Little(b, at, 2)); }
    uint32_t U32(const std::vector<uint8_t> &b, size_t at) { return static_cast<uint32_t>(Little(b, at, 4)); }
    uint64_t U64(const std::vector<uint8_t> &b, size_t at) { return Little(b, at, 8); }
    std::string Hex(const uint8_t *bytes, size_t size) {
        static constexpr char digits[]{"0123456789abcdef"};
        std::string result; result.reserve(size * 2);
        for (size_t i{}; i < size; ++i) { result += digits[bytes[i] >> 4]; result += digits[bytes[i] & 15]; }
        return result;
    }
    std::string Hex64(uint64_t number) { std::ostringstream out; out << std::hex << std::setfill('0') << std::setw(16) << number; return out.str(); }
    std::string Json(const std::string &value) {
        std::string out{"\""};
        // Byte-preserving JSON avoids invalid UTF-8 from untrusted filenames.
        static constexpr char digits[]{"0123456789abcdef"};
        for (unsigned char c : value) {
            if (c == '\\' || c == '"') { out += '\\'; out += static_cast<char>(c); }
            else if (c < 0x20 || c >= 0x7f) { out += "\\u00"; out += digits[c >> 4]; out += digits[c & 15]; }
            else out += static_cast<char>(c);
        }
        return out + '"';
    }
    std::array<uint8_t, 32> Sha256(const uint8_t *bytes, size_t size) { Hash hash; hash.Update(bytes, size); return hash.Finish(); }
    std::array<uint8_t, 32> Sha256(const Reader &source) {
        Hash hash;
        std::array<uint8_t, 1024 * 64> buffer{};
        for (uint64_t offset{}; offset < source.Size();) {
            auto count = static_cast<size_t>(std::min<uint64_t>(source.Size() - offset, buffer.size()));
            source.Exact(offset, buffer.data(), count); hash.Update(buffer.data(), count); offset = Add(offset, count);
        }
        return hash.Finish();
    }

    std::vector<PfsEntry> ReadPfs(Source source, const Limits &limits) {
        const auto header = source->Bytes(0, 0x10);
        if (std::memcmp(header.data(), "PFS0", 4)) throw Error("unsupported_pfs_format", "expected PFS0");
        auto count = U32(header, 4), stringSize = U32(header, 8);
        if (U32(header, 12)) throw Error("invalid_pfs_reserved", "nonzero PFS0 reserved field");
        if (count > limits.maxPfsEntries || stringSize > limits.maxStringBytes) throw Error("scan_budget", "PFS0 table exceeds configured scan budget");
        auto stringOffset = Add(0x10, Multiply(count, 0x18)), dataOffset = Add(stringOffset, stringSize);
        Range(0, dataOffset, source->Size());
        const auto names = source->Bytes(stringOffset, stringSize);
        std::vector<PfsEntry> result;
        std::set<std::string> unique;
        uint64_t totalNames{};
        for (uint32_t i{}; i < count; ++i) {
            const auto entry = source->Bytes(Add(0x10, Multiply(i, 0x18)), 0x18);
            auto nameOffset = U32(entry, 16);
            if (nameOffset >= names.size()) throw Error("invalid_pfs_name", "name offset exceeds string table");
            auto windowEnd = names.begin() + nameOffset + std::min<uint64_t>(names.size() - nameOffset, uint64_t{limits.maxNameBytes} + 1);
            auto end = std::find(names.begin() + nameOffset, windowEnd, uint8_t{});
            if (end == windowEnd) throw Error("invalid_pfs_name", "unterminated or over-budget PFS0 filename");
            totalNames = Add(totalNames, static_cast<uint64_t>(end - (names.begin() + nameOffset)));
            if (totalNames > limits.maxTotalNameBytes) throw Error("scan_budget", "aggregate PFS0 names exceed configured budget");
            std::string name(names.begin() + nameOffset, end);
            if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
                throw Error("invalid_pfs_name", "PFS0 requires a nonempty flat filename");
            if (!unique.insert(name).second) throw Error("duplicate_pfs_name", "duplicate PFS0 filename");
            if (U32(entry, 20)) throw Error("invalid_pfs_reserved", "nonzero PFS0 entry reserved field");
            auto offset = Add(dataOffset, U64(entry, 0)), size = U64(entry, 8);
            Range(offset, size, source->Size());
            result.push_back({name, offset, size, Region(source, offset, size)});
        }
        std::vector<std::pair<uint64_t, uint64_t>> regions;
        for (const auto &entry : result) if (entry.size) regions.emplace_back(entry.offset, Add(entry.offset, entry.size));
        std::sort(regions.begin(), regions.end());
        for (size_t i{1}; i < regions.size(); ++i)
            if (regions[i].first < regions[i - 1].second) throw Error("overlapping_pfs_entries", "PFS0 file regions overlap");
        return result;
    }
}
