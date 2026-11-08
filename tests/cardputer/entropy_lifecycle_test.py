"""Check the real startup/provisioning helpers against a recording RNG.

The ESP-IDF entropy switch and storage boundary are substituted; this checks
source lifecycle, not the statistical quality of hardware random numbers.
"""
import os
import pathlib
import shlex
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]

PREAMBLE = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "key_store.h"
#include "flash_layout.h"
#define ESP_PLATFORM 1
static bool entropy_enabled;
static int initialized, store_status;
static const void *fido_partition;
static cf_device_keys device_keys;
static cf_key_status load_status, provision_status;
static unsigned provision_calls;
void bootloader_random_enable(void) { entropy_enabled = true; }
void bootloader_random_disable(void) { entropy_enabled = false; }
static void esp_fill_random(void *out, size_t len) {
    assert(entropy_enabled && "random output requires an active entropy source");
    memset(out, 0x42, len);
}
static const void *esp_partition_find_first(int type, int subtype,
                                           const char *label) {
    (void)type; (void)subtype;
    return label;
}
static int validate_partition_table(void) { return 0; }
#define store_read NULL
#define store_program NULL
#define store_erase NULL
static int database_erased(void *ctx) { (void)ctx; return 1; }
cf_key_status cf_key_store_load(const cf_key_flash *flash, cf_device_keys *out) {
    (void)flash; (void)out;
    return load_status;
}
cf_key_status cf_key_store_provision(const cf_key_flash *flash,
                                    cf_device_keys *out) {
    ++provision_calls;
    assert(flash->random(flash->ctx, out->key1, sizeof(out->key1)) == 0);
    return provision_status;
}
"""

CHECKS = r"""
int main(void) {
    const cf_key_status states[] = {CF_KEY_EMPTY, CF_KEY_READY, CF_KEY_CORRUPT};
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
        for (int fail = 0; fail < 2; ++fail) {
            initialized = 0;
            entropy_enabled = false;
            provision_calls = 0;
            load_status = states[i];
            provision_status = fail ? CF_KEY_IO_ERROR : CF_KEY_READY;
            /* main() starts the RNG before initializing the OTP/key store. */
            hwrng_start();
            int status = cardputer_key_store_init();
            bool ready = load_status == CF_KEY_READY ||
                         (load_status == CF_KEY_EMPTY && !fail);
            assert((status == 0) == ready);
            assert(provision_calls == (load_status == CF_KEY_EMPTY ? 1u : 0u));
            uint8_t next_random[32];
            esp_fill_random(next_random, sizeof(next_random));
            assert(cardputer_key_store_init() == status);
            assert(provision_calls == (load_status == CF_KEY_EMPTY ? 1u : 0u));
        }
    }
    return 0;
}
"""


class EntropyLifecycleTest(unittest.TestCase):
    def test_key_store_preserves_running_entropy_source(self):
        startup = (ROOT / "pico-keys-sdk/src/main.c").read_text()
        self.assertLess(startup.index("    random_init();"),
                        startup.index("    otp_init();"))
        rng = (ROOT / "pico-keys-sdk/src/rng/hwrng.c").read_text()
        start = rng[rng.index("static void hwrng_start(void)"):
                    rng.index("static uint64_t random_word")]
        store = (ROOT / "src/cardputer/key_store_esp32.c").read_text()
        random_bytes = store[store.index("static int random_bytes("):
                             store.index("static int validate_partition_table(")]
        init = store[store.index("int cardputer_key_store_init(void)"):
                     store.index("void otp_platform_init(")]
        with tempfile.TemporaryDirectory(prefix="cardputer-entropy-") as tmp:
            probe = pathlib.Path(tmp) / "probe.c"
            binary = pathlib.Path(tmp) / "probe"
            probe.write_text(PREAMBLE + start + random_bytes + init + CHECKS)
            build = subprocess.run(
                shlex.split(os.environ.get("CC", "cc")) +
                ["-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "src/cardputer"), str(probe), "-o", str(binary)],
                capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(binary)], capture_output=True,
                                    text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
