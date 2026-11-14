#ifndef CF_BACKUP_STORE_H
#define CF_BACKUP_STORE_H
#include "backup_codec.h"
#include "key_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *ctx;
    cf_backup_read_fn read;
    cf_backup_write_fn program;
    int (*erase)(void *, uint32_t, size_t);
    uint32_t next;
    uint8_t keys[CF_KEY_PAGE_COUNT * CF_KEY_PAGE_SIZE];
    uint8_t check[CF_BACKUP_CHUNK];
} cf_backup_store;

/* Use only in pre-USB maintenance mode, with a persistent restore guard set.
 * Key pages are committed last; a failed apply must NEVER resume normal boot.
 * Caller zeroizes this object after use. */
int cf_backup_store_write(void *ctx, uint32_t offset, const uint8_t *data, size_t len);
int cf_backup_store_finish(cf_backup_store *store);

#ifdef __cplusplus
}
#endif
#endif
