/* Behavioral tests for the portable backup apply engine (backup_store.c).
 *
 * The tests use a NOR model: erase sets 0xFF, programming can only clear bits,
 * offsets and lengths are page aligned, and reads return the current bytes.
 * Every apply is checked against the source store image and against the
 * key-store recovery policy, so an interrupted apply must never look READY.
 *
 * Build (from the repository root):
 *   cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
 *      -I src/cardputer -I <mbedtls>/include \
 *      src/cardputer/backup_store.c src/cardputer/key_store.c \
 *      tests/cardputer/backup_store_test.c \
 *      <mbedtls>/library/libmbedcrypto.a -o backup_store_test
 */
#include "backup_store.h"
#include "flash_layout.h"
#include "key_store.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STORE_SIZE CF_FIDO_STORE_SIZE
#define PAGE_SIZE CF_KEY_PAGE_SIZE
#define CHUNK_COUNT (STORE_SIZE / PAGE_SIZE)
#define LOG_MAX (CHUNK_COUNT + 8u)

#define REQUIRE(cond)                                                        \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static void check_int(const char *what, long actual, long expected) {
    if (actual != expected) {
        fprintf(stderr, "FAIL: %s: got %ld, expected %ld\n", what, actual,
                expected);
        exit(1);
    }
}

typedef struct {
    uint8_t *data;
    size_t size;

    int fail_read_at;
    int fail_program_at;
    int fail_erase_at;
    int read_calls;
    int program_calls;
    int erase_calls;

    uint32_t erase_offsets[LOG_MAX];
    size_t erase_lengths[LOG_MAX];
    uint32_t program_offsets[LOG_MAX];
    size_t program_lengths[LOG_MAX];
} nor_flash;

static int nor_init(nor_flash *flash) {
    memset(flash, 0, sizeof(*flash));
    flash->size = STORE_SIZE;
    flash->data = malloc(flash->size);
    if (!flash->data) {
        return -1;
    }
    memset(flash->data, 0xFF, flash->size);
    flash->fail_read_at = -1;
    flash->fail_program_at = -1;
    flash->fail_erase_at = -1;
    return 0;
}

static void nor_free(nor_flash *flash) {
    free(flash->data);
    flash->data = NULL;
    flash->size = 0;
}

static int nor_read(void *ctx, uint32_t offset, uint8_t *out, size_t len) {
    nor_flash *flash = ctx;
    flash->read_calls++;
    if (flash->fail_read_at > 0 && flash->read_calls == flash->fail_read_at) {
        return -1;
    }
    if (!out || (size_t)offset > flash->size || len > flash->size - offset) {
        return -1;
    }
    memcpy(out, flash->data + offset, len);
    return 0;
}

static int nor_program(void *ctx, uint32_t offset, const uint8_t *data,
                       size_t len) {
    nor_flash *flash = ctx;
    flash->program_calls++;
    if (flash->fail_program_at > 0 &&
        flash->program_calls == flash->fail_program_at) {
        return -1;
    }
    if (!data || (size_t)offset > flash->size || len > flash->size - offset) {
        return -1;
    }
    if (offset % 4u != 0 || len % 4u != 0) {
        return -1;
    }
    for (size_t i = 0; i < len; ++i) {
        if ((flash->data[offset + i] & data[i]) != data[i]) {
            return -1; /* NOR programming can only clear bits. */
        }
    }
    REQUIRE(flash->program_calls <= (int)LOG_MAX);
    flash->program_offsets[flash->program_calls - 1] = offset;
    flash->program_lengths[flash->program_calls - 1] = len;
    for (size_t i = 0; i < len; ++i) {
        flash->data[offset + i] &= data[i];
    }
    return 0;
}

static int nor_erase(void *ctx, uint32_t offset, size_t len) {
    nor_flash *flash = ctx;
    flash->erase_calls++;
    if (flash->fail_erase_at > 0 && flash->erase_calls == flash->fail_erase_at) {
        return -1;
    }
    if ((size_t)offset > flash->size || len > flash->size - offset ||
        offset % PAGE_SIZE != 0 || len % PAGE_SIZE != 0) {
        return -1;
    }
    REQUIRE(flash->erase_calls <= (int)LOG_MAX);
    flash->erase_offsets[flash->erase_calls - 1] = offset;
    flash->erase_lengths[flash->erase_calls - 1] = len;
    memset(flash->data + offset, 0xFF, len);
    return 0;
}

