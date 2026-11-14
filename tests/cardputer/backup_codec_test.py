#!/usr/bin/env python3
"""Independent oracle for the Cardputer-FIDO encrypted backup format.

The C test emits one deterministic envelope with `backup_codec_test --emit`.
This script re-parses that file and validates every chunk with Python's
hashlib PBKDF2-HMAC-SHA256 and `cryptography` AES-256-GCM, i.e. without using
any mbedTLS code or the codec itself.

Usage:
    python3 tests/cardputer/backup_codec_test.py [path-to-backup_codec_test]

The binary path may also come from CF_BACKUP_TEST_BIN.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
from hashlib import pbkdf2_hmac

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

HEADER_SIZE = 80
CHUNK_SIZE = 4096
TAG_SIZE = 16
ITERATIONS = 600_000
STORE_SIZE = 0x102000
CHUNK_COUNT = STORE_SIZE // CHUNK_SIZE
RECORD_SIZE = CHUNK_SIZE + TAG_SIZE
BACKUP_SIZE = HEADER_SIZE + CHUNK_COUNT * RECORD_SIZE

PASSWORD = b"correct horse battery staple"
DEVICE = bytes(
    [
        0x5A, 0x11, 0x42, 0x00, 0xFF, 0x7E, 0x31, 0x88,
        0x09, 0xC4, 0x5D, 0x63, 0xA0, 0x1B, 0xE7, 0x2C,
        0x74, 0x8D, 0x16, 0xFA, 0x39, 0x52, 0xBE, 0x0E,
        0x91, 0x6A, 0xD3, 0x47, 0x85, 0xCF, 0x20, 0x3B,
    ]
)
STORE_SEED = 0x5A


def expected_store() -> bytes:
    """Mirror of fill_store() in backup_codec_test.c."""
    state = 0x9E3779B9 ^ STORE_SEED
    store = bytearray(STORE_SIZE)
    for i in range(STORE_SIZE):
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        store[i] = state >> 24
    for i in range(CHUNK_COUNT):
        marker = f"CFPLAIN-{i:03d}-MARKER".encode("ascii")
        store[i * CHUNK_SIZE : i * CHUNK_SIZE + len(marker)] = marker
    return bytes(store)


def validate(path: str) -> None:
    data = open(path, "rb").read()
    if len(data) != BACKUP_SIZE:
        raise AssertionError(f"file size {len(data)} != {BACKUP_SIZE}")

    if data[:8] != b"CFBAK001":
        raise AssertionError("bad magic")
    version, store_size, chunk_size, iterations = struct.unpack_from(
        "<IIII", data, 8
    )
    if (version, store_size, chunk_size, iterations) != (
        1,
        STORE_SIZE,
        CHUNK_SIZE,
        ITERATIONS,
    ):
        raise AssertionError(
            "bad header fields: "
            f"version={version} store={store_size:#x} "
            f"chunk={chunk_size} iterations={iterations}"
        )
    salt = data[24:40]
    nonce_prefix = data[40:48]
    if data[48:80] != DEVICE:
        raise AssertionError("device binding mismatch")
    if not any(salt):
        raise AssertionError("empty salt")
    if not any(nonce_prefix):
        raise AssertionError("empty nonce prefix")

    key = pbkdf2_hmac("sha256", PASSWORD, salt, ITERATIONS, 32)
    aes = AESGCM(key)
    expected = expected_store()

    for index in range(CHUNK_COUNT):
        offset = HEADER_SIZE + index * RECORD_SIZE
        ciphertext = data[offset : offset + CHUNK_SIZE]
        tag = data[offset + CHUNK_SIZE : offset + RECORD_SIZE]
        nonce = nonce_prefix + struct.pack("<I", index)
        aad = data[:HEADER_SIZE] + struct.pack("<I", index)
        plaintext = aes.decrypt(nonce, ciphertext + tag, aad)
        chunk = expected[index * CHUNK_SIZE : (index + 1) * CHUNK_SIZE]
        if plaintext != chunk:
            raise AssertionError(f"plaintext mismatch at chunk {index}")
        if ciphertext == chunk:
            raise AssertionError(f"ciphertext equals plaintext at chunk {index}")

    # No plaintext may appear in the envelope, including the full-store file.
    for index in range(CHUNK_COUNT):
        marker = f"CFPLAIN-{index:03d}-MARKER".encode("ascii")
        if marker in data:
            raise AssertionError(f"plaintext marker leaked at chunk {index}")
        chunk = expected[index * CHUNK_SIZE : (index + 1) * CHUNK_SIZE]
        if chunk[:128] in data:
            raise AssertionError(f"plaintext block leaked at chunk {index}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "binary",
        nargs="?",
        default=os.environ.get("CF_BACKUP_TEST_BIN"),
        help="path to backup_codec_test (or set CF_BACKUP_TEST_BIN)",
    )
    args = parser.parse_args()
    if not args.binary:
        parser.error("backup_codec_test path is required")
    if not os.access(args.binary, os.X_OK):
        parser.error(f"not executable: {args.binary}")

    with tempfile.TemporaryDirectory(prefix="cf-backup-oracle-") as directory:
        path = os.path.join(directory, "backup.bin")
        subprocess.run([args.binary, "--emit", path], check=True)
        validate(path)

    print(
        "backup_codec_test.py: independent oracle passed "
        f"({CHUNK_COUNT} chunks, {BACKUP_SIZE} bytes)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
