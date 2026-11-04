#include "fake_flash.h"
#include "key_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t kOrder[32] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE,
    0xBA, 0xAE, 0xDC, 0xE6, 0xAF, 0x48, 0xA0, 0x3B,
    0xBF, 0xD2, 0x5E, 0x8C, 0xD0, 0x36, 0x41, 0x41
};

static void require(int condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static void put_u32_le(uint8_t* out, uint32_t value) {
    out[0] = (uint8_t)(value & 0xFF);
    out[1] = (uint8_t)((value >> 8) & 0xFF);
    out[2] = (uint8_t)((value >> 16) & 0xFF);
    out[3] = (uint8_t)((value >> 24) & 0xFF);
}

static void refresh_crc(uint8_t* record) {
    put_u32_le(record + 80, cf_key_crc32(record, 80));
}

static int keys_equal(const cf_device_keys* a, const cf_device_keys* b) {
    return memcmp(a->key1, b->key1, 32) == 0 &&
           memcmp(a->key2, b->key2, 32) == 0;
}

static int keys_are_zero(const cf_device_keys* keys) {
    for (size_t i = 0; i < sizeof(*keys); ++i) {
        if (((const uint8_t*)keys)[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static void make_keys(cf_device_keys* keys, uint8_t seed) {
    memset(keys, 0, sizeof(*keys));
    for (size_t i = 0; i < 32; ++i) {
        keys->key1[i] = (uint8_t)(seed + i + 1);
        keys->key2[i] = (uint8_t)(seed + 0x40 + i);
    }
    /* Keep key 2 below the secp256k1 order. */
    keys->key2[0] = 0x10;
}

static int write_page(fake_flash_t* flash, uint32_t offset,
                      const cf_device_keys* keys) {
    uint8_t record[CF_KEY_PAGE_SIZE];
    cf_key_record_encode(keys, record);
    if (fake_flash_ops(flash).erase(flash, offset, CF_KEY_PAGE_SIZE) != 0) {
        return -1;
    }
    return fake_flash_program_direct(flash, offset, record, CF_KEY_PAGE_SIZE);
}

static void test_crc(void) {
    const uint8_t vector[] = "123456789";
    require(cf_key_crc32(vector, 9) == 0xCBF43926u, "CRC32/ISO-HDLC vector");
}

static void test_record_codec(void) {
    cf_device_keys keys;
    make_keys(&keys, 1);
    uint8_t record[CF_KEY_PAGE_SIZE];
    cf_key_record_encode(&keys, record);
    require(memcmp(record, "CFKEY001", 8) == 0, "magic");
    require(record[8] == 1 && record[9] == 0, "version little endian");
    require(record[12] == 64 && record[13] == 0, "payload length");
    require(record[84] == 0x31 && record[85] == 0x4B && record[86] == 0x46 &&
                record[87] == 0x43,
            "commit marker");
    require(fake_flash_is_erased((const fake_flash_t*)&(fake_flash_t){
                .data = record, .size = sizeof(record)},
                CF_KEY_RECORD_SIZE, CF_KEY_PAGE_SIZE - CF_KEY_RECORD_SIZE),
            "record tail is erased");

    cf_device_keys decoded;
    require(cf_key_record_decode(record, &decoded) == CF_KEY_READY,
            "valid record decodes");
    require(keys_equal(&keys, &decoded), "decoded keys match");

    uint8_t bad[CF_KEY_PAGE_SIZE];
    memcpy(bad, record, sizeof(bad));
    bad[0] ^= 0xFF;
    require(cf_key_record_decode(bad, &decoded) == CF_KEY_CORRUPT, "bad magic");

    memcpy(bad, record, sizeof(bad));
    bad[9] = 2;
    require(cf_key_record_decode(bad, &decoded) == CF_KEY_CORRUPT, "bad version");

    memcpy(bad, record, sizeof(bad));
    bad[12] = 63;
    require(cf_key_record_decode(bad, &decoded) == CF_KEY_CORRUPT, "bad length");

    memcpy(bad, record, sizeof(bad));
    bad[16] ^= 0x01;
    require(cf_key_record_decode(bad, &decoded) == CF_KEY_CORRUPT, "bad CRC");

    memcpy(bad, record, sizeof(bad));
    memset(bad + 84, 0, 4);
    require(cf_key_record_decode(bad, &decoded) == CF_KEY_CORRUPT, "no marker");

    memcpy(bad, record, sizeof(bad));
    memset(bad + 16, 0, 32);
    refresh_crc(bad);
    require(cf_key_record_decode(bad, &decoded) == CF_KEY_CORRUPT,
            "zero key 1 rejected");

    memcpy(bad, record, sizeof(bad));
    memset(bad + 16, 0xFF, 32);
    refresh_crc(bad);
    require(cf_key_record_decode(bad, &decoded) == CF_KEY_CORRUPT,
            "all-FF key 1 rejected");

    memcpy(bad, record, sizeof(bad));
    memset(bad + 48, 0, 32);
    refresh_crc(bad);
    require(cf_key_record_decode(bad, &decoded) == CF_KEY_CORRUPT,
            "zero key 2 rejected");

    memcpy(bad, record, sizeof(bad));
    memcpy(bad + 48, kOrder, 32);
    refresh_crc(bad);
    require(cf_key_record_decode(bad, &decoded) == CF_KEY_CORRUPT,
            "group order is not a private scalar");

    memcpy(bad, record, sizeof(bad));
    memcpy(bad + 48, kOrder, 32);
    bad[79] -= 1; /* order - 1 is the largest valid scalar */
    refresh_crc(bad);
    require(cf_key_record_decode(bad, &decoded) == CF_KEY_READY,
            "order - 1 is valid");

    memset(&decoded, 0xAA, sizeof(decoded));
    require(cf_key_record_decode(bad, &decoded) == CF_KEY_READY,
            "valid record after failures");
}

static void test_empty_and_provision(void) {
    fake_flash_t flash;
    require(fake_flash_init(&flash) == 0, "fake flash init");
    cf_key_flash ops = fake_flash_ops(&flash);
    cf_device_keys keys;
    memset(&keys, 0xAA, sizeof(keys));

    require(cf_key_store_load(&ops, &keys) == CF_KEY_EMPTY,
            "erased pages and database are empty");
    require(keys_are_zero(&keys), "empty load publishes no keys");

    fake_flash_reset_ops(&flash);
    require(cf_key_store_provision(&ops, &keys) == CF_KEY_READY,
            "first provision succeeds");
    require(!keys_are_zero(&keys), "provision publishes keys");
    require(flash.program_calls == 4 && flash.erase_calls == 2,
            "provision writes payload and marker on both pages");

    cf_device_keys loaded;
    require(cf_key_store_load(&ops, &loaded) == CF_KEY_READY,
            "provisioned identity loads");
    require(keys_equal(&keys, &loaded), "loaded identity is stable");

    require(cf_key_store_provision(&ops, &loaded) == CF_KEY_READY,
            "second provision returns the existing identity");
    require(keys_equal(&keys, &loaded), "existing identity is not replaced");

    flash.fail_program_at = -1;
    flash.fail_erase_at = -1;
    fake_flash_reset_ops(&flash);
    require(cf_key_store_load(&ops, &loaded) == CF_KEY_READY,
            "load does not need writes");
    require(flash.program_calls == 0 && flash.erase_calls == 0,
            "load performs no writes");
    fake_flash_free(&flash);
}

static void test_recovery_policy(void) {
    fake_flash_t flash;
    require(fake_flash_init(&flash) == 0, "fake flash init");
    cf_key_flash ops = fake_flash_ops(&flash);
    cf_device_keys keys;
    require(cf_key_store_provision(&ops, &keys) == CF_KEY_READY,
            "provision for recovery tests");

    cf_device_keys loaded;
    require(fake_flash_ops(&flash).erase(&flash, CF_KEY_PAGE_SIZE,
                                         CF_KEY_PAGE_SIZE) == 0,
            "erase mirror page");
    require(cf_key_store_load(&ops, &loaded) == CF_KEY_READY,
            "one valid record is enough");
    require(keys_equal(&keys, &loaded), "single record identity");

    cf_device_keys other;
    make_keys(&other, 9);
    require(write_page(&flash, CF_KEY_PAGE_SIZE, &other) == 0,
            "write conflicting mirror");
    require(cf_key_store_load(&ops, &loaded) == CF_KEY_CORRUPT,
            "two different valid records are corrupt");

    fake_flash_free(&flash);
}

static void test_corrupt_without_regeneration(void) {
    fake_flash_t flash;
    require(fake_flash_init(&flash) == 0, "fake flash init");
    fake_flash_fill_database(&flash, 0x00);
    cf_key_flash ops = fake_flash_ops(&flash);
    cf_device_keys keys;

    require(cf_key_store_load(&ops, &keys) == CF_KEY_CORRUPT,
            "nonempty database without keys is corrupt");

    fake_flash_reset_ops(&flash);
    require(cf_key_store_provision(&ops, &keys) == CF_KEY_CORRUPT,
            "provision refuses to overwrite a nonempty database");
    require(flash.program_calls == 0 && flash.erase_calls == 0,
            "refused provisioning performs no writes");
    require(keys_are_zero(&keys), "refused provisioning publishes nothing");
    fake_flash_free(&flash);
}

static void test_interrupted_provisioning(void) {
    for (int fail_at = 1; fail_at <= 4; ++fail_at) {
        fake_flash_t flash;
        require(fake_flash_init(&flash) == 0, "fake flash init");
        flash.fail_program_at = fail_at;
        cf_key_flash ops = fake_flash_ops(&flash);
        cf_device_keys keys;
        cf_key_status status = cf_key_store_provision(&ops, &keys);
        if (status == CF_KEY_READY) {
            cf_device_keys reloaded;
            require(cf_key_store_load(&ops, &reloaded) == CF_KEY_READY,
                    "published keys are durable");
            require(keys_equal(&keys, &reloaded), "published keys reload");
        } else {
            require(keys_are_zero(&keys), "failed provisioning publishes nothing");
            if (fail_at == 1 || fail_at == 2) {
                cf_device_keys reloaded;
                cf_key_status after = cf_key_store_load(&ops, &reloaded);
                require(after == CF_KEY_CORRUPT || after == CF_KEY_EMPTY,
                        "uncommitted first page is never READY");
                require(keys_are_zero(&reloaded),
                        "uncommitted first page publishes no identity");
            }
        }
        fake_flash_free(&flash);
    }

    fake_flash_t flash;
    require(fake_flash_init(&flash) == 0, "fake flash init");
    flash.fail_erase_at = 1;
    cf_key_flash ops = fake_flash_ops(&flash);
    cf_device_keys keys;
    require(cf_key_store_provision(&ops, &keys) == CF_KEY_IO_ERROR,
            "erase failure is an I/O error");
    require(keys_are_zero(&keys), "erase failure publishes nothing");
    fake_flash_free(&flash);
}

static void test_generation_failures(void) {
    fake_flash_t flash;
    require(fake_flash_init(&flash) == 0, "fake flash init");
    flash.fail_random = 1;
    cf_key_flash ops = fake_flash_ops(&flash);
    cf_device_keys keys;
    require(cf_key_store_provision(&ops, &keys) == CF_KEY_IO_ERROR,
            "RNG failure fails closed");
    require(flash.program_calls == 0 && flash.erase_calls == 0,
            "RNG failure writes nothing");
    require(keys_are_zero(&keys), "RNG failure publishes nothing");
    fake_flash_free(&flash);

    require(fake_flash_init(&flash) == 0, "fake flash init");
    flash.random_zero_key2 = 1;
    ops = fake_flash_ops(&flash);
    require(cf_key_store_provision(&ops, &keys) == CF_KEY_IO_ERROR,
            "invalid key 2 scalar fails closed");
    require(flash.program_calls == 0 && flash.erase_calls == 0,
            "invalid scalar writes nothing");
    fake_flash_free(&flash);
}

static void test_read_failure(void) {
    fake_flash_t flash;
    require(fake_flash_init(&flash) == 0, "fake flash init");
    cf_key_flash ops = fake_flash_ops(&flash);
    cf_device_keys keys;
    flash.fail_read_at = 1;
    require(cf_key_store_load(&ops, &keys) == CF_KEY_IO_ERROR,
            "read failure is an I/O error");
    require(keys_are_zero(&keys), "read failure publishes nothing");
    fake_flash_free(&flash);
}

int main(void) {
    test_crc();
    test_record_codec();
    test_empty_and_provision();
    test_recovery_policy();
    test_corrupt_without_regeneration();
    test_interrupted_provisioning();
    test_generation_failures();
    test_read_failure();
    puts("PASS: committed flash key records and recovery policy");
    return 0;
}
