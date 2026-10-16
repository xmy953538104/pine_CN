// Identity cipher: this test covers production read flow, not AES correctness.
#pragma once
#include <crypto/key_store.h>
inline constexpr int MBEDTLS_CIPHER_AES_128_CTR = 0;
namespace skyline::crypto {
struct AesCipher {
    AesCipher(const KeyStore::Key128 &, int) {}
    void SetIV(const std::array<u8, 16> &) {}
    void Decrypt(u8 *destination, u8 *source, size_t length) {
        if (destination != source) std::memmove(destination, source, length);
    }
    void Decrypt(span<u8>) {}
};
}