static int nor_database_erased(void *ctx) {
    nor_flash *flash = ctx;
    for (uint32_t offset = CF_FIDO_FS_OFFSET; offset < flash->size; ++offset) {
        if (flash->data[offset] != 0xFF) {
            return 0;
        }
    }
    return 1;
}

static cf_key_flash nor_key_ops(nor_flash *flash) {
    cf_key_flash ops;
    ops.ctx = flash;
    ops.read = nor_read;
    ops.program = nor_program;
    ops.erase = nor_erase;
    ops.database_erased = nor_database_erased;
    ops.random = NULL;
    return ops;
}

static cf_key_status load_device(nor_flash *flash, cf_device_keys *keys) {
    cf_key_flash ops = nor_key_ops(flash);
    return cf_key_store_load(&ops, keys);
}

static void init_store(cf_backup_store *store, nor_flash *flash) {
    memset(store, 0, sizeof(*store));
    store->ctx = flash;
    store->read = nor_read;
    store->program = nor_program;
    store->erase = nor_erase;
}

static void fill_bytes(uint8_t *out, size_t len, uint32_t seed) {
    uint32_t state = seed;
    for (size_t i = 0; i < len; ++i) {
        state = state * 1664525u + 1013904223u;
        out[i] = (uint8_t)(state >> 24);
    }
}

static void make_keys(cf_device_keys *keys, uint8_t first, uint8_t second) {
    memset(keys, 0, sizeof(*keys));
    for (size_t i = 0; i < 32; ++i) {
        keys->key1[i] = (uint8_t)(first + i);
        keys->key2[i] = (uint8_t)(second + i);
    }
    keys->key2[0] = 0x01; /* definitely below the secp256k1 order */
    keys->key2[1] = 0x00;
}

/* The device already has a different, valid identity and non-erased data. */
static void program_existing_device(nor_flash *flash) {
    cf_device_keys old;
    make_keys(&old, 0xA0, 0x20);
    uint8_t page[PAGE_SIZE];
    cf_key_record_encode(&old, page);
    memcpy(flash->data, page, PAGE_SIZE);
    memcpy(flash->data + PAGE_SIZE, page, PAGE_SIZE);
    fill_bytes(flash->data + CF_FIDO_FS_OFFSET,
               flash->size - CF_FIDO_FS_OFFSET, 0xABCD1234u);
}

static void make_source(uint8_t *source, cf_device_keys *keys) {
    make_keys(keys, 0x30, 0x40);
    uint8_t page[PAGE_SIZE];
    cf_key_record_encode(keys, page);
    memcpy(source, page, PAGE_SIZE);
    memcpy(source + PAGE_SIZE, page, PAGE_SIZE);
    fill_bytes(source + CF_FIDO_FS_OFFSET,
               STORE_SIZE - CF_FIDO_FS_OFFSET, 0x5EEDBEEFu);
}

static void apply_chunks(cf_backup_store *store, const uint8_t *source,
                         uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t offset = i * PAGE_SIZE;
        REQUIRE(cf_backup_store_write(store, offset, source + offset,
                                      PAGE_SIZE) == 0);
    }
}

