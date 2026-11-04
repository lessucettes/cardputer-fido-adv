#ifndef CF_KEY_STORE_ESP32_H
#define CF_KEY_STORE_ESP32_H

#include <stdint.h>

#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the validated fido_store partition, or NULL before init/failure. */
const esp_partition_t *cardputer_fido_partition(void);
/* 0 when keys are loaded or provisioned, negative on any failure. */
int cardputer_key_store_init(void);
int cardputer_key_store_status(void);

#ifdef __cplusplus
}
#endif

#endif
