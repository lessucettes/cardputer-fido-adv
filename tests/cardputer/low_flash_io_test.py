"""Exercise the SDK's exact flash I/O helpers with failing ESP-IDF calls.

Only the hardware/lockout boundary is substituted. The helpers are extracted
verbatim so these host checks do not depend on ESP-IDF or its 32-bit journal
layout. The full firmware build separately checks their target integration.
"""
import os
import pathlib
import shlex
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "pico-keys-sdk" / "src" / "fs" / "low_flash.c"

PREAMBLE = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ESP_PLATFORM 1
#define PICOKEYS_OK 0
#define PICOKEYS_ERR_NO_MEMORY -1000
#define PICOKEYS_ERR_MEMORY_FATAL -1001
#define PICOKEYS_EXEC_ERROR -1006
#define ESP_OK 0
#define XIP_BASE 0
#define FLASH_SECTOR_SIZE 4096
#define CF_FIDO_FS_OFFSET 8192
#define save_and_disable_interrupts() 1
#define restore_interrupts(a) ((void)(a))
typedef int esp_err_t;
static void *part0;
static int erase_error, program_error, erase_calls, program_calls;
static int suspended;
static uint8_t storage[0x40000];
static bool multicore_lockout_start_timeout_us(int us) {
    (void)us;
    assert(!suspended);
    suspended = 1;
    return true;
}
static bool multicore_lockout_end_timeout_us(int us) {
    (void)us;
    assert(suspended);
    suspended = 0;
    return true;
}
static int esp_partition_erase_range(void *part, uintptr_t addr, size_t size) {
    (void)part;
    ++erase_calls;
    assert(addr >= CF_FIDO_FS_OFFSET);
    addr -= CF_FIDO_FS_OFFSET;
    assert(addr + size <= sizeof(storage));
    if (!erase_error) {
        memset(storage + addr, 0xFF, size);
    }
    return erase_error;
}
static int esp_partition_write(void *part, uintptr_t addr,
                               const uint8_t *data, size_t size) {
    (void)part;
    ++program_calls;
    assert(addr >= CF_FIDO_FS_OFFSET);
    addr -= CF_FIDO_FS_OFFSET;
    assert(addr + size <= sizeof(storage));
    if (!program_error) {
        for (size_t i = 0; i < size; ++i) {
            storage[addr + i] &= data[i];
        }
    }
    return program_error;
}
"""

RECOVERY_PREAMBLE = r"""
#define TOTAL_FLASH_PAGES 6
#define FLASH_PAGE_SIZE 256
#define FLASH_SECTOR(x) ((x) & -(uintptr_t)FLASH_SECTOR_SIZE)
#define PACK(...) __VA_ARGS__ __attribute__((packed))
static const uintptr_t last_base = 0x31000;
static const uintptr_t start_data_pool = 0, end_data_pool = sizeof(storage);
static uint8_t data_page[FLASH_PAGE_SIZE];
static struct { uint8_t page[FLASH_SECTOR_SIZE]; } flash_pages[TOTAL_FLASH_PAGES];
static uint8_t *flash_read(uintptr_t addr) {
    assert(addr < sizeof(storage));
    return storage + addr;
}
"""

RECOVERY_CHECKS = r"""
static void check_recovery(void) {
    memset(storage, 0xFF, sizeof(storage));
    flash_journal_t *entry = (flash_journal_t *)(storage + 0x30000);
    entry->magic = JOURNAL_MAGIC;
    entry->status = JOURNAL_STATUS_PENDING;
    entry->target_addr[0] = 0x38000;
    memset(storage + 0x2F000, 0xA5, FLASH_SECTOR_SIZE);
    program_error = -1;
    assert(low_flash_recover_journal(false) != PICOKEYS_OK);
    assert(entry->status == JOURNAL_STATUS_PENDING);
    program_error = 0;
    assert(low_flash_recover_journal(false) == PICOKEYS_OK);
    assert(entry->status == JOURNAL_STATUS_DONE);
    for (size_t i = 0; i < FLASH_SECTOR_SIZE; ++i) {
        assert(storage[0x38000 + i] == 0xA5);
    }
    puts("PASS: pending journal restores mapped flash after an I/O retry");
}
"""

CHECKS = r"""
int main(void) {
    uint8_t data[FLASH_SECTOR_SIZE] = {0};
    assert(do_flash_op_erase(0) == PICOKEYS_OK);
    assert(do_flash_op_program(0, data, sizeof(data)) == PICOKEYS_OK);
    assert(do_flash_op_erase_program(0, data, sizeof(data)) == PICOKEYS_OK);
    erase_error = -1;
    assert(do_flash_op_erase(0) == PICOKEYS_ERR_NO_MEMORY);
    int previous_program_calls = program_calls;
    assert(do_flash_op_erase_program(0, data, sizeof(data)) == PICOKEYS_ERR_NO_MEMORY);
    assert(program_calls == previous_program_calls);
    erase_error = 0;
    program_error = -1;
    assert(do_flash_op_program(0, data, sizeof(data)) == PICOKEYS_ERR_NO_MEMORY);
    assert(do_flash_op_erase_program(0, data, sizeof(data)) == PICOKEYS_ERR_NO_MEMORY);
    assert(!suspended);
    program_error = 0;
    assert(do_flash_op_erase_program(0, data, sizeof(data)) == PICOKEYS_OK);
    puts("PASS: failed flash I/O is retryable and releases the lockout");
    check_recovery();
    return 0;
}
"""


class LowFlashIoTest(unittest.TestCase):
    def test_failed_io_is_not_reported_as_committed(self):
        source = SOURCE.read_text(encoding="utf-8")
        # Use the actual ADV partition offset macros, not test replacements.
        macros = source[source.index("   #define flash_range_erase"):
                        source.index("  #else\n   #define flash_range_erase")]
        helpers = source[source.index("static int do_flash_op_erase("):
                         source.index("static int journal_mark_done(")]
        journal_type = source[source.index("PACK(\ntypedef struct {"):
                              source.index("static uint16_t journal_slot")]
        journal_constants = source[source.index("#define JOURNAL_MAGIC"):
                                   source.index("static bool journal_pending")]
        recovery = source[source.index("static bool journal_entry_is_erased("):
                          source.index("int low_flash_first_init(")]
        # Include the opening guard paired with the validator section's #endif.
        recovery = "#if defined(ESP_PLATFORM)\n" + recovery
        with tempfile.TemporaryDirectory(prefix="cardputer-flash-io-") as tmp:
            path = pathlib.Path(tmp)
            probe = path / "probe.c"
            binary = path / "probe"
            probe.write_text(PREAMBLE + RECOVERY_PREAMBLE + journal_type +
                             journal_constants + macros + helpers + recovery +
                             RECOVERY_CHECKS + CHECKS,
                             encoding="utf-8")
            compiler = shlex.split(os.environ.get("CC", "cc"))
            build = subprocess.run(
                compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(probe), "-o", str(binary)],
                capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(binary)], capture_output=True,
                                    text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
