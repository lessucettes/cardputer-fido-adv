#include "fake_flash.h"

#include <stdlib.h>
#include <string.h>

static int check_bounds(const fake_flash_t *flash, uint32_t offset, size_t len) {
    if (flash == NULL || flash->data == NULL) {
        return -1;
    }
    if (offset > flash->size || len > flash->size - offset) {
        return -1;
    }
    return 0;
}

static int fake_read(void *ctx, uint32_t offset, uint8_t *out, size_t len) {
    fake_flash_t *flash = ctx;
    flash->read_calls++;
    if (flash->fail_read_at > 0 && flash->read_calls == flash->fail_read_at) {
        return -1;
    }
    if (out == NULL || check_bounds(flash, offset, len) != 0) {
        return -1;
    }
    memcpy(out, flash->data + offset, len);
    return 0;
}

static int fake_program(void *ctx, uint32_t offset, const uint8_t *data,
                        size_t len) {
    fake_flash_t *flash = ctx;
    flash->program_calls++;
    if (flash->fail_program_at > 0 &&
        flash->program_calls == flash->fail_program_at) {
        return -1;
    }
    if (data == NULL || check_bounds(flash, offset, len) != 0) {
        return -1;
    }
    if ((offset % 4u) != 0 || (len % 4u) != 0) {
        return -1;
    }
    for (size_t i = 0; i < len; ++i) {
        if ((flash->data[offset + i] & data[i]) != data[i]) {
            return -1; /* only one-to-zero programming is allowed */
        }
    }
    for (size_t i = 0; i < len; ++i) {
        flash->data[offset + i] &= data[i];
    }
    return 0;
}

static int fake_erase(void *ctx, uint32_t offset, size_t len) {
    fake_flash_t *flash = ctx;
    flash->erase_calls++;
    if (flash->fail_erase_at > 0 && flash->erase_calls == flash->fail_erase_at) {
        return -1;
    }
    if (check_bounds(flash, offset, len) != 0) {
        return -1;
    }
    if ((offset % CF_KEY_PAGE_SIZE) != 0 || (len % CF_KEY_PAGE_SIZE) != 0) {
        return -1;
    }
    memset(flash->data + offset, 0xFF, len);
    return 0;
}

static int fake_database_erased(void *ctx) {
    fake_flash_t *flash = ctx;
    return flash->database_erased;
}

static uint8_t next_random(fake_flash_t *flash) {
    flash->random_state = flash->random_state * 1664525u + 1013904223u;
    return (uint8_t)(flash->random_state >> 24);
}

static int fake_random(void *ctx, uint8_t *out, size_t len) {
    fake_flash_t *flash = ctx;
    if (flash->fail_random) {
        return -1;
    }
    if (out == NULL) {
        return -1;
    }
    flash->random_calls++;
    const int zero = flash->random_zero ||
                     (flash->random_zero_key2 && flash->random_calls > 1);
    for (size_t i = 0; i < len; ++i) {
        out[i] = zero ? 0 : next_random(flash);
    }
    return 0;
}

int fake_flash_init(fake_flash_t *flash) {
    if (flash == NULL) {
        return -1;
    }
    memset(flash, 0, sizeof(*flash));
    flash->size = CF_FAKE_FLASH_SIZE;
    flash->data = malloc(flash->size);
    if (flash->data == NULL) {
        return -1;
    }
    memset(flash->data, 0xFF, flash->size);
    flash->fail_read_at = -1;
    flash->fail_program_at = -1;
    flash->fail_erase_at = -1;
    flash->database_erased = 1;
    flash->random_state = 0x12345678u;
    return 0;
}

void fake_flash_free(fake_flash_t *flash) {
    if (flash == NULL) {
        return;
    }
    free(flash->data);
    flash->data = NULL;
    flash->size = 0;
}

void fake_flash_reset_ops(fake_flash_t *flash) {
    if (flash == NULL) {
        return;
    }
    flash->read_calls = 0;
    flash->program_calls = 0;
    flash->erase_calls = 0;
}

void fake_flash_fill_database(fake_flash_t *flash, uint8_t value) {
    if (flash == NULL || flash->data == NULL) {
        return;
    }
    memset(flash->data + CF_FAKE_DB_OFFSET, value,
           flash->size - CF_FAKE_DB_OFFSET);
    flash->database_erased = value == 0xFF ? 1 : 0;
}

cf_key_flash fake_flash_ops(fake_flash_t *flash) {
    cf_key_flash ops;
    ops.ctx = flash;
    ops.read = fake_read;
    ops.program = fake_program;
    ops.erase = fake_erase;
    ops.database_erased = fake_database_erased;
    ops.random = fake_random;
    return ops;
}

int fake_flash_program_direct(fake_flash_t *flash, uint32_t offset,
                              const uint8_t *data, size_t len) {
    return fake_program(flash, offset, data, len);
}

int fake_flash_is_erased(const fake_flash_t *flash, uint32_t offset, size_t len) {
    if (check_bounds(flash, offset, len) != 0) {
        return 0;
    }
    for (size_t i = 0; i < len; ++i) {
        if (flash->data[offset + i] != 0xFF) {
            return 0;
        }
    }
    return 1;
}
