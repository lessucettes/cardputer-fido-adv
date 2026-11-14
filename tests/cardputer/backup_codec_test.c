/* Behavioral tests for the encrypted Cardputer-FIDO backup codec.
 *
 * The codec uses real mbedTLS AES-256-GCM and PBKDF2-HMAC-SHA256 with an
 * injected entropy source and store reader. These tests exercise the public
 * API only. A Python companion (backup_codec_test.py) independently parses an
 * emitted envelope with hashlib and AES-GCM from `cryptography`.
 *
 * Build (from the repository root, default mbedTLS configuration):
 *   cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
 *      -I src/cardputer -I <mbedtls>/include \
 *      src/cardputer/backup_codec.c tests/cardputer/backup_codec_test.c \
 *      <mbedtls>/library/libmbedcrypto.a -o backup_codec_test
 *
 * `--emit <path>` writes one deterministic envelope for the Python oracle.
 */
#define _GNU_SOURCE

#include "backup_codec.h"
#include "flash_layout.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define CHUNK_COUNT (CF_FIDO_STORE_SIZE / CF_BACKUP_CHUNK)
#define RECORD_SIZE (CF_BACKUP_CHUNK + 16u)
#define BACKUP_SIZE (CF_BACKUP_HEADER + CHUNK_COUNT * RECORD_SIZE)

#define TEST_STORE_SEED 0x5Au
#define TEST_RNG_SEED 0x12345678u
#define ALT_STORE_SEED 0x77u
#define ALT_RNG_SEED 0x87654321u

static const char kPassword[] = "correct horse battery staple";
static const uint8_t kDevice[32] = {
    0x5A, 0x11, 0x42, 0x00, 0xFF, 0x7E, 0x31, 0x88,
    0x09, 0xC4, 0x5D, 0x63, 0xA0, 0x1B, 0xE7, 0x2C,
    0x74, 0x8D, 0x16, 0xFA, 0x39, 0x52, 0xBE, 0x0E,
    0x91, 0x6A, 0xD3, 0x47, 0x85, 0xCF, 0x20, 0x3B
};

static uint8_t g_store[CF_FIDO_STORE_SIZE];
static uint8_t g_alt_store[CF_FIDO_STORE_SIZE];
static uint8_t g_backup[BACKUP_SIZE + 1u];
static uint8_t g_alt_backup[BACKUP_SIZE + 1u];
static uint8_t g_work[BACKUP_SIZE + RECORD_SIZE + 1u];
static size_t g_backup_size;
static size_t g_alt_backup_size;

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

static uint32_t rd32(const uint8_t *in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static void wr32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static void fill_store(uint8_t *store, uint8_t seed) {
    uint32_t state = 0x9E3779B9u ^ (uint32_t)seed;
    for (size_t i = 0; i < CF_FIDO_STORE_SIZE; ++i) {
        state = state * 1664525u + 1013904223u;
        store[i] = (uint8_t)(state >> 24);
    }
    /* Distinct marker at every chunk start lets the leak check prove that no
     * plaintext block survives anywhere in the envelope. */
    for (uint32_t i = 0; i < CHUNK_COUNT; ++i) {
        char marker[32];
        const int n = snprintf(marker, sizeof(marker), "CFPLAIN-%03u-MARKER", i);
        REQUIRE(n > 0 && (size_t)n < sizeof(marker));
        memcpy(store + (size_t)i * CF_BACKUP_CHUNK, marker, (size_t)n);
    }
}

typedef struct {
    uint32_t state;
    int fail;
    int calls;
} test_rng;

static void rng_fill(uint32_t *state, uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        *state = *state * 1664525u + 1013904223u;
        out[i] = (uint8_t)(*state >> 24);
    }
}

static int rng_cb(void *ctx, uint8_t *out, size_t len) {
    test_rng *rng = ctx;
    rng->calls++;
    if (rng->fail) {
        return -1;
    }
    rng_fill(&rng->state, out, len);
    return 0;
}

