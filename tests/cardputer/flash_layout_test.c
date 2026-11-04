#include "fake_flash.h"
#include "flash_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(int condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static cf_partition_desc partition(const char* label, uint8_t type,
                                   uint8_t subtype, uint32_t offset,
                                   uint32_t size) {
    cf_partition_desc part;
    memset(&part, 0, sizeof(part));
    part.type = type;
    part.subtype = subtype;
    part.offset = offset;
    part.size = size;
    snprintf(part.label, sizeof(part.label), "%s", label);
    return part;
}

static size_t base_table(cf_partition_desc* parts) {
    parts[0] = partition("bootloader", 0x00, 0x00, 0x000000, 0x008000);
    parts[1] = partition("partition_table", 0x01, 0x00, 0x008000, 0x001000);
    parts[2] = partition("nvs", 0x02, 0x00, 0x009000, 0x006000);
    parts[3] = partition("app0", 0x00, 0x10, 0x010000, 0x100000);
    parts[4] = partition("fido_store", 0x40, 0x01, 0x110000, 0x102000);
    return 5;
}

static cf_layout_status validate(cf_partition_desc* parts, size_t count) {
    cf_flash_layout layout;
    return cf_flash_layout_validate(parts, count, 0x400000u, &layout);
}

static void test_valid_layout(void) {
    cf_partition_desc parts[8];
    size_t count = base_table(parts);
    cf_flash_layout layout;
    require(cf_flash_layout_validate(parts, count, 0x400000u, &layout) ==
                CF_LAYOUT_OK,
            "valid layout accepted");
    require(layout.flash_size == 0x400000u, "flash size recorded");
    require(layout.store_offset == 0x110000u, "store offset recorded");

    uint32_t physical = 0;
    require(cf_flash_layout_physical(&layout, 0, 16, &physical) == 0,
            "logical zero is addressable");
    require(physical == 0x112000u, "logical zero starts after key pages");
    require(cf_flash_layout_physical(&layout, CF_FIDO_FS_SIZE - 16, 16,
                                     &physical) == 0,
            "last filesystem page is addressable");
    require(physical == 0x110000u + 0x2000u + CF_FIDO_FS_SIZE - 16,
            "last page maps exactly");
    require(cf_flash_layout_physical(&layout, CF_FIDO_FS_SIZE - 8, 16,
                                     &physical) != 0,
            "translation refuses to cross the end");
    require(cf_flash_layout_physical(&layout, CF_FIDO_FS_SIZE, 1, &physical) != 0,
            "logical end is out of range");
    require(cf_flash_layout_physical(&layout, 0, CF_FIDO_FS_SIZE + 1,
                                     &physical) != 0,
            "oversized translation is rejected");
}