static void test_success_and_key_pages_last(void) {
    nor_flash flash;
    REQUIRE(nor_init(&flash) == 0);
    program_existing_device(&flash);

    uint8_t *source = malloc(STORE_SIZE);
    REQUIRE(source != NULL);
    cf_device_keys source_keys;
    make_source(source, &source_keys);

    cf_backup_store store;
    init_store(&store, &flash);

    for (uint32_t i = 0; i < CHUNK_COUNT; ++i) {
        const uint32_t offset = i * PAGE_SIZE;
        REQUIRE(cf_backup_store_write(&store, offset, source + offset,
                                      PAGE_SIZE) == 0);
        if (i >= 2) {
            cf_device_keys loaded;
            const cf_key_status status = load_device(&flash, &loaded);
            /* The credential filesystem (or only part of it) is visible while
             * the key pages stay erased, so load must never publish identity. */
            REQUIRE(status != CF_KEY_READY);
            REQUIRE(status != CF_KEY_EMPTY);
            memset(&loaded, 0, sizeof(loaded));
        }
    }
    check_int("staged next", (long)store.next, STORE_SIZE);
    REQUIRE(memcmp(flash.data, "RESTORE!", 8) == 0);
    REQUIRE(memcmp(flash.data + 8, "\xFF\xFF\xFF\xFF", 4) == 0);
    {
        cf_device_keys loaded;
        REQUIRE(load_device(&flash, &loaded) == CF_KEY_CORRUPT);
        memset(&loaded, 0, sizeof(loaded));
    }

    const int programs_before_finish = flash.program_calls;
    for (int i = 0; i < programs_before_finish; ++i) {
        const int is_key_page =
            flash.program_lengths[i] == PAGE_SIZE &&
            (flash.program_offsets[i] == 0 ||
             flash.program_offsets[i] == PAGE_SIZE);
        REQUIRE(!is_key_page);
    }

    const int erases_before_finish = flash.erase_calls;
    REQUIRE(cf_backup_store_finish(&store) == 0);
    check_int("finish erases", flash.erase_calls, erases_before_finish + 1);
    check_int("finish erase offset",
              (long)flash.erase_offsets[flash.erase_calls - 1], 0);
    check_int("finish erase length",
              (long)flash.erase_lengths[flash.erase_calls - 1],
              CF_FIDO_FS_OFFSET);
    check_int("finish programs", flash.program_calls, programs_before_finish + 2);
    check_int("first key page committed last",
              (long)flash.program_offsets[flash.program_calls - 2], 0);
    check_int("first key page length",
              (long)flash.program_lengths[flash.program_calls - 2], PAGE_SIZE);
    check_int("second key page committed last",
              (long)flash.program_offsets[flash.program_calls - 1], PAGE_SIZE);
    check_int("second key page length",
              (long)flash.program_lengths[flash.program_calls - 1], PAGE_SIZE);

    REQUIRE(memcmp(flash.data, source, STORE_SIZE) == 0);

    cf_device_keys loaded;
    REQUIRE(load_device(&flash, &loaded) == CF_KEY_READY);
    REQUIRE(memcmp(&loaded, &source_keys, sizeof(loaded)) == 0);
    memset(&loaded, 0, sizeof(loaded));

    free(source);
    nor_free(&flash);
}

static void test_invalid_keys_reject_before_erase(void) {
    uint8_t valid_page[PAGE_SIZE];
    cf_device_keys keys_a;
    cf_device_keys keys_b;
    make_keys(&keys_a, 0x10, 0x50);
    make_keys(&keys_b, 0x70, 0x90);
    cf_key_record_encode(&keys_a, valid_page);
    uint8_t other_page[PAGE_SIZE];
    uint8_t *valid_source = malloc(STORE_SIZE);
    REQUIRE(valid_source != NULL);
    cf_device_keys source_keys;
    make_source(valid_source, &source_keys);
    cf_key_record_encode(&keys_b, other_page);

    for (int variant = 0; variant < 3; ++variant) {
        nor_flash flash;
        REQUIRE(nor_init(&flash) == 0);
        program_existing_device(&flash);
        uint8_t *before = malloc(STORE_SIZE);
        REQUIRE(before != NULL);
        memcpy(before, flash.data, STORE_SIZE);

        uint8_t page_a[PAGE_SIZE];
        uint8_t page_b[PAGE_SIZE];
        if (variant == 0) {
            memset(page_a, 0xFF, PAGE_SIZE);
            memset(page_b, 0xFF, PAGE_SIZE);
        } else if (variant == 1) {
            memset(page_a, 0x00, PAGE_SIZE);
            memset(page_b, 0x00, PAGE_SIZE);
        } else {
            memcpy(page_a, valid_page, PAGE_SIZE);
            memcpy(page_b, other_page, PAGE_SIZE); /* mismatched identities */
        }

        cf_backup_store store;
        init_store(&store, &flash);
        REQUIRE(cf_backup_store_write(&store, 0, page_a, PAGE_SIZE) == 0);
        REQUIRE(cf_backup_store_write(&store, PAGE_SIZE, page_b, PAGE_SIZE) == 0);
        check_int("invalid keys staged next", (long)store.next,
                  CF_FIDO_FS_OFFSET);

        const int erases = flash.erase_calls;
        const int programs = flash.program_calls;
        REQUIRE(cf_backup_store_write(&store, CF_FIDO_FS_OFFSET,
                                      valid_source + CF_FIDO_FS_OFFSET,
                                      PAGE_SIZE) == -1);
        check_int("invalid keys erase calls", flash.erase_calls, erases);
        check_int("invalid keys program calls", flash.program_calls, programs);
        check_int("invalid keys next unchanged", (long)store.next,
                  CF_FIDO_FS_OFFSET);
        REQUIRE(memcmp(flash.data, before, STORE_SIZE) == 0);

        cf_device_keys loaded;
        REQUIRE(load_device(&flash, &loaded) == CF_KEY_READY);
        memset(&loaded, 0, sizeof(loaded));

        free(before);
        nor_free(&flash);
    }

    free(valid_source);
}

