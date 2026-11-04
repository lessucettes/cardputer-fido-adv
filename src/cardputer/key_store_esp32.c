#include "key_store_esp32.h"

#include <string.h>

#include "bootloader_random.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "flash_layout.h"
#include "key_store.h"
#include "otp_platform.h"
#include "picokeys.h"

static const esp_partition_t *fido_partition = NULL;
static cf_flash_layout fido_layout;
static cf_device_keys device_keys;
static int store_status = -1;
static int initialized = 0;

const esp_partition_t *cardputer_fido_partition(void) {
    return fido_partition;
}

int cardputer_key_store_status(void) {
    return store_status;
}

static int store_read(void *ctx, uint32_t offset, uint8_t *out, size_t len) {
    (void)ctx;
    if (fido_partition == NULL) {
        return -1;
    }
    return esp_partition_read(fido_partition, offset, out, len) == ESP_OK ? 0 : -1;
}

static int store_program(void *ctx, uint32_t offset, const uint8_t *data,
                         size_t len) {
    (void)ctx;
    if (fido_partition == NULL) {
        return -1;
    }
    return esp_partition_write(fido_partition, offset, data, len) == ESP_OK ? 0 : -1;
}

static int store_erase(void *ctx, uint32_t offset, size_t len) {
    (void)ctx;
    if (fido_partition == NULL) {
        return -1;
    }
    return esp_partition_erase_range(fido_partition, offset, len) == ESP_OK ? 0 : -1;
}

static int database_erased(void *ctx) {
    (void)ctx;
    uint8_t buffer[512];
    for (uint32_t offset = CF_FIDO_FS_OFFSET; offset < CF_FIDO_STORE_SIZE;
         offset += sizeof(buffer)) {
        const size_t length =
            (CF_FIDO_STORE_SIZE - offset) < sizeof(buffer)
                ? (CF_FIDO_STORE_SIZE - offset)
                : sizeof(buffer);
        if (store_read(NULL, offset, buffer, length) != 0) {
            return -1;
        }
        for (size_t i = 0; i < length; ++i) {
            if (buffer[i] != 0xFF) {
                return 0;
            }
        }
    }
    return 1;
}

static int random_bytes(void *ctx, uint8_t *out, size_t len) {
    (void)ctx;
    esp_fill_random(out, len);
    return 0;
}

static int validate_partition_table(void) {
    cf_partition_desc descriptors[CF_MAX_PARTITIONS];
    size_t count = 0;
    esp_partition_iterator_t iterator =
        esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (iterator != NULL && count < CF_MAX_PARTITIONS) {
        const esp_partition_t *partition = esp_partition_get(iterator);
        if (partition != NULL) {
            memset(&descriptors[count], 0, sizeof(descriptors[count]));
            descriptors[count].type = (uint8_t)partition->type;
            descriptors[count].subtype = (uint8_t)partition->subtype;
            descriptors[count].offset = partition->address;
            descriptors[count].size = partition->size;
            strncpy(descriptors[count].label, partition->label,
                    sizeof(descriptors[count].label) - 1);
            ++count;
        }
        iterator = esp_partition_next(iterator);
    }
    const bool too_many = iterator != NULL;
    esp_partition_iterator_release(iterator);
    if (too_many) {
        return -(int)CF_LAYOUT_TOO_MANY;
    }
    uint32_t flash_size = 0;
    if (esp_flash_get_size(esp_flash_default_chip, &flash_size) != ESP_OK) {
        return -(int)CF_LAYOUT_INVALID_ARGUMENT;
    }
    const cf_layout_status status =
        cf_flash_layout_validate(descriptors, count, flash_size, &fido_layout);
    if (status != CF_LAYOUT_OK) {
        return -(int)status;
    }
    return 0;
}

int cardputer_key_store_init(void) {
    if (initialized) {
        return store_status;
    }
    initialized = 1;
    fido_partition = esp_partition_find_first(CF_FIDO_PART_TYPE,
                                              CF_FIDO_PART_SUBTYPE,
                                              CF_FIDO_PART_LABEL);
    if (fido_partition == NULL) {
        store_status = -(int)CF_LAYOUT_MISSING;
        return store_status;
    }
    const int validation = validate_partition_table();
    if (validation != 0) {
        store_status = validation;
        fido_partition = NULL;
        return store_status;
    }

    cf_key_flash flash;
    flash.ctx = NULL;
    flash.read = store_read;
    flash.program = store_program;
    flash.erase = store_erase;
    flash.database_erased = database_erased;
    flash.random = random_bytes;

    cf_key_status status = cf_key_store_load(&flash, &device_keys);
    if (status == CF_KEY_EMPTY) {
        bootloader_random_enable();
        status = cf_key_store_provision(&flash, &device_keys);
        bootloader_random_disable();
    }
    if (status != CF_KEY_READY) {
        memset(&device_keys, 0, sizeof(device_keys));
        store_status = status == CF_KEY_CORRUPT ? -3 : -2;
        return store_status;
    }
    store_status = 0;
    return store_status;
}

void otp_platform_init(const uint8_t **otp_key_1_out,
                       const uint8_t **otp_key_2_out) {
    if (otp_key_1_out != NULL) {
        *otp_key_1_out = NULL;
    }
    if (otp_key_2_out != NULL) {
        *otp_key_2_out = NULL;
    }
    if (cardputer_key_store_init() != 0) {
        return;
    }
    if (otp_key_1_out != NULL) {
        *otp_key_1_out = device_keys.key1;
    }
    if (otp_key_2_out != NULL) {
        *otp_key_2_out = device_keys.key2;
    }
}

int otp_platform_enable_secure_boot(uint8_t bootkey, bool secure_lock) {
    (void)bootkey;
    (void)secure_lock;
    /* Provisioning security features is deliberately unreachable on this
     * port. Return an error instead of delegating to the eFuse backend. */
    return -1;
}

bool otp_platform_is_secure_boot_enabled(uint8_t *bootkey) {
    (void)bootkey;
    return false;
}

bool otp_platform_is_secure_boot_locked(void) {
    return false;
}
