/*
 * Test-only adapter around the production encrypted-backup codec and apply
 * engine. It lets the protocol test move an emulator fido_store image through
 * the real cf_backup_export()/cf_backup_restore() path and then inspect the
 * restored key pages with the production key-store decoder.
 *
 * The file-backed NOR model and the buffer staging are test harness code.
 * They are not the ESP partition/flash driver. See backup_protocol_test.py for
 * the emulator filesystem/OTP bridge and its stated coverage limits.
 *
 * Usage:
 *   backup_protocol_tool pack    STORE OTP FS
 *   backup_protocol_tool extract STORE OTP FS
 *   backup_protocol_tool export  STORE BACKUP DEVICE_HEX
 *   backup_protocol_tool restore BACKUP STORE DEVICE_HEX
 *   backup_protocol_tool verify  BACKUP DEVICE_HEX
 *
 * The password is read from CF_BACKUP_PROTOCOL_PASSWORD and is never printed.
 * DEVICE_HEX is the public 32-byte device binding (64 hex characters).
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include "backup_codec.h"
#include "backup_store.h"
#include "flash_layout.h"
#include "key_store.h"
#include "mbedtls/platform_util.h"

#define IMAGE_SIZE CF_FIDO_STORE_SIZE
#define FS_SIZE CF_FIDO_FS_SIZE
#define OTP_SIZE 64u

typedef struct {
    uint8_t *data;
    size_t size;
} nor_image;

static void fail(const char *message) {
    fprintf(stderr, "backup_protocol_tool: %s\n", message);
    exit(1);
}

static const char *password_from_env(void) {
    const char *password = getenv("CF_BACKUP_PROTOCOL_PASSWORD");
    if (password == NULL || password[0] == '\0') {
        fail("CF_BACKUP_PROTOCOL_PASSWORD is required");
    }
    if (strlen(password) > CF_BACKUP_PASSWORD_MAX) {
        fail("password is too long");
    }
    return password;
}

static void parse_device(const char *text, uint8_t device[32]) {
    if (text == NULL || strlen(text) != 64u) {
        fail("device binding must be exactly 64 hex characters");
    }
    for (size_t i = 0; i < 32; ++i) {
        unsigned value = 0;
        for (int half = 0; half < 2; ++half) {
            const char c = text[i * 2u + (size_t)half];
            unsigned nibble;
            if (c >= '0' && c <= '9') {
                nibble = (unsigned)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                nibble = (unsigned)(c - 'a') + 10u;
            } else if (c >= 'A' && c <= 'F') {
                nibble = (unsigned)(c - 'A') + 10u;
            } else {
                fail("device binding is not hexadecimal");
            }
            value = (value << 4) | nibble;
        }
        device[i] = (uint8_t)value;
    }
}

static uint8_t *read_exact(const char *path, size_t expected) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fail("cannot open input file");
    }
    uint8_t *buffer = malloc(expected);
    if (buffer == NULL) {
        fclose(file);
        fail("out of memory");
    }
    if (fread(buffer, 1, expected, file) != expected || fgetc(file) != EOF) {
        fclose(file);
        free(buffer);
        fail("input file has the wrong size");
    }
    if (fclose(file) != 0) {
        free(buffer);
        fail("cannot close input file");
    }
    return buffer;
}

static uint8_t *load_store(const char *path) {
    return read_exact(path, IMAGE_SIZE);
}

static void save_exact(const char *path, const uint8_t *data, size_t length) {
    char temporary[4096];
    if (strlen(path) + 5u >= sizeof(temporary)) {
        fail("output path is too long");
    }
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    const int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fail("cannot create output file");
    }
    FILE *file = fdopen(fd, "wb");
    if (file == NULL) {
        close(fd);
        unlink(temporary);
        fail("cannot open output stream");
    }
    const int written = fwrite(data, 1, length, file) == length;
    if (written && fflush(file) != 0) {
        fclose(file);
        unlink(temporary);
        fail("cannot flush output file");
    }
    if (fsync(fd) != 0 || fclose(file) != 0 || !written) {
        unlink(temporary);
        fail("cannot write output file");
    }
    if (rename(temporary, path) != 0) {
        unlink(temporary);
        fail("cannot publish output file");
    }
}

static int nor_read(void *ctx, uint32_t offset, uint8_t *out, size_t len) {
    nor_image *image = ctx;
    if (out == NULL || (size_t)offset > image->size || len > image->size - offset) {
        return -1;
    }
    memcpy(out, image->data + offset, len);
    return 0;
}

static int nor_program(void *ctx, uint32_t offset, const uint8_t *data, size_t len) {
    nor_image *image = ctx;
    if (data == NULL || (size_t)offset > image->size || len > image->size - offset) {
        return -1;
    }
    for (size_t i = 0; i < len; ++i) {
        if ((image->data[offset + i] & data[i]) != data[i]) {
            return -1; /* NOR programming can only clear bits. */
        }
    }
    for (size_t i = 0; i < len; ++i) {
        image->data[offset + i] &= data[i];
    }
    return 0;
}

