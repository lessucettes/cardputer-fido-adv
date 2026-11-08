"""Exercise credential length gates using exact production function bodies.

Crypto is a recording boundary: truncated envelopes must never reach it.
This catches size_t underflow on a 64-bit host without relying on mbedTLS's
64-bit-only oversized-message rejection to mask the ESP32 defect.
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
#define RP_ID_HASH_LEN 32
static unsigned crypto_calls;
typedef int mbedtls_chachapoly_context;
static void mbedtls_chachapoly_init(mbedtls_chachapoly_context *c) { *c = 0; }
static void mbedtls_chachapoly_free(mbedtls_chachapoly_context *c) { (void)c; }
static int mbedtls_chachapoly_setkey(mbedtls_chachapoly_context *c,
                                    const uint8_t *key) {
    (void)c; (void)key; return 0;
}
static int credential_derive_chacha_key(uint8_t *key, const uint8_t *id) {
    (void)id; memset(key, 0, 32); return 0;
}
static int mbedtls_chachapoly_auth_decrypt(mbedtls_chachapoly_context *c,
        size_t len, const uint8_t *iv, const uint8_t *aad, size_t aad_len,
        const uint8_t *tag, const uint8_t *in, uint8_t *out) {
    (void)c; (void)iv; (void)aad; (void)aad_len; (void)tag; (void)in; (void)out;
    assert(len <= 1 && "credential payload length underflowed");
    ++crypto_calls;
    return -1;
}
static int credential_silent_tag(const uint8_t *id, size_t len,
                                  const uint8_t *rp, uint8_t *out) {
    (void)id; (void)len; (void)rp; memset(out, 0, 32);
    ++crypto_calls;
    return 0;
}
"""

CHECKS = r"""
int main(void) {
    const char *formats[] = {CRED_PROTO_21_S, CRED_PROTO_22_S, CRED_PROTO_25_S};
    uint8_t rp[32] = {0};
    for (size_t f = 0; f < 3; ++f) {
        size_t overhead = f == 0 ? CRED_ENVELOPE_OVERHEAD
                                 : CRED_SILENT_ENVELOPE_OVERHEAD;
        for (size_t len = 0; len <= overhead + 1; ++len) {
            for (int silent = 0; silent < 2; ++silent) {
                uint8_t id[CRED_SILENT_ENVELOPE_OVERHEAD + 1] = {0};
                memcpy(id, formats[f], CRED_PROTO_LEN);
                crypto_calls = 0;
                int ret = credential_verify(id, len, rp, silent);
                if (len < overhead || (silent && f == 0)) {
                    assert(ret != 0);
                    assert(crypto_calls == 0);
                } else {
                    assert(crypto_calls == 1);
                }
            }
        }
    }
    return 0;
}
"""

AUT_PREAMBLE = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#define CTAP2_ERR_NOT_ALLOWED 0x30
#define CTAP1_ERR_INVALID_PARAMETER 0x02
#define CBOR_ERROR(code) return code
static size_t key_size;
static int ef_keydev;
static bool file_has_data(int ef) { (void)ef; return key_size != 0; }
size_t file_get_size(int ef) { (void)ef; return key_size; }
static struct { bool init; } mse = {true};
static struct { bool present; size_t len; } vendorParamByteString = {true, 48};
static int check_enable(void) {
"""
AUT_CHECKS = r"""
    return 0;
}
int main(void) {
    const size_t invalid[] = {0, 1, 31, 33, 60, 61, 4096};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        key_size = invalid[i];
        assert(check_enable() == CTAP2_ERR_NOT_ALLOWED);
    }
    key_size = 32;
    assert(check_enable() == 0);
    return 0;
}
"""


class CredentialBoundsTest(unittest.TestCase):
    def run_probe(self, source):
        with tempfile.TemporaryDirectory(prefix="cardputer-credential-bounds-") as tmp:
            probe = pathlib.Path(tmp) / "probe.c"
            binary = pathlib.Path(tmp) / "probe"
            probe.write_text(source)
            build = subprocess.run(
                shlex.split(os.environ.get("CC", "cc")) +
                ["-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 str(probe), "-o", str(binary)],
                capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(binary)], capture_output=True,
                                    text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_truncated_envelopes_never_reach_crypto(self):
        header = (ROOT / "src/fido/credential.h").read_text()
        constants = header[header.index("#define CRED_PROTO_21_S"):
                           header.index("extern int credential_verify(")]
        source = (ROOT / "src/fido/credential.c").read_text()
        verify = source[source.index("int credential_verify("):
                        source.index("int credential_create(")]
        self.run_probe(PREAMBLE + constants + verify + CHECKS)

    def test_aut_enable_rejects_incompatible_key_records(self):
        source = (ROOT / "src/fido/cbor_config.c").read_text()
        start = source.index("else if (vendorCommandId == CTAP_CONFIG_AUT_ENABLE)")
        start = source.index("{", start) + 1
        guards = source[start:source.index("mbedtls_chachapoly_context chatx;", start)]
        self.run_probe(AUT_PREAMBLE + guards + AUT_CHECKS)


if __name__ == "__main__":
    unittest.main()
