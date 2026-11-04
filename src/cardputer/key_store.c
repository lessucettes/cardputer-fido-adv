#include "key_store.h"

#include <string.h>

static const uint8_t kMagic[8] = {'C', 'F', 'K', 'E', 'Y', '0', '0', '1'};
static const uint8_t kSecp256k1Order[32] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE,
    0xBA, 0xAE, 0xDC, 0xE6, 0xAF, 0x48, 0xA0, 0x3B,
    0xBF, 0xD2, 0x5E, 0x8C, 0xD0, 0x36, 0x41, 0x41
};

static void put_u32_le(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value & 0xFF);
    out[1] = (uint8_t)((value >> 8) & 0xFF);
    out[2] = (uint8_t)((value >> 16) & 0xFF);
    out[3] = (uint8_t)((value >> 24) & 0xFF);
}

static uint32_t get_u32_le(const uint8_t *in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

static int all_bytes(const uint8_t *data, size_t len, uint8_t value) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff |= (uint8_t)(data[i] ^ value);
    }
    return diff == 0;
}

static int scalar_valid(const uint8_t scalar[32]) {
    int nonzero = 0;
    for (size_t i = 0; i < 32; ++i) {
        nonzero |= scalar[i];
    }
    if (nonzero == 0) {
        return 0;
    }
    for (size_t i = 0; i < 32; ++i) {
        if (scalar[i] < kSecp256k1Order[i]) {
            return 1;
        }
        if (scalar[i] > kSecp256k1Order[i]) {
            return 0;
        }
    }
    return 0; /* exactly the group order is not a private scalar */
}

uint32_t cf_key_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

void cf_key_record_encode(const cf_device_keys *keys,
                          uint8_t record[CF_KEY_PAGE_SIZE]) {
    memset(record, 0xFF, CF_KEY_PAGE_SIZE);
    if (keys == NULL) {
        return;
    }
    memcpy(record, kMagic, sizeof(kMagic));
    put_u32_le(record + 8, CF_KEY_VERSION);
    put_u32_le(record + 12, CF_KEY_PAYLOAD_SIZE);
    memcpy(record + 16, keys->key1, 32);
    memcpy(record + 48, keys->key2, 32);
    const uint32_t crc = cf_key_crc32(record, 80);
    put_u32_le(record + 80, crc);
    put_u32_le(record + 84, CF_KEY_COMMIT_MARKER);
}

cf_key_status cf_key_record_decode(const uint8_t record[CF_KEY_PAGE_SIZE],
                                   cf_device_keys *out) {
    cf_device_keys keys;
    memset(&keys, 0, sizeof(keys));

    if (record == NULL || out == NULL) {
        return CF_KEY_INVALID_ARGUMENT;
    }
    if (memcmp(record, kMagic, sizeof(kMagic)) != 0 ||
        get_u32_le(record + 8) != CF_KEY_VERSION ||
        get_u32_le(record + 12) != CF_KEY_PAYLOAD_SIZE ||
        get_u32_le(record + 84) != CF_KEY_COMMIT_MARKER) {
        return CF_KEY_CORRUPT;
    }
    if (get_u32_le(record + 80) != cf_key_crc32(record, 80)) {
        return CF_KEY_CORRUPT;
    }
    if (all_bytes(record + 16, 32, 0x00) || all_bytes(record + 16, 32, 0xFF)) {
        return CF_KEY_CORRUPT;
    }
    if (!scalar_valid(record + 48)) {
        return CF_KEY_CORRUPT;
    }
    memcpy(keys.key1, record + 16, 32);
    memcpy(keys.key2, record + 48, 32);
    *out = keys;
    return CF_KEY_READY;
}

static cf_key_status read_page(const cf_key_flash *flash, uint32_t offset,
                               uint8_t out[CF_KEY_PAGE_SIZE]) {
    if (flash->read(flash->ctx, offset, out, CF_KEY_PAGE_SIZE) != 0) {
        return CF_KEY_IO_ERROR;
    }
    return CF_KEY_READY;
}