typedef struct {
    const uint8_t *data;
    size_t size;
    int calls;
    int fail_at;
} test_reader;

static int reader_cb(void *ctx, uint32_t offset, uint8_t *out, size_t len) {
    test_reader *reader = ctx;
    reader->calls++;
    if (reader->fail_at > 0 && reader->calls == reader->fail_at) {
        return -1;
    }
    if ((size_t)offset > reader->size || len > reader->size - offset) {
        return -1;
    }
    memcpy(out, reader->data + offset, len);
    return 0;
}

typedef struct {
    uint8_t data[CF_FIDO_STORE_SIZE];
    uint32_t offsets[CHUNK_COUNT + 8u];
    size_t lengths[CHUNK_COUNT + 8u];
    int calls;
    int fail_at;
} test_writer;

static int writer_cb(void *ctx, uint32_t offset, const uint8_t *data,
                     size_t len) {
    test_writer *writer = ctx;
    writer->calls++;
    if (writer->fail_at > 0 && writer->calls == writer->fail_at) {
        return -1;
    }
    if ((size_t)offset > sizeof(writer->data) ||
        len > sizeof(writer->data) - offset ||
        writer->calls > (int)(sizeof(writer->offsets) / sizeof(writer->offsets[0]))) {
        return -1;
    }
    memcpy(writer->data + offset, data, len);
    writer->offsets[writer->calls - 1] = offset;
    writer->lengths[writer->calls - 1] = len;
    return 0;
}

static FILE *open_read_mem(const uint8_t *data, size_t len) {
    FILE *file = fmemopen((void *)data, len, "rb");
    REQUIRE(file != NULL);
    return file;
}

static FILE *open_empty_read(void) {
    FILE *file = tmpfile();
    REQUIRE(file != NULL);
    REQUIRE(fseek(file, 0, SEEK_SET) == 0);
    return file;
}

static int export_mem(uint8_t *out, size_t capacity, const uint8_t *password,
                      size_t password_len, const uint8_t *device,
                      test_rng *rng, test_reader *reader, size_t *out_size) {
    FILE *file = fmemopen(out, capacity, "wb");
    REQUIRE(file != NULL);
    const int result = cf_backup_export(file, password, password_len, device,
                                        reader_cb, reader, rng_cb, rng);
    REQUIRE(fflush(file) == 0 || result != CF_BACKUP_OK);
    const long position = ftell(file);
    REQUIRE(position >= 0);
    *out_size = (size_t)position;
    REQUIRE(fclose(file) == 0 || result != CF_BACKUP_OK);
    return result;
}

static int verify_mem(const uint8_t *data, size_t len, const uint8_t *password,
                      size_t password_len, const uint8_t *device) {
    FILE *file = open_read_mem(data, len);
    const int result = cf_backup_verify(file, password, password_len, device);
    fclose(file);
    return result;
}

static int restore_mem(const uint8_t *data, size_t len, const uint8_t *password,
                       size_t password_len, const uint8_t *device,
                       test_writer *writer) {
    FILE *file = open_read_mem(data, len);
    const int result =
        cf_backup_restore(file, password, password_len, device, writer_cb, writer);
    fclose(file);
    return result;
}

static void init_writer(test_writer *writer) {
    memset(writer, 0, sizeof(*writer));
    writer->fail_at = -1;
}

