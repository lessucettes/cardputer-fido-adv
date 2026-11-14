#include "backup_store.h"
#include "flash_layout.h"
#include <string.h>
#include "mbedtls/platform_util.h"

static int valid_keys(cf_backup_store *store) {
    cf_device_keys a = {0}, b = {0};
    const cf_key_status sa = cf_key_record_decode(store->keys, &a);
    const cf_key_status sb = cf_key_record_decode(store->keys + CF_KEY_PAGE_SIZE, &b);
    const int result = (sa == CF_KEY_READY || sb == CF_KEY_READY) &&
        !(sa == CF_KEY_READY && sb == CF_KEY_READY && memcmp(&a, &b, sizeof(a)) != 0);
    mbedtls_platform_zeroize(&a, sizeof(a));
    mbedtls_platform_zeroize(&b, sizeof(b));
    return result;
}

static int program_checked(cf_backup_store *store, uint32_t offset,
                           const uint8_t *data, size_t len) {
    if (store->program(store->ctx, offset, data, len) != 0 ||
        store->read(store->ctx, offset, store->check, len) != 0 ||
        memcmp(store->check, data, len) != 0) {
        return -1;
    }
    mbedtls_platform_zeroize(store->check, sizeof(store->check));
    return 0;
}

int cf_backup_store_write(void *ctx, uint32_t offset, const uint8_t *data, size_t len) {
    cf_backup_store *store = ctx;
    if (!store || !store->read || !store->program || !store->erase || !data ||
        len != CF_BACKUP_CHUNK || offset != store->next ||
        offset >= CF_FIDO_STORE_SIZE) {
        return -1;
    }
    if (offset < CF_FIDO_FS_OFFSET) {
        memcpy(store->keys + offset, data, len);
    } else {
        if (offset == CF_FIDO_FS_OFFSET) {
            if (!valid_keys(store) ||
                store->erase(store->ctx, 0, CF_FIDO_FS_OFFSET) != 0) {
                return -1;
            }
            /* An interrupted apply must look corrupt, never like a fresh
             * empty device that can silently provision a different identity. */
            static const uint8_t marker[8] = {'R','E','S','T','O','R','E','!'};
            if (program_checked(store, 0, marker, sizeof(marker)) != 0) {
                return -1;
            }
        }
        if (store->erase(store->ctx, offset, len) != 0 ||
            program_checked(store, offset, data, len) != 0) {
            return -1;
        }
    }
    store->next += (uint32_t)len;
    return 0;
}

int cf_backup_store_finish(cf_backup_store *store) {
    if (!store || store->next != CF_FIDO_STORE_SIZE ||
        !store->erase || !store->program || !store->read || !valid_keys(store)) {
        return -1;
    }
    if (store->erase(store->ctx, 0, CF_FIDO_FS_OFFSET) != 0) {
        return -1;
    }
    for (uint32_t offset = 0; offset < CF_FIDO_FS_OFFSET; offset += CF_KEY_PAGE_SIZE) {
        if (program_checked(store, offset, store->keys + offset, CF_KEY_PAGE_SIZE) != 0) {
            return -1;
        }
    }
    return 0;
}