static void test_single_valid_key_page_accepted(void) {
    for (int valid_first = 0; valid_first < 2; ++valid_first) {
        nor_flash flash;
        REQUIRE(nor_init(&flash) == 0);
        program_existing_device(&flash);

        uint8_t *source = malloc(STORE_SIZE);
        REQUIRE(source != NULL);
        cf_device_keys source_keys;
        make_source(source, &source_keys);

        uint8_t valid_page[PAGE_SIZE];
        cf_key_record_encode(&source_keys, valid_page);
        uint8_t erased_page[PAGE_SIZE];
        memset(erased_page, 0xFF, PAGE_SIZE);

        cf_backup_store store;
        init_store(&store, &flash);
        const uint8_t *page_a = valid_first ? valid_page : erased_page;
        const uint8_t *page_b = valid_first ? erased_page : valid_page;
        memcpy(source, page_a, PAGE_SIZE);
        memcpy(source + PAGE_SIZE, page_b, PAGE_SIZE);
        REQUIRE(cf_backup_store_write(&store, 0, page_a, PAGE_SIZE) == 0);
        REQUIRE(cf_backup_store_write(&store, PAGE_SIZE, page_b, PAGE_SIZE) == 0);
        REQUIRE(cf_backup_store_write(&store, CF_FIDO_FS_OFFSET,
                                      source + CF_FIDO_FS_OFFSET,
                                      PAGE_SIZE) == 0);
        check_int("single valid page erase calls", flash.erase_calls, 2);

        for (uint32_t i = 3; i < CHUNK_COUNT; ++i) {
            REQUIRE(cf_backup_store_write(&store, i * PAGE_SIZE,
                                          source + i * PAGE_SIZE,
                                          PAGE_SIZE) == 0);
        }
        REQUIRE(cf_backup_store_finish(&store) == 0);
        REQUIRE(memcmp(flash.data, source, STORE_SIZE) == 0);

        cf_device_keys loaded;
        REQUIRE(load_device(&flash, &loaded) == CF_KEY_READY);
        REQUIRE(memcmp(&loaded, &source_keys, sizeof(loaded)) == 0);
        memset(&loaded, 0, sizeof(loaded));

        free(source);
        nor_free(&flash);
    }
}

static void test_out_of_order_and_argument_rejection(void) {
    nor_flash flash;
    REQUIRE(nor_init(&flash) == 0);
    program_existing_device(&flash);
    uint8_t *source = malloc(STORE_SIZE);
    REQUIRE(source != NULL);
    cf_device_keys source_keys;
    make_source(source, &source_keys);

    cf_backup_store store;
    init_store(&store, &flash);

    REQUIRE(cf_backup_store_write(NULL, 0, source, PAGE_SIZE) == -1);
    REQUIRE(cf_backup_store_write(&store, 0, NULL, PAGE_SIZE) == -1);
    REQUIRE(cf_backup_store_write(&store, 0, source, 0) == -1);
    REQUIRE(cf_backup_store_write(&store, 0, source, PAGE_SIZE - 1) == -1);
    REQUIRE(cf_backup_store_write(&store, 0, source, PAGE_SIZE + 1) == -1);
    REQUIRE(cf_backup_store_write(&store, PAGE_SIZE, source + PAGE_SIZE,
                                  PAGE_SIZE) == -1);
    check_int("argument rejection erase calls", flash.erase_calls, 0);
    check_int("argument rejection program calls", flash.program_calls, 0);

    cf_backup_store missing;
    memset(&missing, 0, sizeof(missing));
    REQUIRE(cf_backup_store_write(&missing, 0, source, PAGE_SIZE) == -1);

    REQUIRE(cf_backup_store_write(&store, 0, source, PAGE_SIZE) == 0);
    REQUIRE(cf_backup_store_write(&store, 0, source, PAGE_SIZE) == -1);
    REQUIRE(cf_backup_store_write(&store, CF_FIDO_FS_OFFSET,
                                  source + CF_FIDO_FS_OFFSET, PAGE_SIZE) == -1);
    REQUIRE(cf_backup_store_write(&store, PAGE_SIZE, source + PAGE_SIZE,
                                  PAGE_SIZE) == 0);

    cf_backup_store beyond;
    init_store(&beyond, &flash);
    beyond.next = STORE_SIZE;
    REQUIRE(cf_backup_store_write(&beyond, STORE_SIZE, source, PAGE_SIZE) == -1);
    check_int("out-of-order erase calls", flash.erase_calls, 0);
    check_int("out-of-order program calls", flash.program_calls, 0);

    free(source);
    nor_free(&flash);
}

