// NOT CRYPTOGRAPHY. XTS is a reversible XOR test marker; ECB/CTR are identity.
// This tests real NCA/Backing/PFS calls and exceptions, never AES correctness.
#pragma once
#include <crypto/key_store.h>
inline constexpr int MBEDTLS_CIPHER_AES_128_XTS = 1;
inline constexpr int MBEDTLS_CIPHER_AES_128_ECB = 2;
inline constexpr int MBEDTLS_CIPHER_AES_128_CTR = 3;
namespace skyline::crypto {
inline size_t testXtsCalls{}, testCtrCalls{};
class AesCipher {
    int type;
  public:
    template<class Key> AesCipher(const Key &, int pType) : type(pType) {}
    void SetIV(const std::array<u8, 16> &) {}
    void Decrypt(u8 *out, u8 *in, size_t size) {
        if (type == MBEDTLS_CIPHER_AES_128_CTR) ++testCtrCalls;
        if (out != in) std::memmove(out, in, size);
    }
    void Decrypt(span<u8> data) { Decrypt(data.data(), data.data(), data.size()); }
    void XtsDecrypt(u8 *out, u8 *in, size_t size, size_t, size_t) {
        ++testXtsCalls;
        for (size_t i{}; i < size; ++i) out[i] = in[i] ^ 0xa5;
    }
    void XtsDecrypt(span<u8> data, size_t sector, size_t sectorSize) { XtsDecrypt(data.data(), data.data(), data.size(), sector, sectorSize); }
};
}