static int nor_erase(void *ctx, uint32_t offset, size_t len) {
    nor_image *image = ctx;
    if ((size_t)offset > image->size || len > image->size - offset) {
        return -1;
    }
    memset(image->data + offset, 0xFF, len);
    return 0;
}

static int nor_database_erased(void *ctx) {
    nor_image *image = ctx;
    for (size_t i = CF_FIDO_FS_OFFSET; i < image->size; ++i) {
        if (image->data[i] != 0xFF) {
            return 0;
        }
    }
    return 1;
}

static cf_key_flash key_flash(nor_image *image) {
    cf_key_flash flash;
    memset(&flash, 0, sizeof(flash));
    flash.ctx = image;
    flash.read = nor_read;
    flash.program = nor_program;
    flash.erase = nor_erase;
    flash.database_erased = nor_database_erased;
    return flash;
}

static int store_read(void *ctx, uint32_t offset, uint8_t *out, size_t len) {
    return nor_read(ctx, offset, out, len);
}

static int random_bytes(void *ctx, uint8_t *out, size_t len) {
    (void)ctx;
    size_t done = 0;
    while (done < len) {
        const ssize_t count = getrandom(out + done, len - done, 0);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        done += (size_t)count;
    }
    return 0;
}

static int command_pack(const char *store_path, const char *otp_path,
                        const char *fs_path) {
    uint8_t *otp = read_exact(otp_path, OTP_SIZE);
    uint8_t *fs = read_exact(fs_path, FS_SIZE);
    uint8_t *image = malloc(IMAGE_SIZE);
    if (image == NULL) {
        free(otp);
        free(fs);
        fail("out of memory");
    }
    memset(image, 0xFF, IMAGE_SIZE);
    cf_device_keys keys;
    memcpy(keys.key1, otp, 32);
    memcpy(keys.key2, otp + 32, 32);
    uint8_t page[CF_KEY_PAGE_SIZE];
    cf_key_record_encode(&keys, page);
    cf_device_keys decoded_a;
    cf_device_keys decoded_b;
    const int ok =
        cf_key_record_decode(page, &decoded_a) == CF_KEY_READY &&
        cf_key_record_decode(page, &decoded_b) == CF_KEY_READY &&
        memcmp(&decoded_a, &keys, sizeof(keys)) == 0 &&
        memcmp(&decoded_b, &keys, sizeof(keys)) == 0;
    memcpy(image, page, sizeof(page));
    memcpy(image + CF_KEY_PAGE_SIZE, page, sizeof(page));
    memcpy(image + CF_FIDO_FS_OFFSET, fs, FS_SIZE);
    mbedtls_platform_zeroize(&keys, sizeof(keys));
    mbedtls_platform_zeroize(&decoded_a, sizeof(decoded_a));
    mbedtls_platform_zeroize(&decoded_b, sizeof(decoded_b));
    mbedtls_platform_zeroize(page, sizeof(page));
    free(otp);
    free(fs);
    if (!ok) {
        free(image);
        fail("production key record did not round-trip; check the OTP scalar");
    }
    save_exact(store_path, image, IMAGE_SIZE);
    free(image);
    puts("pack: production key records and filesystem window staged");
    return 0;
}

static int command_extract(const char *store_path, const char *otp_path,
                           const char *fs_path) {
    uint8_t *image = load_store(store_path);
    nor_image store = {image, IMAGE_SIZE};
    cf_key_flash flash = key_flash(&store);
    cf_device_keys keys;
    const cf_key_status status = cf_key_store_load(&flash, &keys);
    if (status != CF_KEY_READY) {
        mbedtls_platform_zeroize(&keys, sizeof(keys));
        mbedtls_platform_zeroize(image, IMAGE_SIZE);
        free(image);
        fail("restored key pages are not READY");
    }
    uint8_t otp[OTP_SIZE];
    memcpy(otp, keys.key1, 32);
    memcpy(otp + 32, keys.key2, 32);
    save_exact(otp_path, otp, sizeof(otp));
    save_exact(fs_path, image + CF_FIDO_FS_OFFSET, FS_SIZE);
    mbedtls_platform_zeroize(&keys, sizeof(keys));
    mbedtls_platform_zeroize(otp, sizeof(otp));
    mbedtls_platform_zeroize(image, IMAGE_SIZE);
    free(image);
    puts("extract: key pages decoded READY and filesystem window extracted");
    return 0;
}