static void test_invalid_layouts(void) {
    cf_partition_desc parts[CF_MAX_PARTITIONS + 1];
    size_t count = base_table(parts);

    require(validate(parts, count - 1) == CF_LAYOUT_MISSING,
            "missing fido_store rejected");

    parts[4].type = 0x41;
    require(validate(parts, count) == CF_LAYOUT_WRONG_TYPE, "wrong type rejected");
    parts[4].type = 0x40;

    parts[4].subtype = 0x02;
    require(validate(parts, count) == CF_LAYOUT_WRONG_TYPE,
            "wrong subtype rejected");
    parts[4].subtype = 0x01;

    parts[4].size = 0x101000;
    require(validate(parts, count) == CF_LAYOUT_WRONG_SIZE, "wrong size rejected");
    parts[4].size = 0x102000;

    parts[4].offset = 0x110800;
    require(validate(parts, count) == CF_LAYOUT_NOT_ALIGNED,
            "misaligned store rejected");
    parts[4].offset = 0x110000;

    parts[4].offset = 0x300000;
    require(validate(parts, count) == CF_LAYOUT_OUT_OF_RANGE,
            "store beyond flash rejected");
    parts[4].offset = 0x110000;

    cf_partition_desc reserved[2];
    reserved[0] = partition("fido_store", 0x40, 0x01, 0x0, 0x102000);
    reserved[1] = partition("app0", 0x00, 0x10, 0x200000, 0x100000);
    require(validate(reserved, 2) == CF_LAYOUT_OVERLAP,
            "store over bootloader/partition table rejected");

    parts[3].size = 0x101000;
    require(validate(parts, count) == CF_LAYOUT_OVERLAP,
            "overlap with app rejected");
    parts[3].size = 0x100000;

    parts[2].size = 0;
    require(validate(parts, count) == CF_LAYOUT_INVALID_ARGUMENT,
            "zero-length partition rejected");
    parts[2].size = 0x6000;

    parts[5] = partition("fido_store", 0x40, 0x01, 0x220000, 0x102000);
    require(validate(parts, count + 1) == CF_LAYOUT_DUPLICATE_LABEL,
            "duplicate fido_store rejected");

    parts[5] = partition("app0", 0x00, 0x10, 0x300000, 0x100000);
    require(validate(parts, count + 1) == CF_LAYOUT_DUPLICATE_LABEL,
            "duplicate label rejected");

    parts[0].offset = 0xFFFFFF00u;
    require(validate(parts, count) == CF_LAYOUT_OUT_OF_RANGE,
            "overflowing offset rejected");
    parts[0].offset = 0;

    for (size_t i = 0; i < CF_MAX_PARTITIONS + 1; ++i) {
        parts[i] = partition("filler", 0x00, 0x00,
                             (uint32_t)(0x300000 + i * 0x1000), 0x1000);
    }
    require(validate(parts, CF_MAX_PARTITIONS + 1) == CF_LAYOUT_TOO_MANY,
            "too many partitions rejected");

    cf_flash_layout layout;
    require(cf_flash_layout_validate(parts, 1, 0, &layout) ==
                CF_LAYOUT_INVALID_ARGUMENT,
            "zero flash size rejected");
}

static void test_translation_isolates_keys(void) {
    fake_flash_t flash;
    memset(&flash, 0, sizeof(flash));
    flash.size = 0x400000u;
    flash.data = malloc(flash.size);
    require(flash.data != NULL, "large fake flash");
    memset(flash.data, 0xAA, flash.size);
    flash.fail_read_at = -1;
    flash.fail_program_at = -1;
    flash.fail_erase_at = -1;

    cf_partition_desc parts[8];
    size_t count = base_table(parts);
    cf_flash_layout layout;
    require(cf_flash_layout_validate(parts, count, 0x400000u, &layout) ==
                CF_LAYOUT_OK,
            "layout for translation");

    uint32_t physical = 0;
    require(cf_flash_layout_physical(&layout, 0, 16, &physical) == 0,
            "map first filesystem bytes");
    const uint8_t zeroes[16] = {0};
    require(fake_flash_program_direct(&flash, physical, zeroes, sizeof(zeroes)) ==
                0,
            "program through translated address");
    for (uint32_t i = 0; i < CF_FIDO_FS_OFFSET; ++i) {
        require(flash.data[layout.store_offset + i] == 0xAA,
                "key pages remain untouched");
    }
    require(flash.data[physical] == 0x00, "filesystem byte was written");
    require(flash.data[layout.store_offset + CF_FIDO_KEYS_OFFSET] == 0xAA,
            "keys_offset is not the filesystem start");

    require(cf_flash_layout_physical(&layout, CF_FIDO_FS_SIZE - 16, 16,
                                     &physical) == 0,
            "map last filesystem bytes");
    require(fake_flash_program_direct(&flash, physical, zeroes, sizeof(zeroes)) ==
                0,
            "program last page");
    require(flash.data[layout.store_offset + CF_FIDO_STORE_SIZE - 1] == 0x00,
            "last byte inside the partition was written");
    require(flash.data[layout.store_offset + CF_FIDO_STORE_SIZE] == 0xAA,
            "neighbouring partition remains untouched");
    fake_flash_free(&flash);
}

int main(void) {
    test_valid_layout();
    test_invalid_layouts();
    test_translation_isolates_keys();
    puts("PASS: dedicated FIDO partition validation and offset translation");
    return 0;
}