static void make_fixture(void) {
    fill_store(g_store, TEST_STORE_SEED);
    test_rng rng = {TEST_RNG_SEED, 0, 0};
    test_reader reader = {g_store, sizeof(g_store), 0, -1};
    const int result =
        export_mem(g_backup, sizeof(g_backup), (const uint8_t *)kPassword,
                   strlen(kPassword), kDevice, &rng, &reader,
                   &g_backup_size);
    REQUIRE(result == CF_BACKUP_OK);
    REQUIRE(g_backup_size == BACKUP_SIZE);
    check_int("fixture random calls", rng.calls, 1);
    check_int("fixture read calls", reader.calls, CHUNK_COUNT);

    fill_store(g_alt_store, ALT_STORE_SEED);
    test_rng alt_rng = {ALT_RNG_SEED, 0, 0};
    test_reader alt_reader = {g_alt_store, sizeof(g_alt_store), 0, -1};
    REQUIRE(export_mem(g_alt_backup, sizeof(g_alt_backup),
                       (const uint8_t *)kPassword, strlen(kPassword), kDevice,
                       &alt_rng, &alt_reader, &g_alt_backup_size) ==
            CF_BACKUP_OK);
    REQUIRE(g_alt_backup_size == BACKUP_SIZE);
}

static void test_envelope_format(void) {
    REQUIRE(g_backup_size == BACKUP_SIZE);
    REQUIRE(memcmp(g_backup, "CFBAK001", 8) == 0);
    check_int("version", rd32(g_backup + 8), 1);
    check_int("store size", (long)rd32(g_backup + 12),
              (long)CF_FIDO_STORE_SIZE);
    check_int("chunk size", rd32(g_backup + 16), CF_BACKUP_CHUNK);
    check_int("iterations", rd32(g_backup + 20), CF_BACKUP_ITERATIONS);
    REQUIRE(memcmp(g_backup + 48, kDevice, 32) == 0);

    uint8_t expected_random[24];
    uint32_t state = TEST_RNG_SEED;
    rng_fill(&state, expected_random, sizeof(expected_random));
    REQUIRE(memcmp(g_backup + 24, expected_random, sizeof(expected_random)) == 0);
    REQUIRE(memcmp(g_backup + 24, g_alt_backup + 24, 24) != 0);

    int salt_nonzero = 0;
    int prefix_nonzero = 0;
    for (size_t i = 0; i < 16; ++i) {
        salt_nonzero |= g_backup[24 + i];
        prefix_nonzero |= g_backup[40 + i];
    }
    REQUIRE(salt_nonzero != 0);
    REQUIRE(prefix_nonzero != 0);

    for (uint32_t i = 0; i < CHUNK_COUNT; ++i) {
        const uint8_t *plain = g_store + (size_t)i * CF_BACKUP_CHUNK;
        const uint8_t *cipher = g_backup + CF_BACKUP_HEADER +
                                (size_t)i * RECORD_SIZE;
        REQUIRE(memcmp(cipher, plain, CF_BACKUP_CHUNK) != 0);
    }

    for (uint32_t i = 0; i < CHUNK_COUNT; ++i) {
        char marker[32];
        const int n = snprintf(marker, sizeof(marker), "CFPLAIN-%03u-MARKER", i);
        REQUIRE(n > 0);
        REQUIRE(memmem(g_backup, g_backup_size, marker, (size_t)n) == NULL);
        REQUIRE(memmem(g_backup, g_backup_size, g_store + (size_t)i * CF_BACKUP_CHUNK,
                       128) == NULL);
    }
}

static void test_roundtrip_all_bytes(void) {
    REQUIRE(verify_mem(g_backup, g_backup_size, (const uint8_t *)kPassword,
                       strlen(kPassword), kDevice) == CF_BACKUP_OK);
    test_writer writer;
    init_writer(&writer);
    REQUIRE(restore_mem(g_backup, g_backup_size, (const uint8_t *)kPassword,
                        strlen(kPassword), kDevice, &writer) == CF_BACKUP_OK);
    check_int("restore writes", writer.calls, CHUNK_COUNT);
    for (uint32_t i = 0; i < CHUNK_COUNT; ++i) {
        check_int("restore offset", writer.offsets[i], (long)i * CF_BACKUP_CHUNK);
        check_int("restore length", (long)writer.lengths[i], CF_BACKUP_CHUNK);
    }
    REQUIRE(memcmp(writer.data, g_store, CF_FIDO_STORE_SIZE) == 0);
}

