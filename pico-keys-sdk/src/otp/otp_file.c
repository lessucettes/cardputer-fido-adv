/*
 * Host-emulation OTP provider with an optional plain file.
 *
 * This file exists only for isolated protocol tests. It is never compiled into
 * the ADV firmware. Set PICO_KEYS_OTP_FILE to a temporary path to keep the two
 * device secrets stable across emulator restarts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "otp_platform.h"
#include "picokeys.h"
#include "random.h"

static uint8_t otp_keys[64];
static int keys_loaded = 0;

static void load_or_create(void) {
    if (keys_loaded) {
        return;
    }
    keys_loaded = 1;
    memset(otp_keys, 0, sizeof(otp_keys));
    const char *path = getenv("PICO_KEYS_OTP_FILE");
    if (path != NULL && path[0] != '\0') {
        FILE *file = fopen(path, "rb");
        if (file != NULL) {
            const size_t read = fread(otp_keys, 1, sizeof(otp_keys), file);
            fclose(file);
            if (read == sizeof(otp_keys)) {
                return;
            }
        }
        random_fill_buffer(BYTE_ARRAY(otp_keys, sizeof(otp_keys)));
        file = fopen(path, "wb");
        if (file != NULL) {
            (void)fwrite(otp_keys, 1, sizeof(otp_keys), file);
            fclose(file);
        }
        return;
    }
    memset(otp_keys, 0xAC, 32);
    memset(otp_keys + 32, 0xBE, 32);
}

int otp_platform_enable_secure_boot(uint8_t bootkey, bool secure_lock) {
    (void)bootkey;
    (void)secure_lock;
    return -1;
}

bool otp_platform_is_secure_boot_enabled(uint8_t *bootkey) {
    (void)bootkey;
    return false;
}

bool otp_platform_is_secure_boot_locked(void) {
    return false;
}

void otp_platform_init(const uint8_t **otp_key_1_out,
                       const uint8_t **otp_key_2_out) {
    load_or_create();
    if (otp_key_1_out != NULL) {
        *otp_key_1_out = otp_keys;
    }
    if (otp_key_2_out != NULL) {
        *otp_key_2_out = otp_keys + 32;
    }
}
