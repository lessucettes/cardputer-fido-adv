#include "credential_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    cf_credential_item items[40];
    size_t count;
    size_t total_override;
    int truncated;
    int remove_calls;
    int fail_remove;
    int forbidden;
    int missing;
} fake_backend;

static void require(int condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static int fake_list(void* ctx, cf_credential_item* items, size_t capacity,
                     size_t* total_out, int* truncated) {
    fake_backend* backend = ctx;
    const size_t total = backend->total_override != 0
                             ? backend->total_override
                             : backend->count;
    const size_t copy = total < capacity ? total : capacity;
    for (size_t i = 0; i < copy && i < backend->count; ++i) {
        items[i] = backend->items[i];
    }
    if (total_out != NULL) {
        *total_out = total;
    }
    if (truncated != NULL) {
        *truncated = backend->truncated;
    }
    return 0;
}

static int fake_remove(void* ctx, const uint8_t* id, size_t id_len) {
    fake_backend* backend = ctx;
    backend->remove_calls++;
    if (backend->forbidden) {
        return -2;
    }
    if (backend->fail_remove) {
        return -1;
    }
    if (backend->missing) {
        return 1;
    }
    for (size_t i = 0; i < backend->count; ++i) {
        if (backend->items[i].id_len == id_len &&
            memcmp(backend->items[i].id, id, id_len) == 0) {
            for (size_t j = i + 1; j < backend->count; ++j) {
                backend->items[j - 1] = backend->items[j];
            }
            backend->count--;
            return 0;
        }
    }
    return 1;
}

static void add_item(fake_backend* backend, uint8_t id, const char* rp,
                     const char* account, int protected_key) {
    cf_credential_item* item = &backend->items[backend->count++];
    memset(item, 0, sizeof(*item));
    item->id[0] = id;
    item->id_len = 1;
    snprintf(item->rp_id, sizeof(item->rp_id), "%s", rp);
    snprintf(item->account, sizeof(item->account), "%s", account);
    item->protected_key = protected_key != 0;
}

int main(void) {
    fake_backend backend;
    memset(&backend, 0, sizeof(backend));
    add_item(&backend, 1, "example.com", "alice", 0);
    add_item(&backend, 2, "example.org", "bob", 0);

    cf_credential_backend ops;
    memset(&ops, 0, sizeof(ops));
    ops.ctx = &backend;
    ops.list = fake_list;
    ops.remove = fake_remove;

    cf_credential_service service;
    cf_credential_service_init(&service, ops);

    require(cf_credential_request_list(&service) == CF_CRED_OK, "queue list");
    require(cf_credential_request_list(&service) == CF_CRED_BUSY,
            "one pending request at a time");
    require(cf_credential_service_poll(&service) == CF_CRED_RESULT_LIST,
            "list executes");
    require(cf_credential_take_result(&service) == CF_CRED_RESULT_LIST,
            "list result is reported once");
    require(cf_credential_take_result(&service) == CF_CRED_RESULT_NONE,
            "result is consumed");
    size_t count = 0;
    int truncated = 0;
    const cf_credential_item* items = cf_credential_items(&service, &count, &truncated);
    require(items != NULL && count == 2, "two items listed");
    require(strcmp(items[0].rp_id, "example.com") == 0, "first RP");
    require(strcmp(items[1].account, "bob") == 0, "second account");
    require(!truncated, "not truncated");

    require(cf_credential_request_list(&service) == CF_CRED_OK, "queue user op");
    cf_credential_service_set_busy(&service, 1);
    require(cf_credential_service_poll(&service) == CF_CRED_RESULT_NONE,
            "USB authentication keeps the queue stopped");
    require(cf_credential_request_pending(&service), "request remains queued");
    require(cf_credential_request_delete(&service, (const uint8_t*)"\x01", 1) ==
                CF_CRED_BUSY,
            "new local request rejected while busy");
    cf_credential_service_set_busy(&service, 0);
    require(cf_credential_service_poll(&service) == CF_CRED_RESULT_LIST,
            "queued list runs after authentication");

    const uint8_t id_one = 1;
    require(cf_credential_request_delete(&service, &id_one, 1) == CF_CRED_OK,
            "queue delete");
    require(cf_credential_service_poll(&service) == CF_CRED_RESULT_DELETED,
            "delete succeeds");
    require(backend.count == 1 && backend.items[0].id[0] == 2,
            "backend removed the selected credential");
    require(backend.remove_calls == 1, "one remove call");

    require(cf_credential_request_delete(&service, &id_one, 1) == CF_CRED_OK,
            "queue stale delete");
    backend.missing = 1;
    require(cf_credential_service_poll(&service) == CF_CRED_RESULT_NOT_FOUND,
            "stale identity is not found");
    backend.missing = 0;

    require(cf_credential_request_delete(&service, &id_one, 1) == CF_CRED_OK,
            "queue failing delete");
    backend.fail_remove = 1;
    require(cf_credential_service_poll(&service) == CF_CRED_RESULT_ERROR,
            "storage failure is reported");
    backend.fail_remove = 0;

    backend.forbidden = 1;
    require(cf_credential_request_delete(&service, &id_one, 1) == CF_CRED_OK,
            "queue protected delete");
    require(cf_credential_service_poll(&service) == CF_CRED_RESULT_FORBIDDEN,
            "protected credential deletion is refused");
    backend.forbidden = 0;

    require(cf_credential_request_delete(&service, NULL, 0) == CF_CRED_INVALID,
            "empty identity rejected");

    backend.total_override = 40;
    backend.truncated = 1;
    require(cf_credential_request_list(&service) == CF_CRED_OK, "queue full list");
    require(cf_credential_service_poll(&service) == CF_CRED_RESULT_LIST,
            "full list executes");
    items = cf_credential_items(&service, &count, &truncated);
    require(count == CF_CRED_MAX_ITEMS, "list is bounded");
    require(truncated, "truncation is visible to the UI");

    puts("PASS: serialized resident-key management");
    return 0;
}