static int command_export(const char *store_path, const char *backup_path,
                          const char *device_text) {
    uint8_t device[32];
    parse_device(device_text, device);
    uint8_t *image = load_store(store_path);
    nor_image store = {image, IMAGE_SIZE};
    const char *password = password_from_env();
    const int fd = open(backup_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        mbedtls_platform_zeroize(image, IMAGE_SIZE);
        free(image);
        fail("cannot create backup file");
    }
    FILE *file = fdopen(fd, "wb");
    if (file == NULL) {
        close(fd);
        mbedtls_platform_zeroize(image, IMAGE_SIZE);
        free(image);
        fail("cannot open backup stream");
    }
    int result = cf_backup_export(file, (const uint8_t *)password,
                                  strlen(password), device, store_read, &store,
                                  random_bytes, NULL);
    if (result == CF_BACKUP_OK && fflush(file) != 0) {
        result = CF_BACKUP_IO;
    }
    if (result == CF_BACKUP_OK && fsync(fd) != 0) {
        result = CF_BACKUP_IO;
    }
    if (fclose(file) != 0) {
        result = CF_BACKUP_IO;
    }
    mbedtls_platform_zeroize(image, IMAGE_SIZE);
    free(image);
    if (result != CF_BACKUP_OK) {
        unlink(backup_path);
        fail("cf_backup_export failed");
    }
    printf("export: codec status %d\n", result);
    return 0;
}

static uint8_t *load_or_create_store(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        if (errno != ENOENT) {
            fail("cannot open target store");
        }
        uint8_t *image = malloc(IMAGE_SIZE);
        if (image == NULL) {
            fail("out of memory");
        }
        memset(image, 0xFF, IMAGE_SIZE);
        return image;
    }
    fclose(file);
    return load_store(path);
}

static int command_restore(const char *backup_path, const char *store_path,
                           const char *device_text) {
    uint8_t device[32];
    parse_device(device_text, device);
    uint8_t *image = load_or_create_store(store_path);
    nor_image target = {image, IMAGE_SIZE};
    const char *password = password_from_env();
    FILE *file = fopen(backup_path, "rb");
    if (file == NULL) {
        mbedtls_platform_zeroize(image, IMAGE_SIZE);
        free(image);
        fail("cannot open backup file");
    }
    cf_backup_store store;
    memset(&store, 0, sizeof(store));
    store.ctx = &target;
    store.read = nor_read;
    store.program = nor_program;
    store.erase = nor_erase;
    int result = cf_backup_restore(file, (const uint8_t *)password,
                                   strlen(password), device,
                                   cf_backup_store_write, &store);
    if (fclose(file) != 0) {
        result = CF_BACKUP_IO;
    }
    if (result == CF_BACKUP_OK && cf_backup_store_finish(&store) != 0) {
        result = CF_BACKUP_IO;
    }
    mbedtls_platform_zeroize(&store, sizeof(store));
    if (result != CF_BACKUP_OK) {
        mbedtls_platform_zeroize(image, IMAGE_SIZE);
        free(image);
        fail("cf_backup_restore failed before publishing the store");
    }
    save_exact(store_path, image, IMAGE_SIZE);
    mbedtls_platform_zeroize(image, IMAGE_SIZE);
    free(image);
    printf("restore: codec/store status %d\n", result);
    return 0;
}

static int command_verify(const char *backup_path, const char *device_text) {
    uint8_t device[32];
    parse_device(device_text, device);
    const char *password = password_from_env();
    FILE *file = fopen(backup_path, "rb");
    if (file == NULL) {
        fail("cannot open backup file");
    }
    const int result = cf_backup_verify(file, (const uint8_t *)password,
                                        strlen(password), device);
    if (fclose(file) != 0) {
        fail("cannot close backup file");
    }
    if (result != CF_BACKUP_OK) {
        fail("cf_backup_verify rejected the backup");
    }
    printf("verify: codec status %d\n", result);
    return 0;
}

static void usage(void) {
    fputs("usage:\n"
          "  backup_protocol_tool pack STORE OTP FS\n"
          "  backup_protocol_tool extract STORE OTP FS\n"
          "  backup_protocol_tool export STORE BACKUP DEVICE_HEX\n"
          "  backup_protocol_tool restore BACKUP STORE DEVICE_HEX\n"
          "  backup_protocol_tool verify BACKUP DEVICE_HEX\n",
          stderr);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    if (strcmp(argv[1], "pack") == 0 && argc == 5) {
        return command_pack(argv[2], argv[3], argv[4]);
    }
    if (strcmp(argv[1], "extract") == 0 && argc == 5) {
        return command_extract(argv[2], argv[3], argv[4]);
    }
    if (strcmp(argv[1], "export") == 0 && argc == 5) {
        return command_export(argv[2], argv[3], argv[4]);
    }
    if (strcmp(argv[1], "restore") == 0 && argc == 5) {
        return command_restore(argv[2], argv[3], argv[4]);
    }
    if (strcmp(argv[1], "verify") == 0 && argc == 4) {
        return command_verify(argv[2], argv[3]);
    }
    usage();
    return 2;
}
