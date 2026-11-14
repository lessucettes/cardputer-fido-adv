#include "backup_codec.h"

#include <stdlib.h>
#include <string.h>
#include "flash_layout.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/platform_util.h"
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#endif

/* Version 1, all integers little endian:
 * 0 magic[8], 8 version(u32), 12 store size(u32), 16 chunk size(u32),
 * 20 PBKDF2 iterations(u32), 24 salt[16], 40 nonce prefix[8], 48 device[32].
 * Each record is 4096 ciphertext bytes + 16-byte GCM tag. Its nonce is the
 * random 8-byte prefix || LE32(index); AAD is header || LE32(index).
 * Exact fixed lengths prevent allocation/KDF amplification by untrusted SD.
 */
static const uint8_t magic[8] = {'C', 'F', 'B', 'A', 'K', '0', '0', '1'};

typedef struct {
    mbedtls_gcm_context gcm;
    uint8_t header[CF_BACKUP_HEADER];
    uint8_t aad[CF_BACKUP_HEADER + 4];
    uint8_t nonce[12];
    uint8_t key[32];
    uint8_t plain[CF_BACKUP_CHUNK];
    uint8_t cipher[CF_BACKUP_CHUNK];
    uint8_t tag[16];
} backup_context;

static void put32(uint8_t *out, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) {
        out[i] = (uint8_t)(n >> (8 * i));
    }
}

static uint32_t get32(const uint8_t *in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static int password_valid(const uint8_t *password, size_t len, size_t minimum) {
    if (!password || len < minimum || len > CF_BACKUP_PASSWORD_MAX) {
        return 0;
    }
    for (size_t i = 0; i < len; ++i) {
        if (password[i] < 32 || password[i] > 126) {
            return 0;
        }
    }
    return 1;
}

static void release_context(backup_context *ctx) {
    mbedtls_gcm_free(&ctx->gcm);
    mbedtls_platform_zeroize(ctx, sizeof(*ctx));
    free(ctx);
}

static int derive_bytes(backup_context *ctx, const uint8_t *password, size_t len) {
    return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, password, len,
                ctx->header + 24, 16, CF_BACKUP_ITERATIONS, 32, ctx->key);
}

#ifdef ESP_PLATFORM
typedef struct {
    backup_context *ctx;
    const uint8_t *password;
    size_t len;
    SemaphoreHandle_t done;
    int result;
} kdf_job;

static void kdf_worker(void *arg) {
    kdf_job *job = arg;
    job->result = derive_bytes(job->ctx, job->password, job->len);
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}
#endif

static int derive(backup_context *ctx, const uint8_t *password, size_t len) {
    int result;
#ifdef ESP_PLATFORM
    /* PBKDF2 may run for seconds. At idle priority the scheduler can service
     * the idle watchdog; the main maintenance task sleeps, not spins. */
    kdf_job job = {ctx, password, len, xSemaphoreCreateBinary(), -1};
    if (!job.done) { return CF_BACKUP_MEMORY; }
    if (xTaskCreate(kdf_worker, "backup_kdf", 4096, &job, tskIDLE_PRIORITY, NULL) != pdPASS) {
        vSemaphoreDelete(job.done);
        return CF_BACKUP_MEMORY;
    }
    xSemaphoreTake(job.done, portMAX_DELAY);
    result = job.result;
    vSemaphoreDelete(job.done);
#else
    result = derive_bytes(ctx, password, len);
#endif
    if (result == 0) {
        result = mbedtls_gcm_setkey(&ctx->gcm, MBEDTLS_CIPHER_ID_AES, ctx->key, 256);
    }
    mbedtls_platform_zeroize(ctx->key, sizeof(ctx->key));
    return result == 0 ? CF_BACKUP_OK : CF_BACKUP_CRYPTO;
}

static void record_context(backup_context *ctx, uint32_t index) {
    memcpy(ctx->aad, ctx->header, CF_BACKUP_HEADER);
    put32(ctx->aad + CF_BACKUP_HEADER, index);
    memcpy(ctx->nonce, ctx->header + 40, 8);
    put32(ctx->nonce + 8, index);
}

static int read_header(FILE *file, backup_context *ctx, const uint8_t device[32]) {
    clearerr(file);
    if (fseek(file, 0, SEEK_SET) != 0 ||
        fread(ctx->header, 1, CF_BACKUP_HEADER, file) != CF_BACKUP_HEADER) {
        return CF_BACKUP_IO;
    }
    if (memcmp(ctx->header, magic, sizeof(magic)) != 0 ||
        get32(ctx->header + 8) != 1 ||
        get32(ctx->header + 12) != CF_FIDO_STORE_SIZE ||
        get32(ctx->header + 16) != CF_BACKUP_CHUNK ||
        get32(ctx->header + 20) != CF_BACKUP_ITERATIONS ||
        memcmp(ctx->header + 48, device, 32) != 0) {
        return CF_BACKUP_INVALID;
    }
    return CF_BACKUP_OK;
}

