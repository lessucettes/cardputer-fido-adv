#ifndef CF_BACKUP_MBEDTLS_CONFIG_H
#define CF_BACKUP_MBEDTLS_CONFIG_H
/* Minimal software build of the same vendored primitives used by firmware. */
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_GCM_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_PKCS5_C
#define MBEDTLS_DEPRECATED_REMOVED
#endif
