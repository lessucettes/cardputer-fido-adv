#ifndef CF_BACKUP_CODEC_H
#define CF_BACKUP_CODEC_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CF_BACKUP_OK = 0,
    CF_BACKUP_INVALID = -1,
    CF_BACKUP_IO = -2,
    CF_BACKUP_AUTH = -3,
    CF_BACKUP_MEMORY = -4,
    CF_BACKUP_CRYPTO = -5
};
#define CF_BACKUP_PASSWORD_MIN 12u
#define CF_BACKUP_PASSWORD_MAX 64u
#define CF_BACKUP_ITERATIONS 600000u
#define CF_BACKUP_CHUNK 4096u
#define CF_BACKUP_HEADER 80u

typedef int (*cf_backup_read_fn)(void *, uint32_t, uint8_t *, size_t);
typedef int (*cf_backup_write_fn)(void *, uint32_t, const uint8_t *, size_t);
typedef int (*cf_backup_random_fn)(void *, uint8_t *, size_t);

/* Files are caller-owned and must be opened in binary mode. No plaintext is
 * written to the file. The device binding is public, not an encryption key.
 * Export requires 12..64 printable ASCII bytes; secrets are never logged. */
int cf_backup_export(FILE *file, const uint8_t *password, size_t password_len,
                     const uint8_t device[32], cf_backup_read_fn read,
                     void *ctx, cf_backup_random_fn random, void *random_ctx);
int cf_backup_verify(FILE *file, const uint8_t *password, size_t password_len,
                     const uint8_t device[32]);
/* Authenticates the entire file BEFORE the first write. Each chunk is checked
 * again while applying. Caller must persist a recovery guard before calling:
 * media/I/O/power failure after writes start requires retrying restoration. */
int cf_backup_restore(FILE *file, const uint8_t *password, size_t password_len,
                      const uint8_t device[32], cf_backup_write_fn write,
                      void *ctx);

#ifdef __cplusplus
}
#endif
#endif