static void test_password_validation(void) {
    uint8_t scratch[BACKUP_SIZE];
    uint8_t password[80];
    memset(password, 'A', sizeof(password));

    const struct {
        const uint8_t *password;
        size_t len;
        int expected;
    } cases[] = {
        {NULL, 12, CF_BACKUP_INVALID},
        {password, 0, CF_BACKUP_INVALID},
        {password, 11, CF_BACKUP_INVALID},
        {password, 65, CF_BACKUP_INVALID},
        {password, 80, CF_BACKUP_INVALID},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        test_rng rng = {TEST_RNG_SEED, 0, 0};
        test_reader reader = {g_store, sizeof(g_store), 0, -1};
        size_t written = 12345;
        check_int("rejected password export",
                  export_mem(scratch, sizeof(scratch), cases[i].password,
                             cases[i].len, kDevice, &rng, &reader,
                             &written),
                  cases[i].expected);
        check_int("rejected password random calls", rng.calls, 0);
        check_int("rejected password read calls", reader.calls, 0);
        check_int("rejected password file bytes", (long)written, 0);
    }

    memcpy(password, kPassword, strlen(kPassword));
    password[5] = 0x1F;
    {
        test_rng rng = {TEST_RNG_SEED, 0, 0};
        test_reader reader = {g_store, sizeof(g_store), 0, -1};
        size_t written = 0;
        check_int("control character export",
                  export_mem(scratch, sizeof(scratch), password, strlen(kPassword),
                             kDevice, &rng, &reader, &written),
                  CF_BACKUP_INVALID);
        check_int("control character random calls", rng.calls, 0);
    }
    memcpy(password, kPassword, strlen(kPassword));
    password[7] = 0x7F;
    {
        test_rng rng = {TEST_RNG_SEED, 0, 0};
        test_reader reader = {g_store, sizeof(g_store), 0, -1};
        size_t written = 0;
        check_int("DEL export",
                  export_mem(scratch, sizeof(scratch), password, strlen(kPassword),
                             kDevice, &rng, &reader, &written),
                  CF_BACKUP_INVALID);
    }

    {
        test_rng rng = {TEST_RNG_SEED, 0, 0};
        test_reader reader = {g_store, sizeof(g_store), 0, -1};
        size_t written = 0;
        check_int("NULL file", cf_backup_export(NULL, (const uint8_t *)kPassword,
                                                strlen(kPassword), kDevice,
                                                reader_cb, &reader, rng_cb, &rng),
                  CF_BACKUP_INVALID);
        check_int("NULL device",
                  cf_backup_export((FILE *)stdout, (const uint8_t *)kPassword,
                                   strlen(kPassword), NULL, reader_cb, &reader,
                                   rng_cb, &rng),
                  CF_BACKUP_INVALID);
        check_int("NULL read",
                  cf_backup_export((FILE *)stdout, (const uint8_t *)kPassword,
                                   strlen(kPassword), kDevice, NULL, &reader,
                                   rng_cb, &rng),
                  CF_BACKUP_INVALID);
        check_int("NULL random",
                  cf_backup_export((FILE *)stdout, (const uint8_t *)kPassword,
                                   strlen(kPassword), kDevice, reader_cb,
                                   &reader, NULL, &rng),
                  CF_BACKUP_INVALID);
        check_int("invalid arguments wrote no random", rng.calls, 0);
        check_int("invalid arguments wrote no bytes", (long)written, 0);
    }

    /* Minimum and maximum accepted lengths must survive a real round trip. */
    memset(password, 'A', sizeof(password));
    const struct {
        size_t len;
        const char *name;
    } accepted[] = {{12, "minimum password"}, {64, "maximum password"}};
    for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); ++i) {
        test_rng rng = {TEST_RNG_SEED, 0, 0};
        test_reader reader = {g_store, sizeof(g_store), 0, -1};
        size_t written = 0;
        check_int(accepted[i].name,
                  export_mem(g_work, sizeof(g_work), password, accepted[i].len,
                             kDevice, &rng, &reader, &written),
                  CF_BACKUP_OK);
        check_int("accepted password envelope", (long)written, BACKUP_SIZE);
        if (accepted[i].len == CF_BACKUP_PASSWORD_MAX) {
            check_int("maximum password verify",
                      verify_mem(g_work, written, password, accepted[i].len,
                                 kDevice),
                      CF_BACKUP_OK);
        }
    }

    check_int("oversize verify password",
              verify_mem(g_backup, g_backup_size, password, 65, kDevice),
              CF_BACKUP_INVALID);
    password[3] = 0x1F;
    check_int("non-printable verify password",
              verify_mem(g_backup, g_backup_size, password, 64, kDevice),
              CF_BACKUP_INVALID);
    check_int("NULL verify password",
              verify_mem(g_backup, g_backup_size, NULL, 12, kDevice),
              CF_BACKUP_INVALID);
    check_int("NULL verify file",
              cf_backup_verify(NULL, (const uint8_t *)kPassword, strlen(kPassword),
                               kDevice),
              CF_BACKUP_INVALID);
}