static void test_incomplete_finalize_refused(void) {
    nor_flash flash;
    REQUIRE(nor_init(&flash) == 0);
    program_existing_device(&flash);
    uint8_t *source = malloc(STORE_SIZE);
    REQUIRE(source != NULL);
    cf_device_keys source_keys;
    make_source(source, &source_keys);

    cf_backup_store store;
    init_store(&store, &flash);
    REQUIRE(cf_backup_store_finish(NULL) == -1);
    REQUIRE(cf_backup_store_finish(&store) == -1);
    check_int("fresh finish erase calls", flash.erase_calls, 0);
    check_int("fresh finish program calls", flash.program_calls, 0);

    apply_chunks(&store, source, CHUNK_COUNT - 1);
    const int erases = flash.erase_calls;
    const int programs = flash.program_calls;
    REQUIRE(cf_backup_store_finish(&store) == -1);
    check_int("incomplete finish erase calls", flash.erase_calls, erases);
    check_int("incomplete finish program calls", flash.program_calls, programs);
    cf_device_keys loaded;
    REQUIRE(load_device(&flash, &loaded) != CF_KEY_READY);
    REQUIRE(load_device(&flash, &loaded) != CF_KEY_EMPTY);
    memset(&loaded, 0, sizeof(loaded));

    /* Complete the final chunk; the staged keys are then deliberately
     * destroyed to prove finish validates them before touching flash. */
    REQUIRE(cf_backup_store_write(&store, (CHUNK_COUNT - 1) * PAGE_SIZE,
                                  source + (CHUNK_COUNT - 1) * PAGE_SIZE,
                                  PAGE_SIZE) == 0);
    memset(store.keys, 0, sizeof(store.keys));
    const int erases_before = flash.erase_calls;
    const int programs_before = flash.program_calls;
    REQUIRE(cf_backup_store_finish(&store) == -1);
    check_int("invalid staged keys finish erase calls", flash.erase_calls,
              erases_before);
    check_int("invalid staged keys finish program calls", flash.program_calls,
              programs_before);
    memset(&loaded, 0, sizeof(loaded));
    REQUIRE(load_device(&flash, &loaded) != CF_KEY_READY);
    REQUIRE(load_device(&flash, &loaded) != CF_KEY_EMPTY);

    free(source);
    nor_free(&flash);
}

