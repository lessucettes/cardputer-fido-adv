/*
 * Upstream resident-credential adapter for the local Saved Keys screen.
 *
 * This file lives in the fido component because it reuses the protocol
 * credential structures. The UI never receives private scalars, seeds or raw
 * credential blobs: only a SHA-256 identity and display strings cross the
 * boundary.
 */
#include "credential_service.h"

#include <string.h>

#include "credential.h"
#include "fido.h"
#include "files.h"
#include "flash.h"
#include "mbedtls/sha256.h"

static void copy_display_text(char *out, size_t cap, const uint8_t *data,
                              size_t len) {
    if (out == NULL || cap == 0) {
        return;
    }
    size_t written = 0;
    for (size_t i = 0; i < len && written + 1 < cap; ++i) {
        const uint8_t ch = data[i];
        out[written++] = (ch >= 0x20 && ch != 0x7F) ? (char)ch : ' ';
    }
    out[written] = '\0';
}

static int credential_identity(const Credential *cred, uint8_t out[32]) {
    const uint8_t *source = NULL;
    size_t length = 0;
    if (cred->id.present && cred->id.len > 0) {
        source = cred->id.data;
        length = cred->id.len;
    } else if (cred->residentId.present && cred->residentId.len > 0) {
        source = cred->residentId.data;
        length = cred->residentId.len;
    }
    if (source == NULL || length == 0) {
        return -1;
    }
    return mbedtls_sha256(source, length, out, 0) == 0 ? 0 : -1;
}

static int upstream_list(void *ctx, cf_credential_item *items, size_t capacity,
                         size_t *total_out, int *truncated) {
    (void)ctx;
    size_t count = 0;
    int more = 0;
    for (int i = 0; i < MAX_RESIDENT_CREDENTIALS; ++i) {
        file_t *ef = file_search((uint16_t)(EF_CRED + i));
        if (!file_has_data(ef)) {
            continue;
        }
        uint8_t rp_hash[RP_ID_HASH_LEN];
        if (credential_resident_rp_id_hash(ef, rp_hash) != PICOKEYS_OK) {
            continue;
        }
        Credential cred = {0};
        if (credential_load_resident(ef, rp_hash, &cred) != 0) {
            credential_free(&cred);
            continue;
        }
        /* UV-required credentials remain visible as an opaque protected row.
         * Do not disclose their RP/account metadata here and never delete
         * them without host authorization. */
        const bool protected_key =
            cred.extensions.credProtect == CRED_PROT_UV_REQUIRED;
        uint8_t identity[32];
        if (credential_identity(&cred, identity) != 0) {
            credential_free(&cred);
            continue;
        }
        if (count < capacity) {
            cf_credential_item *item = &items[count];
            memset(item, 0, sizeof(*item));
            memcpy(item->id, identity, sizeof(item->id));
            item->id_len = sizeof(item->id);
            item->protected_key = protected_key ? 1 : 0;
            if (!protected_key) {
                if (cred.rpId.present && cred.rpId.data != NULL) {
                    copy_display_text(item->rp_id, sizeof(item->rp_id),
                                      (const uint8_t *)cred.rpId.data, cred.rpId.len);
                }
                if (cred.userName.present && cred.userName.data != NULL) {
                    copy_display_text(item->account, sizeof(item->account),
                                      (const uint8_t *)cred.userName.data,
                                      cred.userName.len);
                } else if (cred.userDisplayName.present &&
                           cred.userDisplayName.data != NULL) {
                    copy_display_text(item->account, sizeof(item->account),
                                      (const uint8_t *)cred.userDisplayName.data,
                                      cred.userDisplayName.len);
                }
            }
            ++count;
        } else {
            more = 1;
        }
        credential_free(&cred);
    }
    if (total_out != NULL) {
        *total_out = count + (more ? 1u : 0u);
    }
    if (truncated != NULL) {
        *truncated = more;
    }
    return 0;
}

static int upstream_remove(void *ctx, const uint8_t *id, size_t id_len) {
    (void)ctx;
    if (id == NULL || id_len != 32) {
        return 1;
    }
    for (int i = 0; i < MAX_RESIDENT_CREDENTIALS; ++i) {
        file_t *ef = file_search((uint16_t)(EF_CRED + i));
        if (!file_has_data(ef)) {
            continue;
        }
        uint8_t rp_hash[RP_ID_HASH_LEN];
        if (credential_resident_rp_id_hash(ef, rp_hash) != PICOKEYS_OK) {
            continue;
        }
        Credential cred = {0};
        if (credential_load_resident(ef, rp_hash, &cred) != 0) {
            credential_free(&cred);
            continue;
        }
        uint8_t identity[32];
        const int identified = credential_identity(&cred, identity);
        const bool protected_key =
            cred.extensions.credProtect == CRED_PROT_UV_REQUIRED;
        credential_free(&cred);
        if (identified != 0 || memcmp(identity, id, 32) != 0) {
            continue;
        }
        if (protected_key) {
            return -2;
        }
        const bool legacy_rp = !resident_container_is_marker(ef);
        if (credential_resident_delete(ef) != PICOKEYS_OK) {
            return -1;
        }
        if (legacy_rp) {
            const int rp_ret = credential_rp_legacy_decrement(rp_hash);
            if (rp_ret != PICOKEYS_OK && rp_ret != PICOKEYS_ERR_FILE_NOT_FOUND) {
                return -1;
            }
        }
        dev_state_update(DEV_STATE_CRED_STATE);
        flash_commit();
        return 0;
    }
    return 1;
}

cf_credential_backend cardputer_credentials_backend(void) {
    cf_credential_backend backend;
    backend.ctx = NULL;
    backend.list = upstream_list;
    backend.remove = upstream_remove;
    return backend;
}