cf_key_status cf_key_store_load(const cf_key_flash *flash, cf_device_keys *out) {
    uint8_t page_a[CF_KEY_PAGE_SIZE];
    uint8_t page_b[CF_KEY_PAGE_SIZE];
    cf_device_keys keys_a;
    cf_device_keys keys_b;
    cf_key_status status_a;
    cf_key_status status_b;
    int erased_a;
    int erased_b;
    int database_erased;

    if (flash == NULL || out == NULL || flash->read == NULL ||
        flash->database_erased == NULL || flash->program == NULL ||
        flash->erase == NULL) {
        return CF_KEY_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    memset(&keys_a, 0, sizeof(keys_a));
    memset(&keys_b, 0, sizeof(keys_b));

    if (read_page(flash, 0, page_a) != CF_KEY_READY) {
        return CF_KEY_IO_ERROR;
    }
    if (read_page(flash, CF_KEY_PAGE_SIZE, page_b) != CF_KEY_READY) {
        return CF_KEY_IO_ERROR;
    }
    status_a = cf_key_record_decode(page_a, &keys_a);
    status_b = cf_key_record_decode(page_b, &keys_b);

    if (status_a == CF_KEY_READY && status_b == CF_KEY_READY) {
        if (memcmp(keys_a.key1, keys_b.key1, 32) != 0 ||
            memcmp(keys_a.key2, keys_b.key2, 32) != 0) {
            memset(&keys_a, 0, sizeof(keys_a));
            memset(&keys_b, 0, sizeof(keys_b));
            return CF_KEY_CORRUPT;
        }
        *out = keys_a;
        memset(&keys_b, 0, sizeof(keys_b));
        return CF_KEY_READY;
    }
    if (status_a == CF_KEY_READY) {
        *out = keys_a;
        memset(&keys_b, 0, sizeof(keys_b));
        return CF_KEY_READY;
    }
    if (status_b == CF_KEY_READY) {
        *out = keys_b;
        memset(&keys_a, 0, sizeof(keys_a));
        return CF_KEY_READY;
    }

    erased_a = all_bytes(page_a, CF_KEY_PAGE_SIZE, 0xFF);
    erased_b = all_bytes(page_b, CF_KEY_PAGE_SIZE, 0xFF);
    database_erased = flash->database_erased(flash->ctx);
    memset(&keys_a, 0, sizeof(keys_a));
    memset(&keys_b, 0, sizeof(keys_b));
    if (database_erased < 0) {
        return CF_KEY_IO_ERROR;
    }
    if (erased_a && erased_b && database_erased == 1) {
        return CF_KEY_EMPTY;
    }
    return CF_KEY_CORRUPT;
}

static int fill_random(const cf_key_flash *flash, uint8_t *out, size_t len) {
    if (flash->random == NULL) {
        return -1;
    }
    return flash->random(flash->ctx, out, len);
}

static int generate_keys(const cf_key_flash *flash, cf_device_keys *keys) {
    memset(keys, 0, sizeof(*keys));
    for (int attempt = 0; attempt < 64; ++attempt) {
        if (fill_random(flash, keys->key1, sizeof(keys->key1)) != 0) {
            return -1;
        }
        if (!all_bytes(keys->key1, sizeof(keys->key1), 0x00) &&
            !all_bytes(keys->key1, sizeof(keys->key1), 0xFF)) {
            break;
        }
        if (attempt == 63) {
            return -1;
        }
    }
    for (int attempt = 0; attempt < 64; ++attempt) {
        if (fill_random(flash, keys->key2, sizeof(keys->key2)) != 0) {
            return -1;
        }
        if (scalar_valid(keys->key2)) {
            return 0;
        }
    }
    return -1;
}

static cf_key_status write_page(const cf_key_flash *flash, uint32_t offset,
                                const cf_device_keys *keys,
                                uint8_t record[CF_KEY_PAGE_SIZE]) {
    cf_key_record_encode(keys, record);
    if (flash->erase(flash->ctx, offset, CF_KEY_PAGE_SIZE) != 0) {
        return CF_KEY_IO_ERROR;
    }
    /* Commit marker is written only after the payload and CRC are durable. */
    if (flash->program(flash->ctx, offset, record, 84) != 0) {
        return CF_KEY_IO_ERROR;
    }
    if (flash->program(flash->ctx, offset + 84, record + 84, 4) != 0) {
        return CF_KEY_IO_ERROR;
    }
    if (flash->read(flash->ctx, offset, record, CF_KEY_PAGE_SIZE) != 0) {
        return CF_KEY_IO_ERROR;
    }
    cf_device_keys verify;
    memset(&verify, 0, sizeof(verify));
    const cf_key_status status = cf_key_record_decode(record, &verify);
    const int equal = status == CF_KEY_READY &&
                      memcmp(verify.key1, keys->key1, 32) == 0 &&
                      memcmp(verify.key2, keys->key2, 32) == 0;
    memset(&verify, 0, sizeof(verify));
    return equal ? CF_KEY_READY : CF_KEY_CORRUPT;
}

cf_key_status cf_key_store_provision(const cf_key_flash *flash,
                                     cf_device_keys *out) {
    uint8_t record[CF_KEY_PAGE_SIZE];
    cf_device_keys existing;
    cf_device_keys generated;
    cf_key_status status;

    if (flash == NULL || out == NULL || flash->read == NULL ||
        flash->program == NULL || flash->erase == NULL ||
        flash->database_erased == NULL) {
        return CF_KEY_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    status = cf_key_store_load(flash, &existing);
    if (status == CF_KEY_READY) {
        *out = existing;
        memset(&existing, 0, sizeof(existing));
        return CF_KEY_READY;
    }
    memset(&existing, 0, sizeof(existing));
    if (status == CF_KEY_CORRUPT || status == CF_KEY_IO_ERROR ||
        status == CF_KEY_INVALID_ARGUMENT) {
        return status;
    }
    if (generate_keys(flash, &generated) != 0) {
        memset(&generated, 0, sizeof(generated));
        return CF_KEY_IO_ERROR;
    }

    status = write_page(flash, 0, &generated, record);
    if (status != CF_KEY_READY) {
        memset(&generated, 0, sizeof(generated));
        memset(record, 0, sizeof(record));
        return status;
    }
    /* A committed page A is already a durable identity. A failed mirror is
     * reported by the next load as a single valid record, not as data loss. */
    (void)write_page(flash, CF_KEY_PAGE_SIZE, &generated, record);

    *out = generated;
    memset(&generated, 0, sizeof(generated));
    memset(record, 0, sizeof(record));
    return CF_KEY_READY;
}