static void test_wrong_password(void) {
    uint8_t wrong[sizeof(kPassword)];
    memcpy(wrong, kPassword, sizeof(kPassword));
    wrong[0] = (uint8_t)(wrong[0] ^ 0x01);
    REQUIRE(memcmp(wrong, kPassword, strlen(kPassword)) != 0);

    test_writer writer;
    init_writer(&writer);
    check_int("wrong password restore",
              restore_mem(g_backup, g_backup_size, wrong, strlen(kPassword),
                          kDevice, &writer),
              CF_BACKUP_AUTH);
    check_int("wrong password write callbacks", writer.calls, 0);
}

static void test_device_binding(void) {
    uint8_t other[32];
    memcpy(other, kDevice, sizeof(other));
    other[31] ^= 0x01;

    test_writer writer;
    init_writer(&writer);
    check_int("other device restore",
              restore_mem(g_backup, g_backup_size, (const uint8_t *)kPassword,
                          strlen(kPassword), other, &writer),
              CF_BACKUP_INVALID);
    check_int("other device write callbacks", writer.calls, 0);
    check_int("other device verify",
              verify_mem(g_backup, g_backup_size, (const uint8_t *)kPassword,
                         strlen(kPassword), other),
              CF_BACKUP_INVALID);

    init_writer(&writer);
    check_int("NULL device restore",
              restore_mem(g_backup, g_backup_size, (const uint8_t *)kPassword,
                          strlen(kPassword), NULL, &writer),
              CF_BACKUP_INVALID);
    check_int("NULL device write callbacks", writer.calls, 0);
}

static void test_corruption_before_any_write(void) {
    test_writer writer;

    memcpy(g_work, g_backup, g_backup_size);
    g_work[g_backup_size - 1] ^= 0x80; /* last GCM tag */
    init_writer(&writer);
    check_int("last tag restore",
              restore_mem(g_work, g_backup_size, (const uint8_t *)kPassword,
                          strlen(kPassword), kDevice, &writer),
              CF_BACKUP_AUTH);
    check_int("last tag write callbacks", writer.calls, 0);

    memcpy(g_work, g_backup, g_backup_size);
    g_work[24] ^= 0x01; /* salt: correct header, wrong key */
    init_writer(&writer);
    check_int("salt corruption restore",
              restore_mem(g_work, g_backup_size, (const uint8_t *)kPassword,
                          strlen(kPassword), kDevice, &writer),
              CF_BACKUP_AUTH);
    check_int("salt corruption write callbacks", writer.calls, 0);
}

