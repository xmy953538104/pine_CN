// SPDX-License-Identifier: MPL-2.0
#include "romfs_detail.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace skyline::vfs::prototype {
    namespace detail {
        uint64_t Add(uint64_t a, uint64_t b) {
            if (b > UINT64_MAX - a) throw Invalid("integer_overflow");
            return a + b;
        }
        uint64_t Align(uint64_t value, uint64_t alignment) {
            if (!alignment || (alignment & (alignment - 1))) throw Invalid("invalid_alignment");
            return Add(value, alignment - 1) & ~(alignment - 1);
        }
        void CheckRange(uint64_t offset, uint64_t size, uint64_t limit) {
            if (offset > limit || size > limit - offset) throw Invalid("out_of_range");
        }
        void Exact(Backing &backing, uint64_t offset, uint8_t *out, size_t size) {
            CheckRange(offset, size, backing.size);
            if (offset > SIZE_MAX) throw Invalid("offset_not_addressable");
            if (backing.ReadUnchecked({out, size}, static_cast<size_t>(offset)) != size)
                throw std::runtime_error("virtual RomFS underlying source short read");
        }
        std::vector<uint8_t> Read(Backing &backing, uint64_t offset, uint64_t size) {
            CheckRange(offset, size, backing.size);
            if (size > SIZE_MAX) throw Invalid("size_not_addressable");
            std::vector<uint8_t> result(static_cast<size_t>(size));
            Exact(backing, offset, result.data(), result.size());
            return result;
        }
        uint64_t Little(const std::vector<uint8_t> &bytes, size_t offset, size_t width) {
            CheckRange(offset, width, bytes.size());
            uint64_t value{};
            if (width > 8) throw Invalid("invalid_integer_width");
            for (size_t i{}; i < width; ++i) value |= uint64_t{bytes[offset + i]} << (i * 8);
            return value;
        }
        void Put(std::vector<uint8_t> &bytes, uint64_t offset, uint64_t value, size_t width) {
            CheckRange(offset, width, bytes.size());
            if (width > 8) throw Invalid("invalid_integer_width");
            for (size_t i{}; i < width; ++i) bytes[static_cast<size_t>(offset) + i] = static_cast<uint8_t>(value >> (i * 8));
        }
        bool DigestValid(const std::string &digest) {
            return digest.size() == 64 && digest.find_first_not_of("0123456789abcdef") == std::string::npos;
        }
        std::string Fold(const std::string &name) {
            std::string result(name);
            for (auto &c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            return result;
        }
        std::string CanonicalPath(const std::string &path, const Limits &limits) {
            if (path.empty() || path.size() > limits.maxPathBytes || path.front() == '/' || path.back() == '/') throw Invalid("invalid_path");
            uint64_t depth{};
            for (size_t at{}; at < path.size();) {
                auto end = path.find('/', at); if (end == std::string::npos) end = path.size();
                auto name = path.substr(at, end - at);
                if (name.empty() || name == "." || name == ".." || name.size() > limits.maxNameBytes || name.back() == ' ' || name.back() == '.') throw Invalid("invalid_path");
                for (unsigned char c : name) if (c < 0x20 || c >= 0x7f || c == '\\' || c == ':') throw Invalid("unsupported_path_encoding");
                if (++depth > limits.maxDepth) throw Invalid("path_depth_budget");
                at = end + 1;
            }
            return path;
        }
        uint32_t PathHash(uint32_t parent, const std::string &name) {
            uint32_t hash = parent ^ 123456789U;
            for (unsigned char c : name) hash = ((hash >> 5) | (hash << 27)) ^ c;
            return hash;
        }

        namespace {
            class Sha256 {
                std::array<uint32_t,8> h{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
                std::array<uint8_t,64> block{};
                size_t used{};
                uint64_t length{};
                static uint32_t R(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
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
                    for (size_t i{}; i < 16; ++i) for (size_t j{}; j < 4; ++j) w[i] = (w[i] << 8) | block[i*4+j];
                    for (size_t i{16}; i < 64; ++i) w[i] = w[i-16] + (R(w[i-15],7)^R(w[i-15],18)^(w[i-15]>>3)) + w[i-7] + (R(w[i-2],17)^R(w[i-2],19)^(w[i-2]>>10));
                    auto a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],v=h[7];
                    for (size_t i{}; i < 64; ++i) {
                        auto t=v+(R(e,6)^R(e,11)^R(e,25))+((e&f)^(~e&g))+k[i]+w[i];
                        auto u=(R(a,2)^R(a,13)^R(a,22))+((a&b)^(a&c)^(b&c));
                        v=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;
                    }
                    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=v;
                }
              public:
                void Update(const uint8_t *data, size_t size) {
                    length = Add(length,size);
                    while(size) {
                        auto amount=std::min(size,block.size()-used); std::copy_n(data,amount,block.data()+used);
                        used+=amount; data+=amount; size-=amount;
                        if(used==64) { Transform(); used=0; }
                    }
                }
                std::string Finish() {
                    if(length>UINT64_MAX/8) throw Invalid("integer_overflow");
                    auto bits=length*8; const uint8_t marker=0x80,zero=0; Update(&marker,1);
                    while(used!=56) Update(&zero,1);
                    uint8_t tail[8]{};for(size_t i{};i<8;++i)tail[7-i]=static_cast<uint8_t>(bits>>(8*i));Update(tail,8);
                    constexpr char digits[]{"0123456789abcdef"};std::string out;out.reserve(64);
                    for(size_t i{};i<32;++i){auto x=static_cast<uint8_t>(h[i/4]>>(24-8*(i%4)));out+=digits[x>>4];out+=digits[x&15];}return out;
                }
            };
        }
        std::string HashBacking(Backing &backing) {
            Sha256 hash; std::array<uint8_t,65536> bytes{}; const auto size=backing.size;
            for(uint64_t at{};at<size;) {auto n=static_cast<size_t>(std::min<uint64_t>(size-at,bytes.size()));Exact(backing,at,bytes.data(),n);hash.Update(bytes.data(),n);at=Add(at,n);}
            if(backing.size!=size) throw std::runtime_error("virtual RomFS source size changed");
            return hash.Finish();
        }
        std::string HashPayload(const PayloadSource &source) {
            source.CheckUnchanged();
            Sha256 hash; std::array<uint8_t,65536> bytes{}; const auto size=source.Size();
            for(uint64_t at{};at<size;) {auto n=static_cast<size_t>(std::min<uint64_t>(size-at,bytes.size()));if(source.Read(at,bytes.data(),n)!=n)throw std::runtime_error("payload short read");hash.Update(bytes.data(),n);at=Add(at,n);}
            if(source.Size()!=size)throw std::runtime_error("payload size changed");
            source.CheckUnchanged();
            return hash.Finish();
        }
    }
    std::string BytesSha256(const uint8_t *data, size_t size) {
        // Use the same bounded streaming engine through a private immutable source.
        class Bytes final : public PayloadSource {
            const uint8_t *data; size_t size;
          public:
            Bytes(const uint8_t *data,size_t size):data(data),size(size){}
            uint64_t Size()const override{return size;}
            void CheckUnchanged()const override{}
            size_t Read(uint64_t at,uint8_t*out,size_t n)const override{detail::CheckRange(at,n,size);if(n)std::memcpy(out,data+at,n);return n;}
        } bytes(data,size);
        return detail::HashPayload(bytes);
    }
}
