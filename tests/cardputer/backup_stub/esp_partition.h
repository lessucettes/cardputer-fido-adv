#pragma once
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Opaque-in-production descriptor; the probe owns the fake storage behind it. */
typedef struct {
    uint32_t address;
    uint32_t size;
} esp_partition_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_partition_read(const esp_partition_t *partition, size_t src_offset,
                             void *dst, size_t size);
esp_err_t esp_partition_write(const esp_partition_t *partition, size_t dst_offset,
                              const void *src, size_t size);
esp_err_t esp_partition_erase_range(const esp_partition_t *partition,
                                    size_t offset, size_t size);

#ifdef __cplusplus
}
#endif