static void test_truncation_and_trailing(void) {
    test_writer writer;

    {
        FILE *empty = open_empty_read();
        check_int("empty header verify",
                  cf_backup_verify(empty, (const uint8_t *)kPassword,
                                   strlen(kPassword), kDevice),
                  CF_BACKUP_IO);
        fclose(empty);
    }
    check_int("truncated header verify",
              verify_mem(g_backup, CF_BACKUP_HEADER - 1,
                         (const uint8_t *)kPassword, strlen(kPassword), kDevice),
              CF_BACKUP_IO);
    init_writer(&writer);
    check_int("truncated header restore",
              restore_mem(g_backup, CF_BACKUP_HEADER - 1,
                          (const uint8_t *)kPassword, strlen(kPassword), kDevice,
                          &writer),
              CF_BACKUP_IO);
    check_int("truncated header write callbacks", writer.calls, 0);

    init_writer(&writer);
    check_int("missing final tag byte restore",
              restore_mem(g_backup, g_backup_size - 1,
                          (const uint8_t *)kPassword, strlen(kPassword), kDevice,
                          &writer),
              CF_BACKUP_IO);
    check_int("missing final tag byte write callbacks", writer.calls, 0);

    memcpy(g_work, g_backup, g_backup_size);
    g_work[g_backup_size] = 0x00;
    init_writer(&writer);
    check_int("trailing byte restore",
              restore_mem(g_work, g_backup_size + 1,
                          (const uint8_t *)kPassword, strlen(kPassword), kDevice,
                          &writer),
              CF_BACKUP_INVALID);
    check_int("trailing byte write callbacks", writer.calls, 0);
}

static void expect_header_reject(uint8_t *mutated, const char *name) {
    test_writer writer;
    init_writer(&writer);
    check_int(name, restore_mem(mutated, g_backup_size,
                                (const uint8_t *)kPassword, strlen(kPassword),
                                kDevice, &writer),
              CF_BACKUP_INVALID);
    check_int("malformed header write callbacks", writer.calls, 0);
}

static void test_malformed_kdf_header(void) {
    memcpy(g_work, g_backup, g_backup_size);
    g_work[0] ^= 0x01;
    expect_header_reject(g_work, "bad magic");

    memcpy(g_work, g_backup, g_backup_size);
    wr32(g_work + 8, 2);
    expect_header_reject(g_work, "bad version");

    memcpy(g_work, g_backup, g_backup_size);
    wr32(g_work + 12, CF_FIDO_STORE_SIZE + 1u);
    expect_header_reject(g_work, "wrong store size");

    memcpy(g_work, g_backup, g_backup_size);
    wr32(g_work + 12, 0);
    expect_header_reject(g_work, "zero store size");

    memcpy(g_work, g_backup, g_backup_size);
    wr32(g_work + 16, 2048);
    expect_header_reject(g_work, "wrong chunk size");

    memcpy(g_work, g_backup, g_backup_size);
    wr32(g_work + 16, 0);
    expect_header_reject(g_work, "zero chunk size");

    memcpy(g_work, g_backup, g_backup_size);
    wr32(g_work + 20, 0);
    expect_header_reject(g_work, "zero iterations");

    memcpy(g_work, g_backup, g_backup_size);
    wr32(g_work + 20, 1);
    expect_header_reject(g_work, "tiny iteration count");

    memcpy(g_work, g_backup, g_backup_size);
    wr32(g_work + 20, 0xFFFFFFFFu);
    expect_header_reject(g_work, "maximum iteration count");

    memcpy(g_work, g_backup, g_backup_size);
    g_work[48] ^= 0x01;
    expect_header_reject(g_work, "wrong device header");
}

typedef struct {
    uint8_t data[BACKUP_SIZE + RECORD_SIZE];
    size_t capacity;
    size_t position;
    size_t limit;
    int failed;
} writer_cookie;

