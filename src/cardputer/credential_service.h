#ifndef CF_CREDENTIAL_SERVICE_H
#define CF_CREDENTIAL_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#define CF_CRED_ID_MAX 32u
#define CF_CRED_RP_MAX 256u
#define CF_CRED_ACCOUNT_MAX 65u
#define CF_CRED_MAX_ITEMS 32u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t id[CF_CRED_ID_MAX];
    uint8_t id_len;
    char rp_id[CF_CRED_RP_MAX];
    char account[CF_CRED_ACCOUNT_MAX];
    uint8_t protected_key;
} cf_credential_item;

typedef struct {
    void *ctx;
    /*
     * Fills at most capacity items and reports the total number available.
     * Returns 0 on success, negative on failure.
     */
    int (*list)(void *ctx, cf_credential_item *items, size_t capacity,
                size_t *total_out, int *truncated);
    /*
     * Removes exactly the credential with the supplied opaque identity.
     * Returns 0 on success, 1 when the identity no longer exists, negative on
     * storage failure. The identity must be re-resolved at call time.
     */
    int (*remove)(void *ctx, const uint8_t *id, size_t id_len);
} cf_credential_backend;

typedef enum {
    CF_CRED_OK = 0,
    CF_CRED_BUSY = 1,
    CF_CRED_INVALID = 2
} cf_cred_status;

typedef enum {
    CF_CRED_RESULT_NONE = 0,
    CF_CRED_RESULT_LIST = 1,
    CF_CRED_RESULT_DELETED = 2,
    CF_CRED_RESULT_NOT_FOUND = 3,
    CF_CRED_RESULT_ERROR = 4
} cf_cred_result;

typedef enum {
    CF_CRED_OP_NONE = 0,
    CF_CRED_OP_LIST = 1,
    CF_CRED_OP_DELETE = 2
} cf_cred_op;

typedef struct {
    cf_credential_backend backend;
    int busy;
    int have_request;
    cf_cred_op request;
    uint8_t pending_id[CF_CRED_ID_MAX];
    size_t pending_id_len;
    cf_credential_item items[CF_CRED_MAX_ITEMS];
    size_t count;
    int truncated;
    cf_cred_result result;
    int items_dirty;
} cf_credential_service;

void cf_credential_service_init(cf_credential_service *service,
                                cf_credential_backend backend);

/* Called by the owner loop around a CTAP authentication operation. */
void cf_credential_service_set_busy(cf_credential_service *service, int busy);

cf_cred_status cf_credential_request_list(cf_credential_service *service);
cf_cred_status cf_credential_request_delete(cf_credential_service *service,
                                            const uint8_t *id, size_t id_len);

/*
 * Executes at most one queued request. Must be called from the authenticator
 * owner context, never from the display/input task directly.
 */
cf_cred_result cf_credential_service_poll(cf_credential_service *service);

cf_cred_result cf_credential_take_result(cf_credential_service *service);
int cf_credential_request_pending(const cf_credential_service *service);
const cf_credential_item *cf_credential_items(
    const cf_credential_service *service, size_t *count, int *truncated);

#ifdef __cplusplus
}
#endif

#endif