static int scan(FILE *file, backup_context *ctx, cf_backup_write_fn write, void *arg) {
    for (uint32_t offset = 0; offset < CF_FIDO_STORE_SIZE; offset += CF_BACKUP_CHUNK) {
        if (fread(ctx->cipher, 1, CF_BACKUP_CHUNK, file) != CF_BACKUP_CHUNK ||
            fread(ctx->tag, 1, sizeof(ctx->tag), file) != sizeof(ctx->tag)) {
            return CF_BACKUP_IO;
        }
        record_context(ctx, offset / CF_BACKUP_CHUNK);
        if (mbedtls_gcm_auth_decrypt(&ctx->gcm, CF_BACKUP_CHUNK,
                ctx->nonce, sizeof(ctx->nonce), ctx->aad, sizeof(ctx->aad),
                ctx->tag, sizeof(ctx->tag), ctx->cipher, ctx->plain) != 0) {
            return CF_BACKUP_AUTH;
        }
        if (write && write(arg, offset, ctx->plain, CF_BACKUP_CHUNK) != 0) {
            return CF_BACKUP_IO;
        }
        mbedtls_platform_zeroize(ctx->plain, sizeof(ctx->plain));
    }
    if (fgetc(file) != EOF || ferror(file)) {
        return CF_BACKUP_INVALID;
    }
    return CF_BACKUP_OK;
}

int cf_backup_export(FILE *file, const uint8_t *password, size_t len,
                     const uint8_t device[32], cf_backup_read_fn read,
                     void *arg, cf_backup_random_fn random, void *random_arg) {
    if (!file || !device || !read || !random ||
        !password_valid(password, len, CF_BACKUP_PASSWORD_MIN)) {
        return CF_BACKUP_INVALID;
    }
    backup_context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return CF_BACKUP_MEMORY;
    }
    mbedtls_gcm_init(&ctx->gcm);
    memcpy(ctx->header, magic, sizeof(magic));
    put32(ctx->header + 8, 1);
    put32(ctx->header + 12, CF_FIDO_STORE_SIZE);
    put32(ctx->header + 16, CF_BACKUP_CHUNK);
    put32(ctx->header + 20, CF_BACKUP_ITERATIONS);
    memcpy(ctx->header + 48, device, 32);
    int result = CF_BACKUP_CRYPTO;
    if (random(random_arg, ctx->header + 24, 24) != 0) {
        goto done;
    }
    result = derive(ctx, password, len);
    if (result != CF_BACKUP_OK) {
        goto done;
    }
    result = CF_BACKUP_IO;
    if (fseek(file, 0, SEEK_SET) != 0 ||
        fwrite(ctx->header, 1, CF_BACKUP_HEADER, file) != CF_BACKUP_HEADER) {
        goto done;
    }
    for (uint32_t offset = 0; offset < CF_FIDO_STORE_SIZE; offset += CF_BACKUP_CHUNK) {
        if (read(arg, offset, ctx->plain, CF_BACKUP_CHUNK) != 0) {
            goto done;
        }
        record_context(ctx, offset / CF_BACKUP_CHUNK);
        if (mbedtls_gcm_crypt_and_tag(&ctx->gcm, MBEDTLS_GCM_ENCRYPT,
                CF_BACKUP_CHUNK, ctx->nonce, sizeof(ctx->nonce), ctx->aad,
                sizeof(ctx->aad), ctx->plain, ctx->cipher,
                sizeof(ctx->tag), ctx->tag) != 0) {
            result = CF_BACKUP_CRYPTO;
            goto done;
        }
        mbedtls_platform_zeroize(ctx->plain, sizeof(ctx->plain));
        if (fwrite(ctx->cipher, 1, CF_BACKUP_CHUNK, file) != CF_BACKUP_CHUNK ||
            fwrite(ctx->tag, 1, sizeof(ctx->tag), file) != sizeof(ctx->tag)) {
            goto done;
        }
    }
    result = fflush(file) == 0 ? CF_BACKUP_OK : CF_BACKUP_IO;
done:
    release_context(ctx);
    return result;
}

static int consume(FILE *file, const uint8_t *password, size_t len,
                   const uint8_t device[32], cf_backup_write_fn write, void *arg) {
    if (!file || !device || !password_valid(password, len, 1)) {
        return CF_BACKUP_INVALID;
    }
    backup_context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return CF_BACKUP_MEMORY;
    }
    mbedtls_gcm_init(&ctx->gcm);
    int result = read_header(file, ctx, device);
    if (result == CF_BACKUP_OK) {
        result = derive(ctx, password, len);
    }
    if (result == CF_BACKUP_OK) {
        result = scan(file, ctx, NULL, NULL);
    }
    if (result == CF_BACKUP_OK && write) {
        /* Keep the original header and derived key: never accept another
         * envelope if removable media changes between verification and use. */
        uint8_t header[CF_BACKUP_HEADER];
        clearerr(file);
        if (fseek(file, 0, SEEK_SET) != 0 ||
            fread(header, 1, sizeof(header), file) != sizeof(header) ||
            memcmp(header, ctx->header, sizeof(header)) != 0) {
            result = CF_BACKUP_INVALID;
        } else {
            result = scan(file, ctx, write, arg);
        }
    }
    release_context(ctx);
    return result;
}

int cf_backup_verify(FILE *file, const uint8_t *password, size_t len,
                     const uint8_t device[32]) {
    return consume(file, password, len, device, NULL, NULL);
}

int cf_backup_restore(FILE *file, const uint8_t *password, size_t len,
                      const uint8_t device[32], cf_backup_write_fn write, void *arg) {
    return write ? consume(file, password, len, device, write, arg) : CF_BACKUP_INVALID;
}
