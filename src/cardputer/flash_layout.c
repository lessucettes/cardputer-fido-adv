#include "flash_layout.h"

#include <string.h>

static int label_terminated(const char label[17]) {
    for (size_t i = 0; i < 17; ++i) {
        if (label[i] == '\0') {
            return 1;
        }
    }
    return 0;
}

static int ranges_overlap(uint32_t a_offset, uint32_t a_size, uint32_t b_offset,
                          uint32_t b_size) {
    /* Subtraction-based checks avoid a_offset + a_size overflow. */
    if (a_offset >= b_offset) {
        return a_offset - b_offset < b_size;
    }
    return b_offset - a_offset < a_size;
}

cf_layout_status cf_flash_layout_validate(const cf_partition_desc *partitions,
                                           size_t count, uint32_t flash_size,
                                           cf_flash_layout *out) {
    if (partitions == NULL || out == NULL || flash_size == 0) {
        return CF_LAYOUT_INVALID_ARGUMENT;
    }
    if (count == 0) {
        return CF_LAYOUT_MISSING;
    }
    if (count > CF_MAX_PARTITIONS) {
        return CF_LAYOUT_TOO_MANY;
    }

    size_t store_index = count;
    for (size_t i = 0; i < count; ++i) {
        if (!label_terminated(partitions[i].label)) {
            return CF_LAYOUT_INVALID_ARGUMENT;
        }
        if (partitions[i].size == 0) {
            return CF_LAYOUT_INVALID_ARGUMENT;
        }
        if (partitions[i].offset > flash_size ||
            partitions[i].size > flash_size - partitions[i].offset) {
            return CF_LAYOUT_OUT_OF_RANGE;
        }
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(partitions[i].label, partitions[j].label) == 0) {
                return CF_LAYOUT_DUPLICATE_LABEL;
            }
        }
        if (strcmp(partitions[i].label, CF_FIDO_PART_LABEL) == 0) {
            if (store_index != count) {
                return CF_LAYOUT_DUPLICATE_LABEL;
            }
            store_index = i;
        }
    }
    if (store_index == count) {
        return CF_LAYOUT_MISSING;
    }

    const cf_partition_desc *store = &partitions[store_index];
    if (store->type != CF_FIDO_PART_TYPE ||
        store->subtype != CF_FIDO_PART_SUBTYPE) {
        return CF_LAYOUT_WRONG_TYPE;
    }
    if (store->size != CF_FIDO_STORE_SIZE) {
        return CF_LAYOUT_WRONG_SIZE;
    }
    if ((store->offset % 0x1000u) != 0) {
        return CF_LAYOUT_NOT_ALIGNED;
    }
    for (size_t i = 0; i < count; ++i) {
        if (i == store_index) {
            continue;
        }
        if (ranges_overlap(store->offset, store->size, partitions[i].offset,
                           partitions[i].size)) {
            return CF_LAYOUT_OVERLAP;
        }
    }

    out->flash_size = flash_size;
    out->store_offset = store->offset;
    return CF_LAYOUT_OK;
}

int cf_flash_layout_physical(const cf_flash_layout *layout, uint32_t logical,
                             size_t len, uint32_t *physical) {
    if (layout == NULL || physical == NULL || layout->flash_size == 0) {
        return -1;
    }
    if (len > CF_FIDO_FS_SIZE) {
        return -1;
    }
    if (logical > CF_FIDO_FS_SIZE || len > CF_FIDO_FS_SIZE - logical) {
        return -1;
    }
    if (len > UINT32_MAX || logical > UINT32_MAX - CF_FIDO_FS_OFFSET ||
        layout->store_offset > UINT32_MAX - CF_FIDO_FS_OFFSET - logical) {
        return -1;
    }
    const uint32_t offset = layout->store_offset + CF_FIDO_FS_OFFSET + logical;
    if (offset > layout->flash_size || len > layout->flash_size - offset) {
        return -1;
    }
    *physical = offset;
    return 0;
}