static ssize_t cookie_write(void *ctx, const char *buffer, size_t size) {
    writer_cookie *cookie = ctx;
    if (cookie->position >= cookie->limit) {
        cookie->failed = 1;
        return -1;
    }
    size_t count = size;
    if (count > cookie->limit - cookie->position) {
        count = cookie->limit - cookie->position;
        cookie->failed = 1;
    }
    if (count > cookie->capacity - cookie->position) {
        count = cookie->capacity - cookie->position;
        cookie->failed = 1;
    }
    memcpy(cookie->data + cookie->position, buffer, count);
    cookie->position += count;
    return (ssize_t)count;
}

static int cookie_seek(void *ctx, off64_t *offset, int whence) {
    writer_cookie *cookie = ctx;
    off64_t target;
    if (whence == SEEK_SET) {
        target = *offset;
    } else if (whence == SEEK_CUR) {
        target = (off64_t)cookie->position + *offset;
    } else {
        return -1;
    }
    if (target < 0 || (size_t)target > cookie->capacity) {
        return -1;
    }
    cookie->position = (size_t)target;
    *offset = target;
    return 0;
}

static void test_export_injected_failures(void) {
    uint8_t scratch[BACKUP_SIZE];
    size_t written = 0;

    {
        test_rng rng = {TEST_RNG_SEED, 1, 0};
        test_reader reader = {g_store, sizeof(g_store), 0, -1};
        check_int("random failure",
                  export_mem(scratch, sizeof(scratch),
                             (const uint8_t *)kPassword, strlen(kPassword),
                             kDevice, &rng, &reader, &written),
                  CF_BACKUP_CRYPTO);
        check_int("random failure bytes", (long)written, 0);
        check_int("random failure read calls", reader.calls, 0);
    }

    {
        test_rng rng = {TEST_RNG_SEED, 0, 0};
        test_reader reader = {g_store, sizeof(g_store), 0, 1};
        check_int("first read failure",
                  export_mem(scratch, sizeof(scratch),
                             (const uint8_t *)kPassword, strlen(kPassword),
                             kDevice, &rng, &reader, &written),
                  CF_BACKUP_IO);
        check_int("first read failure header only", (long)written,
                  CF_BACKUP_HEADER);
        check_int("first read failure read calls", reader.calls, 1);
    }

    {
        writer_cookie cookie;
        memset(&cookie, 0, sizeof(cookie));
        cookie.capacity = sizeof(cookie.data);
        cookie.limit = CF_BACKUP_HEADER + 2u * RECORD_SIZE + 100u;
        cookie_io_functions_t functions;
        memset(&functions, 0, sizeof(functions));
        functions.write = cookie_write;
        functions.seek = cookie_seek;
        FILE *file = fopencookie(&cookie, "wb", functions);
        REQUIRE(file != NULL);
        test_rng rng = {TEST_RNG_SEED, 0, 0};
        test_reader reader = {g_store, sizeof(g_store), 0, -1};
        const int result = cf_backup_export(file, (const uint8_t *)kPassword,
                                            strlen(kPassword), kDevice,
                                            reader_cb, &reader, rng_cb, &rng);
        fclose(file);
        check_int("write failure", result, CF_BACKUP_IO);
        REQUIRE(cookie.failed == 1);
    }
}

static void test_restore_write_failure(void) {
    test_writer writer;
    init_writer(&writer);
    writer.fail_at = 101;
    check_int("injected restore write failure",
              restore_mem(g_backup, g_backup_size, (const uint8_t *)kPassword,
                          strlen(kPassword), kDevice, &writer),
              CF_BACKUP_IO);
    check_int("restore write callbacks", writer.calls, 101);
    REQUIRE(memcmp(writer.data, g_store, 100u * CF_BACKUP_CHUNK) == 0);

    init_writer(&writer);
    check_int("NULL restore writer",
              cf_backup_restore(NULL, (const uint8_t *)kPassword,
                                strlen(kPassword), kDevice, NULL, &writer),
              CF_BACKUP_INVALID);
    check_int("NULL restore writer callbacks", writer.calls, 0);
}

typedef struct {
    const uint8_t *first;
    size_t first_size;
    const uint8_t *replacement;
    size_t replacement_size;
    size_t position;
    int swapped;
} swap_cookie;

