#ifndef CF_KEY_STORE_H
#define CF_KEY_STORE_H

#include <stddef.h>
#include <stdint.h>

#define CF_KEY_PAGE_SIZE 4096u
#define CF_KEY_PAGE_COUNT 2u
#define CF_KEY_RECORD_SIZE 88u
#define CF_KEY_PAYLOAD_SIZE 64u
#define CF_KEY_VERSION 1u
#define CF_KEY_COMMIT_MARKER 0x43464B31u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CF_KEY_EMPTY = 0,
    CF_KEY_READY = 1,
    CF_KEY_CORRUPT = 2,
    CF_KEY_IO_ERROR = 3,
    CF_KEY_INVALID_ARGUMENT = 4
} cf_key_status;

/*
 * The flash adapter owns the two key pages. Offsets are relative to the first
 * key page. Erase and program must behave like NOR flash: erase sets bytes to
 * 0xFF and programming can only clear bits.
 */
typedef struct {
    void *ctx;
    int (*read)(void *ctx, uint32_t offset, uint8_t *out, size_t len);
    int (*program)(void *ctx, uint32_t offset, const uint8_t *data, size_t len);
    int (*erase)(void *ctx, uint32_t offset, size_t len);
    /* Returns 1 only when the entire credential filesystem is erased. */
    int (*database_erased)(void *ctx);
    /* Returns 0 and fills len bytes on success. */
    int (*random)(void *ctx, uint8_t *out, size_t len);
} cf_key_flash;

typedef struct {
    uint8_t key1[32];
    uint8_t key2[32];
} cf_device_keys;

uint32_t cf_key_crc32(const uint8_t *data, size_t len);

/* Serializes the fixed record and fills the unused tail with 0xFF. */
void cf_key_record_encode(const cf_device_keys *keys,
                          uint8_t record[CF_KEY_PAGE_SIZE]);

/*
 * Decodes one record. The commit marker, CRC, version, payload length and the
 * secp256k1 scalar range are all validated. Output is zeroized on failure.
 */
cf_key_status cf_key_record_decode(const uint8_t record[CF_KEY_PAGE_SIZE],
                                   cf_device_keys *out);

/*
 * Reads both mirrored pages and applies the recovery policy. This function
 * never writes to flash.
 */
cf_key_status cf_key_store_load(const cf_key_flash *flash, cf_device_keys *out);

/*
 * Provisions a new identity only when both key pages and the credential
 * filesystem are erased. A committed copy is verified before the output is
 * published. Existing or corrupt data is never overwritten.
 */
cf_key_status cf_key_store_provision(const cf_key_flash *flash,
                                     cf_device_keys *out);

#ifdef __cplusplus
}
#endif

#endif
