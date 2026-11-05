#include "credential_service.h"

#include <string.h>

void cf_credential_service_init(cf_credential_service *service,
                                cf_credential_backend backend) {
    if (service == NULL) {
        return;
    }
    memset(service, 0, sizeof(*service));
    service->backend = backend;
    service->result = CF_CRED_RESULT_NONE;
}

void cf_credential_service_set_busy(cf_credential_service *service, int busy) {
    if (service != NULL) {
        service->busy = busy != 0;
    }
}

static int queue_request(cf_credential_service *service, cf_cred_op op) {
    if (service == NULL) {
        return CF_CRED_INVALID;
    }
    if (service->busy || service->have_request) {
        return CF_CRED_BUSY;
    }
    service->request = op;
    service->have_request = 1;
    return CF_CRED_OK;
}

cf_cred_status cf_credential_request_list(cf_credential_service *service) {
    return queue_request(service, CF_CRED_OP_LIST);
}

cf_cred_status cf_credential_request_delete(cf_credential_service *service,
                                            const uint8_t *id, size_t id_len) {
    if (service == NULL || id == NULL || id_len == 0 || id_len > CF_CRED_ID_MAX) {
        return CF_CRED_INVALID;
    }
    const cf_cred_status status = queue_request(service, CF_CRED_OP_DELETE);
    if (status != CF_CRED_OK) {
        return status;
    }
    memcpy(service->pending_id, id, id_len);
    service->pending_id_len = id_len;
    return CF_CRED_OK;
}

static cf_cred_result run_list(cf_credential_service *service) {
    size_t total = 0;
    int truncated = 0;
    if (service->backend.list == NULL) {
        return CF_CRED_RESULT_ERROR;
    }
    const int rc = service->backend.list(service->backend.ctx, service->items,
                                         CF_CRED_MAX_ITEMS, &total, &truncated);
    if (rc != 0) {
        return CF_CRED_RESULT_ERROR;
    }
    service->count = total > CF_CRED_MAX_ITEMS ? CF_CRED_MAX_ITEMS : total;
    service->truncated = truncated || total > CF_CRED_MAX_ITEMS;
    service->items_dirty = 1;
    return CF_CRED_RESULT_LIST;
}

static cf_cred_result run_delete(cf_credential_service *service) {
    if (service->backend.remove == NULL) {
        return CF_CRED_RESULT_ERROR;
    }
    const int rc = service->backend.remove(service->backend.ctx,
                                           service->pending_id,
                                           service->pending_id_len);
    if (rc == 0) {
        const size_t removed_len = service->pending_id_len;
        for (size_t i = 0; i < service->count; ++i) {
            if (service->items[i].id_len == removed_len &&
                memcmp(service->items[i].id, service->pending_id,
                       removed_len) == 0) {
                for (size_t j = i + 1; j < service->count; ++j) {
                    service->items[j - 1] = service->items[j];
                }
                --service->count;
                break;
            }
        }
        service->items_dirty = 1;
    }
    memset(service->pending_id, 0, sizeof(service->pending_id));
    service->pending_id_len = 0;
    if (rc == 0) {
        return CF_CRED_RESULT_DELETED;
    }
    if (rc == 1) {
        return CF_CRED_RESULT_NOT_FOUND;
    }
    if (rc == -2) {
        return CF_CRED_RESULT_FORBIDDEN;
    }
    return CF_CRED_RESULT_ERROR;
}

cf_cred_result cf_credential_service_poll(cf_credential_service *service) {
    if (service == NULL || service->busy || !service->have_request) {
        return CF_CRED_RESULT_NONE;
    }
    const cf_cred_op op = service->request;
    service->request = CF_CRED_OP_NONE;
    service->have_request = 0;
    switch (op) {
        case CF_CRED_OP_LIST:
            service->result = run_list(service);
            break;
        case CF_CRED_OP_DELETE:
            service->result = run_delete(service);
            break;
        default:
            service->result = CF_CRED_RESULT_NONE;
            break;
    }
    return service->result;
}

cf_cred_result cf_credential_take_result(cf_credential_service *service) {
    if (service == NULL) {
        return CF_CRED_RESULT_NONE;
    }
    const cf_cred_result result = service->result;
    service->result = CF_CRED_RESULT_NONE;
    return result;
}

int cf_credential_request_pending(const cf_credential_service *service) {
    return service != NULL && service->have_request;
}

const cf_credential_item *cf_credential_items(
    const cf_credential_service *service, size_t *count, int *truncated) {
    if (service == NULL) {
        if (count != NULL) {
            *count = 0;
        }
        if (truncated != NULL) {
            *truncated = 0;
        }
        return NULL;
    }
    if (count != NULL) {
        *count = service->count;
    }
    if (truncated != NULL) {
        *truncated = service->truncated;
    }
    return service->items;
}