static ssize_t swap_read(void *ctx, char *buffer, size_t size) {
    swap_cookie *cookie = ctx;
    if (cookie->position >= cookie->first_size) {
        return 0;
    }
    size_t count = size;
    if (count > cookie->first_size - cookie->position) {
        count = cookie->first_size - cookie->position;
    }
    memcpy(buffer, cookie->first + cookie->position, count);
    cookie->position += count;
    return (ssize_t)count;
}

static int swap_seek(void *ctx, off64_t *offset, int whence) {
    swap_cookie *cookie = ctx;
    off64_t target;
    if (whence == SEEK_SET) {
        target = *offset;
    } else if (whence == SEEK_CUR) {
        target = (off64_t)cookie->position + *offset;
    } else {
        return -1;
    }
    /* The codec completes the full verification scan, then rewinds to re-read
     * the header and apply. Swap the medium exactly on that rewind. */
    if (target == 0 && cookie->position >= cookie->first_size &&
        cookie->swapped == 0) {
        cookie->first = cookie->replacement;
        cookie->first_size = cookie->replacement_size;
        cookie->swapped = 1;
    }
    if (target < 0 || (size_t)target > cookie->first_size) {
        return -1;
    }
    cookie->position = (size_t)target;
    *offset = target;
    return 0;
}

static void test_media_replacement_between_verify_and_apply(void) {
    REQUIRE(memcmp(g_backup + 24, g_alt_backup + 24, 24) != 0);

    swap_cookie cookie;
    memset(&cookie, 0, sizeof(cookie));
    cookie.first = g_backup;
    cookie.first_size = g_backup_size;
    cookie.replacement = g_alt_backup;
    cookie.replacement_size = g_alt_backup_size;
    cookie_io_functions_t functions;
    memset(&functions, 0, sizeof(functions));
    functions.read = swap_read;
    functions.seek = swap_seek;
    FILE *file = fopencookie(&cookie, "rb", functions);
    REQUIRE(file != NULL);

    test_writer writer;
    init_writer(&writer);
    const int result = cf_backup_restore(file, (const uint8_t *)kPassword,
                                         strlen(kPassword), kDevice, writer_cb,
                                         &writer);
    fclose(file);
    check_int("replacement restore", result, CF_BACKUP_INVALID);
    check_int("replacement write callbacks", writer.calls, 0);
    check_int("replacement swaps", cookie.swapped, 1);
}

static int emit_backup(const char *path) {
    fill_store(g_store, TEST_STORE_SEED);
    test_rng rng = {TEST_RNG_SEED, 0, 0};
    test_reader reader = {g_store, sizeof(g_store), 0, -1};
    size_t size = 0;
    const int result =
        export_mem(g_backup, sizeof(g_backup), (const uint8_t *)kPassword,
                   strlen(kPassword), kDevice, &rng, &reader, &size);
    if (result != CF_BACKUP_OK || size != BACKUP_SIZE) {
        fprintf(stderr, "emit export failed: %d\n", result);
        return 1;
    }
    FILE *file = fopen(path, "wb");
    if (!file) {
        perror(path);
        return 1;
    }
    const size_t written = fwrite(g_backup, 1, size, file);
    if (fclose(file) != 0 || written != size) {
        fprintf(stderr, "emit write failed\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--emit") == 0) {
        return emit_backup(argv[2]);
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--emit <path>]\n", argv[0]);
        return 2;
    }

    make_fixture();
    test_envelope_format();
    test_roundtrip_all_bytes();
    test_password_validation();
    test_wrong_password();
    test_device_binding();
    test_corruption_before_any_write();
    test_truncation_and_trailing();
    test_malformed_kdf_header();
    test_export_injected_failures();
    test_restore_write_failure();
    test_media_replacement_between_verify_and_apply();

    puts("backup_codec_test: all checks passed");
    return 0;
}
