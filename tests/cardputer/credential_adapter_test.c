/* Exercise the real Saved Keys adapter with an in-memory resident backend.
 * Storage decoding is stubbed; identity hashing and UI service are production.
 */
#include "credential_service.h"
#include "credential.h"
#include "files.h"
#include "flash.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern cf_credential_backend cardputer_credentials_backend(void);

static file_t slots[2];
static bool present[2] = {true, true};
static uint8_t ids[2] = {1, 2};
static int removed = -1;
static int commits;

file_t *file_search(uint16_t fid) {
    if (fid < EF_CRED || fid >= EF_CRED + 2) {
        return NULL;
    }
    const unsigned index = fid - EF_CRED;
    return present[index] ? &slots[index] : NULL;
}

bool file_has_data(const file_t *file) { return file != NULL; }

int credential_resident_rp_id_hash(const file_t *file, uint8_t hash[RP_ID_HASH_LEN]) {
    (void)file;
    memset(hash, 0, RP_ID_HASH_LEN);
    return PICOKEYS_OK;
}

int credential_load_resident(const file_t *file, const uint8_t *hash, Credential *cred) {
    (void)hash;
    const unsigned index = file == &slots[0] ? 0 : 1;
    memset(cred, 0, sizeof(*cred));
    cred->id.present = true;
    cred->id.data = &ids[index];
    cred->id.len = 1;
    cred->extensions.credProtect = index == 0 ? CRED_PROT_UV_REQUIRED
                                             : CRED_PROT_UV_OPTIONAL;
    cred->rpId.present = true;
    cred->rpId.data = "example.com";
    cred->rpId.len = strlen(cred->rpId.data);
    cred->userName.present = true;
    cred->userName.data = index == 0 ? "secret" : "alice";
    cred->userName.len = strlen(cred->userName.data);
    return PICOKEYS_OK;
}

void credential_free(Credential *cred) { memset(cred, 0, sizeof(*cred)); }
bool resident_container_is_marker(const file_t *file) { (void)file; return true; }

int credential_resident_delete(const file_t *file) {
    removed = file == &slots[0] ? 0 : 1;
    present[removed] = false;
    return PICOKEYS_OK;
}

int credential_rp_legacy_decrement(const uint8_t hash[RP_ID_HASH_LEN]) {
    (void)hash;
    return PICOKEYS_OK;
}

int dev_state_update(dev_state_t state) { (void)state; return PICOKEYS_OK; }
void flash_commit(void) { ++commits; }

int main(void) {
    cf_credential_service service;
    cf_credential_service_init(&service, cardputer_credentials_backend());
    assert(cf_credential_request_list(&service) == CF_CRED_OK);
    assert(cf_credential_service_poll(&service) == CF_CRED_RESULT_LIST);
    size_t count;
    int truncated;
    const cf_credential_item *items = cf_credential_items(&service, &count, &truncated);
    assert(count == 2 && !truncated);
    assert(items[0].protected_key && items[0].rp_id[0] == '\0' &&
           items[0].account[0] == '\0');
    assert(!items[1].protected_key && strcmp(items[1].account, "alice") == 0);
    const cf_credential_item protected_key = items[0];
    const cf_credential_item normal_key = items[1];

    assert(cf_credential_request_delete(&service, normal_key.id, normal_key.id_len) == CF_CRED_OK);
    assert(cf_credential_service_poll(&service) == CF_CRED_RESULT_DELETED);
    assert(removed == 1 && present[0] && !present[1] && commits == 1);
    items = cf_credential_items(&service, &count, &truncated);
    assert(count == 1 && memcmp(items[0].id, protected_key.id, protected_key.id_len) == 0);

    removed = -1;
    assert(cf_credential_request_delete(&service, protected_key.id, protected_key.id_len) == CF_CRED_OK);
    assert(cf_credential_service_poll(&service) == CF_CRED_RESULT_FORBIDDEN);
    assert(removed == -1 && present[0] && commits == 1);

    assert(cf_credential_request_delete(&service, normal_key.id, normal_key.id_len) == CF_CRED_OK);
    assert(cf_credential_service_poll(&service) == CF_CRED_RESULT_NOT_FOUND);
    assert(removed == -1 && commits == 1);
    puts("PASS: local deletion selects identity before checking UV policy");
    return 0;
}
