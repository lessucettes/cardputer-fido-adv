#ifndef CF_FAKE_FLASH_H
#define CF_FAKE_FLASH_H

#include <stddef.h>
#include <stdint.h>

#include "key_store.h"

#define CF_FAKE_FLASH_SIZE 0x102000u
#define CF_FAKE_DB_OFFSET 0x2000u

typedef struct {
    uint8_t *data;
    size_t size;
    int read_calls;
    int program_calls;
    int erase_calls;
    int fail_read_at;
    int fail_program_at;
    int fail_erase_at;
    int fail_random;
    int random_zero;
    int random_zero_key2;
    int random_calls;
    int database_erased;
    uint32_t random_state;
} fake_flash_t;

int fake_flash_init(fake_flash_t *flash);
void fake_flash_free(fake_flash_t *flash);
void fake_flash_reset_ops(fake_flash_t *flash);
void fake_flash_fill_database(fake_flash_t *flash, uint8_t value);
cf_key_flash fake_flash_ops(fake_flash_t *flash);

/* Test helpers. */
int fake_flash_program_direct(fake_flash_t *flash, uint32_t offset,
                              const uint8_t *data, size_t len);
int fake_flash_is_erased(const fake_flash_t *flash, uint32_t offset, size_t len);

#endif
