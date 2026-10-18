// Minimal configuration for the repository's pinned mbedTLS 2.16.11 C sources.
// Host synthetic tests only; the Android/default configuration is unchanged.
#ifndef PINE_CATALOG_NATIVE_AES_CONFIG_H
#define PINE_CATALOG_NATIVE_AES_CONFIG_H
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CTR
#define MBEDTLS_CIPHER_MODE_XTS
#endif