static void test_injected_failures_never_ready(void) {
    uint8_t *source = malloc(STORE_SIZE);
    REQUIRE(source != NULL);
    cf_device_keys source_keys;
    make_source(source, &source_keys);

    /* Failure during the first key-region erase: nothing was modified yet. */
    {
        nor_flash flash;
        REQUIRE(nor_init(&flash) == 0);
        program_existing_device(&flash);
        cf_backup_store store;
        init_store(&store, &flash);
        apply_chunks(&store, source, 2);
        flash.fail_erase_at = flash.erase_calls + 1;
        REQUIRE(cf_backup_store_write(&store, CF_FIDO_FS_OFFSET,
                                      source + CF_FIDO_FS_OFFSET,
                                      PAGE_SIZE) == -1);
        check_int("key erase failure program calls", flash.program_calls, 0);
        cf_device_keys loaded;
        REQUIRE(load_device(&flash, &loaded) == CF_KEY_READY);
        memset(&loaded, 0, sizeof(loaded));
        nor_free(&flash);
    }

    /* Marker program failure and marker readback failure. */
    for (int mode = 0; mode < 2; ++mode) {
        nor_flash flash;
        REQUIRE(nor_init(&flash) == 0);
        program_existing_device(&flash);
        cf_backup_store store;
        init_store(&store, &flash);
        apply_chunks(&store, source, 2);
        if (mode == 0) {
            flash.fail_program_at = flash.program_calls + 1;
        } else {
            flash.fail_read_at = flash.read_calls + 1;
        }
        REQUIRE(cf_backup_store_write(&store, CF_FIDO_FS_OFFSET,
                                      source + CF_FIDO_FS_OFFSET,
                                      PAGE_SIZE) == -1);
        cf_device_keys loaded;
        const cf_key_status status = load_device(&flash, &loaded);
        REQUIRE(status != CF_KEY_READY);
        REQUIRE(status != CF_KEY_EMPTY);
        memset(&loaded, 0, sizeof(loaded));
        nor_free(&flash);
    }

    /* Erase, program and readback failures on the first filesystem chunk. */
    for (int mode = 0; mode < 3; ++mode) {
        nor_flash flash;
        REQUIRE(nor_init(&flash) == 0);
        program_existing_device(&flash);
        cf_backup_store store;
        init_store(&store, &flash);
        apply_chunks(&store, source, 2);
        if (mode == 0) {
            flash.fail_erase_at = flash.erase_calls + 2;
        } else if (mode == 1) {
            flash.fail_program_at = flash.program_calls + 2;
        } else {
            flash.fail_read_at = flash.read_calls + 2;
        }
        REQUIRE(cf_backup_store_write(&store, CF_FIDO_FS_OFFSET,
                                      source + CF_FIDO_FS_OFFSET,
                                      PAGE_SIZE) == -1);
        cf_device_keys loaded;
        const cf_key_status status = load_device(&flash, &loaded);
        REQUIRE(status != CF_KEY_READY);
        REQUIRE(status != CF_KEY_EMPTY);
        memset(&loaded, 0, sizeof(loaded));
        nor_free(&flash);
    }

    /* Failure in the middle of the filesystem application. */
    {
        nor_flash flash;
        REQUIRE(nor_init(&flash) == 0);
        program_existing_device(&flash);
        cf_backup_store store;
        init_store(&store, &flash);
        apply_chunks(&store, source, 2 + 100);
        flash.fail_program_at = flash.program_calls + 1;
        const uint32_t offset = (2 + 100) * PAGE_SIZE;
        REQUIRE(cf_backup_store_write(&store, offset, source + offset,
                                      PAGE_SIZE) == -1);
        check_int("mid apply next", (long)store.next, offset);
        cf_device_keys loaded;
        const cf_key_status status = load_device(&flash, &loaded);
        REQUIRE(status != CF_KEY_READY);
        REQUIRE(status != CF_KEY_EMPTY);
        memset(&loaded, 0, sizeof(loaded));
        nor_free(&flash);
    }

    /* Failure while committing the key pages in finish. */
    for (int mode = 0; mode < 2; ++mode) {
        nor_flash flash;
        REQUIRE(nor_init(&flash) == 0);
        program_existing_device(&flash);
        cf_backup_store store;
        init_store(&store, &flash);
        apply_chunks(&store, source, CHUNK_COUNT);
        if (mode == 0) {
            flash.fail_erase_at = flash.erase_calls + 1;
        } else {
            flash.fail_program_at = flash.program_calls + 1;
        }
        REQUIRE(cf_backup_store_finish(&store) == -1);
        cf_device_keys loaded;
        const cf_key_status status = load_device(&flash, &loaded);
        REQUIRE(status != CF_KEY_READY);
        REQUIRE(status != CF_KEY_EMPTY);
        memset(&loaded, 0, sizeof(loaded));
        nor_free(&flash);
    }

    free(source);
}

int main(void) {
    test_success_and_key_pages_last();
    test_invalid_keys_reject_before_erase();
    test_single_valid_key_page_accepted();
    test_out_of_order_and_argument_rejection();
    test_incomplete_finalize_refused();
    test_injected_failures_never_ready();
    puts("backup_store_test: all checks passed");
    return 0;
}
