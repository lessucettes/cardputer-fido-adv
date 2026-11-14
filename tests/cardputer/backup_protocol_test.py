#!/usr/bin/env python3
"""End-to-end credential continuity test for the encrypted Cardputer backup.

The test creates real CTAP2 registrations in the pinned Pico FIDO emulator,
runs the resulting store image through the production encrypted-backup codec
and the production apply engine (`backup_codec.c`, `backup_store.c` and
`key_store.c`), then restores a *re-created* emulator flash/root state and
verifies the original credential IDs and signatures without enrolling again.

`backup_protocol_tool.c` is only the file-backed adapter used to invoke the
production functions and to decode restored key pages. It is not an encryptor.

Usage (after the parent CMake integration provides the tool target):

    .venv-protocol/bin/python tests/cardputer/backup_protocol_test.py \
        --emulator build/emulator/pico_fido \
        --tool build/cardputer-host/cardputer_backup_protocol_tool

Manual tool build from the repository root if the CMake target is not present:

    cc -std=c11 -Wall -Wextra -Werror -g -O1 \
       -fsanitize=address,undefined -fno-omit-frame-pointer \
       -I src/cardputer -I pico-keys-sdk/third-party/mbedtls/include \
       tests/cardputer/backup_protocol_tool.c \
       build/cardputer-host/libcardputer_backup_core.a \
       -o build/cardputer-host/cardputer_backup_protocol_tool

Coverage limits (deliberately not hidden):
  * The host emulator is the upstream 8 MiB `memory.flash` build, not the
    Cardputer ADV firmware. ADV discovers the partition in the validated table
    and uses a 0x100000 logical filesystem at `fido_store + 0x2000`. The test
    backs up the top 0x100000 bytes of the emulator flash because the emulator
    allocator places every record chain and both pool headers in that window.
    Those records contain absolute 8 MiB-space pointers, so this fido_store
    image is *not* byte-valid for the ADV 1 MiB mapping. The production codec
    and apply engine are byte-transparent, so the encrypted backup/restore
    proof is unaffected, but on-device logical-address relocation and the ESP
    partition driver are not exercised.
  * The emulator takes its device roots from `PICO_KEYS_OTP_FILE`
    (`otp_file.c`); ADV keeps mirrored roots in the two fido_store key pages.
    The test encodes its ephemeral emulator roots with the production key
    record format, backs them up and restores them, then feeds the restored
    key pages back into the emulator OTP file. This proves the production key
    record decode/recovery and key-pages-last restore path, but not the ADV
    flash-backed OTP adapter and partition boot wiring.
  * The codec device binding is derived from the same Linux host identity
    algorithm the emulator build uses, not the ESP32 efuse. It is a public
    value, not a secret.
  * The tool's NOR model is memory-backed test harness, not ESP flash. Power
    loss and media failure are covered by `backup_store_test.c`, not here.
  * No physical USB, hardware, PIN, or display/keyboard path is exercised.
All secrets are generated per run; no real backup or hardware key is used.

The emulator exposes CTAPHID on 127.0.0.1:35962 (its CCID base is 35963).
This suite owns that port; do not run it concurrently with another protocol
suite. It refuses to start if the port is already in use.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import secrets
import socket
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "cardputer"))

try:
    from fido2.ctap import CtapError
except ModuleNotFoundError:  # optional during unittest policy-test discovery
    CtapError = None

from protocol_test import (  # noqa: E402  (path and dependency first)
    Emulator,
    KEY_PARAMS,
    connect,
    create_user,
)

RP = {"id": "backup-protocol.example", "name": "Backup protocol test"}
FLASH_SIZE = 8 * 1024 * 1024
WINDOW_SIZE = 0x100000
FLASH_SECTOR = 0x4000
FLASH_DATA_HEADER = 12
END_ROM_POOL = FLASH_SIZE - FLASH_DATA_HEADER - 4
START_ROM_POOL = END_ROM_POOL - 4 * FLASH_SECTOR
END_DATA_POOL = START_ROM_POOL - FLASH_DATA_HEADER
WINDOW_BASE = FLASH_SIZE - WINDOW_SIZE
STORE_SIZE = 0x102000
BACKUP_HEADER = 80
BACKUP_CHUNK = 4096
BACKUP_TAG = 16
BACKUP_SIZE = BACKUP_HEADER + (STORE_SIZE // BACKUP_CHUNK) * (
    BACKUP_CHUNK + BACKUP_TAG
)
SECP256K1_ORDER = int(
    "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141", 16
)


class TestFailure(AssertionError):
    pass


def check(condition: object, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def make_otp() -> bytes:
    """Two random 32-byte roots; key2 must satisfy the production scalar check."""
    key1 = secrets.token_bytes(32)
    while True:
        key2 = secrets.token_bytes(32)
        value = int.from_bytes(key2, "big")
        if 0 < value < SECP256K1_ORDER:
            return key1 + key2


def emulator_device_binding() -> bytes:
    """Mirror serial.c get_linux_hardware_id() for the emulator build."""
    fields = (
        ("UUID=", "/sys/class/dmi/id/product_uuid"),
        ("BOARD=", "/sys/class/dmi/id/board_serial"),
        ("PRODUCT=", "/sys/class/dmi/id/product_serial"),
        ("CHASSIS=", "/sys/class/dmi/id/chassis_serial"),
        ("MACHINE=", "/etc/machine-id"),
    )
    bad = {
        "none",
        "unknown",
        "default string",
        "to be filled by o.e.m.",
        "00000000-0000-0000-0000-000000000000",
        "ffffffff-ffff-ffff-ffff-ffffffffffff",
    }
    serial = []
    for prefix, path in fields:
        try:
            value = pathlib.Path(path).read_text(encoding="ascii").splitlines()[0]
        except (OSError, IndexError, UnicodeDecodeError):
            continue
        value = value.rstrip("\r\n")
        if not value or value.lower() in bad:
            continue
        serial.append(f"{prefix}{value};")
    if serial:
        board_id = hashlib.sha256("".join(serial).encode()).digest()[:16]
    else:
        board_id = bytes(16)  # serial_init() fallback
    return hashlib.sha256(board_id).digest()


def read_ptr(flash: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", flash, offset)[0]


def walk_records(flash: bytes, head: int) -> list[tuple[int, int, int]]:
    """Validate one upstream filesystem record chain inside the 8 MiB image."""
    base = read_ptr(flash, head)
    records = []
    seen = set()
    first = True
    while base:
        check(WINDOW_BASE <= base < FLASH_SIZE,
              f"record {base:#x} outside the emulator filesystem window")
        check(base not in seen, f"cycle in record chain at {base:#x}")
        seen.add(base)
        check(base + 20 <= FLASH_SIZE, "record header outside flash")
        next_addr, previous, fid, length = struct.unpack_from(
            "<QQHH", flash, base
        )
        check(fid not in (0x0000, 0xFFFF), f"invalid record fid {fid:#x}")
        header = 20
        if length == 0xFFFF:
            check(base + 24 <= FLASH_SIZE, "extended length outside flash")
            length = struct.unpack_from("<I", flash, base + 20)[0]
            header = 24
        check(base + header + length <= FLASH_SIZE,
              f"record {base:#x} payload outside flash")
        check(next_addr == 0 or next_addr < base,
              f"record {base:#x} next pointer {next_addr:#x} is not descending")
        if first:
            check(previous == head,
                  f"first record prev {previous:#x} does not match header "
                  f"{head:#x}")
            first = False
        else:
            check(previous > base,
                  f"record {base:#x} prev pointer {previous:#x} is not newer")
        records.append((base, fid, length))
        base = next_addr
    return records


def inspect_emulator_flash(flash: bytes) -> tuple[bytes, list[tuple[int, int, int]]]:
    check(len(flash) == FLASH_SIZE,
          f"emulator flash is {len(flash):#x}, expected {FLASH_SIZE:#x}")
    check(WINDOW_BASE <= END_DATA_POOL < FLASH_SIZE, "data pool header window")
    check(WINDOW_BASE <= END_ROM_POOL < FLASH_SIZE, "rom pool header window")
    dynamic = walk_records(flash, END_DATA_POOL)
    persistent = walk_records(flash, END_ROM_POOL)
    check(len(dynamic) >= 10,
          f"only {len(dynamic)} filesystem records found")
    fids = {fid for _, fid, _ in dynamic + persistent}
    check(any((fid >> 8) == 0xD1 for fid in fids),
          "resident manifest record missing from emulator flash")
    check(any(0xD3 <= (fid >> 8) <= 0xD6 for fid in fids),
          "resident credential record missing from emulator flash")
    return flash[WINDOW_BASE:], dynamic + persistent


def run_tool(tool: pathlib.Path, arguments: list[str], password: str | None,
             expect_success: bool = True) -> subprocess.CompletedProcess:
    environment = dict(os.environ)
    if password is not None:
        environment["CF_BACKUP_PROTOCOL_PASSWORD"] = password
    result = subprocess.run(
        [str(tool), *arguments],
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=600,
    )
    if expect_success and result.returncode != 0:
        detail = (result.stderr or result.stdout)[-2000:]
        raise TestFailure(
            f"{tool.name} {' '.join(arguments[:1])} failed "
            f"({result.returncode}): {detail}"
        )
    if not expect_success and result.returncode == 0:
        raise TestFailure(
            f"{tool.name} {' '.join(arguments[:1])} unexpectedly succeeded"
        )
    return result


def start_emulator(emulator_path: pathlib.Path,
                   workdir: pathlib.Path) -> Emulator:
    emulator = Emulator(emulator_path, workdir)
    emulator.start()
    return emulator


def stop_emulator(emulator: Emulator) -> None:
    emulator.stop()


def register_fixture(ctap) -> dict[str, dict]:
    credentials = {}
    for name, resident in (("alice", True), ("bob", True), ("eve", False)):
        attested = ctap.make_credential(
            hashlib.sha256(f"register-{name}".encode()).digest(),
            RP,
            create_user(name),
            KEY_PARAMS,
            options={"rk": resident},
        )
        credentials[name] = {
            "cred_id": bytes(attested.auth_data.credential_data.credential_id),
            "public_key": attested.auth_data.credential_data.public_key,
            "resident": resident,
        }
    return credentials


def assert_credentials(ctap, credentials: dict[str, dict], label: str) -> None:
    resident = {name: item for name, item in credentials.items()
                if item["resident"]}
    nonresident = {name: item for name, item in credentials.items()
                   if not item["resident"]}

    for name, item in resident.items():
        client_data_hash = hashlib.sha256(
            f"{label}-{name}-allow-list".encode()
        ).digest()
        assertion = ctap.get_assertion(
            RP["id"],
            client_data_hash,
            allow_list=[{"type": "public-key", "id": item["cred_id"]}],
        )
        assertion.verify(client_data_hash, item["public_key"])
        check(bytes(assertion.credential["id"]) == item["cred_id"],
              f"{label}: resident {name} credential ID changed")

    discoverable_hash = hashlib.sha256(f"{label}-discoverable".encode()).digest()
    assertions = ctap.get_assertions(RP["id"], discoverable_hash)
    check(len(assertions) == len(resident),
          f"{label}: expected {len(resident)} resident credentials, "
          f"got {len(assertions)}")
    discovered = {}
    for assertion in assertions:
        check(assertion.user is not None, f"{label}: resident user missing")
        discovered[assertion.user["id"]] = assertion
    expected_users = {name.encode() for name in resident}
    check(set(discovered) == expected_users,
          f"{label}: discoverable users {set(discovered)} != {expected_users}")
    for name, item in resident.items():
        assertion = discovered[name.encode()]
        assertion.verify(discoverable_hash, item["public_key"])
        check(bytes(assertion.credential["id"]) == item["cred_id"],
              f"{label}: discoverable {name} credential ID changed")
    for name in nonresident:
        check(name.encode() not in discovered,
              f"{label}: nonresident {name} was discoverable")

    for name, item in nonresident.items():
        client_data_hash = hashlib.sha256(
            f"{label}-{name}-allow-list".encode()
        ).digest()
        assertion = ctap.get_assertion(
            RP["id"],
            client_data_hash,
            allow_list=[{"type": "public-key", "id": item["cred_id"]}],
        )
        assertion.verify(client_data_hash, item["public_key"])
        check(bytes(assertion.credential["id"]) == item["cred_id"],
              f"{label}: nonresident {name} credential ID changed")


def run(emulator_path: pathlib.Path, tool: pathlib.Path) -> None:
    if CtapError is None:
        raise TestFailure(
            "fido2 is required; use the environment from tests/cardputer/requirements-protocol.txt")
    check(emulator_path.is_file(), f"emulator not found: {emulator_path}")
    check(os.access(tool, os.X_OK), f"tool not executable: {tool}")

    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        # Repeated runs can leave closed emulator connections in TIME_WAIT.
        # Reuse those addresses, but still refuse an active listener.
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        probe.bind(("127.0.0.1", 35962))
    except OSError as exc:
        raise TestFailure(
            "port 35962 is busy; do not run protocol suites concurrently"
        ) from exc
    finally:
        probe.close()

    device = emulator_device_binding()
    device_hex = device.hex()
    password = secrets.token_urlsafe(24)
    wrong_password = password + "x"
    wrong_device = bytes([device[0] ^ 0x01]) + device[1:]
    check(12 <= len(password) <= 64, "generated password length is invalid")

    with tempfile.TemporaryDirectory(prefix="cf-backup-protocol-") as directory:
        root = pathlib.Path(directory)
        source_workdir = root / "device-source"
        source_workdir.mkdir()
        otp_source = make_otp()
        (source_workdir / "otp.bin").write_bytes(otp_source)
        os.chmod(source_workdir / "otp.bin", 0o600)

        source_store = root / "source.fidostore"
        source_otp = root / "source.otp"
        source_fs = root / "source.fs"
        backup = root / "backup.cfb"
        target_store = root / "target.fidostore"
        target_otp = root / "target.otp"
        target_fs = root / "target.fs"
        restored_otp = root / "restored.otp"
        restored_fs = root / "restored.fs"

        emulator = start_emulator(emulator_path, source_workdir)
        try:
            hid, ctap = connect()
            check("FIDO_2_0" in ctap.info.versions, "FIDO_2_0 missing")
            check(ctap.info.options.get("rk") is True,
                  "resident keys unavailable")
            credentials = register_fixture(ctap)
            assert_credentials(ctap, credentials, "pre-backup")
            hid.close()
        finally:
            stop_emulator(emulator)
        print("PASS: emulator created 2 resident and 1 nonresident credential; "
              "baseline signatures verify")

        flash = (source_workdir / "memory.flash").read_bytes()
        window, records = inspect_emulator_flash(flash)
        check(len(window) == WINDOW_SIZE, "filesystem window size")
        (root / "window.bin").write_bytes(window)
        run_tool(tool, ["pack", str(source_store),
                        str(source_workdir / "otp.bin"),
                        str(root / "window.bin")], None)
        run_tool(tool, ["extract", str(source_store), str(source_otp),
                        str(source_fs)], None)
        check(source_otp.read_bytes() == otp_source,
              "production key record did not preserve the emulator roots")
        check(source_fs.read_bytes() == window,
              "production key extract did not preserve the filesystem window")
        print(f"PASS: online filesystem records captured ({len(records)} "
              "records) and staged with production key records")

        run_tool(tool, ["export", str(source_store), str(backup), device_hex],
                 password)
        check(backup.is_file(), "backup file missing")
        check(backup.stat().st_size == BACKUP_SIZE,
              f"backup size {backup.stat().st_size} != {BACKUP_SIZE}")
        blob = backup.read_bytes()
        check(blob[:8] == b"CFBAK001", "backup magic")
        check(otp_source[:32] not in blob and otp_source[32:] not in blob,
              "emulator root appears in the encrypted backup")
        print(f"PASS: production codec exported an encrypted backup "
              f"({BACKUP_SIZE} bytes) without plaintext roots")

        run_tool(tool, ["verify", str(backup), device_hex], password)
        run_tool(tool, ["verify", str(backup), device_hex], wrong_password,
                 expect_success=False)
        run_tool(tool, ["verify", str(backup), wrong_device.hex()], password,
                 expect_success=False)
        print("PASS: wrong password and wrong device binding are rejected")

        target_root = make_otp()
        target_window = bytes(((index * 37 + 11) & 0xFF)
                              for index in range(WINDOW_SIZE))
        target_otp.write_bytes(target_root)
        target_fs.write_bytes(target_window)
        os.chmod(target_otp, 0o600)
        run_tool(tool, ["pack", str(target_store), str(target_otp),
                        str(target_fs)], None)
        target_before = hashlib.sha256(target_store.read_bytes()).digest()

        run_tool(tool, ["restore", str(backup), str(target_store), device_hex],
                 password)
        check(hashlib.sha256(target_store.read_bytes()).digest()
              != target_before, "restore left the target store unchanged")
        check(target_store.read_bytes() == source_store.read_bytes(),
              "restored fido_store is not byte-identical to the source store")
        print("PASS: production apply engine replaced a different on-disk "
              "store byte-for-byte")

        run_tool(tool, ["extract", str(target_store), str(restored_otp),
                        str(restored_fs)], None)
        check(restored_otp.read_bytes() == otp_source,
              "restored key pages do not decode to the original emulator roots")
        check(restored_otp.read_bytes() != target_root,
              "restored key pages still hold the target identity")
        check(restored_fs.read_bytes() == window,
              "restored filesystem window differs from the backup payload")
        print("PASS: restored key pages decode READY to the source roots; "
              "no new identity was provisioned")

        # Control: the same restored filesystem with a different root must not
        # resurrect the old credentials. This proves the key half is real.
        bad_workdir = root / "device-wrong-root"
        bad_workdir.mkdir()
        (bad_workdir / "otp.bin").write_bytes(target_root)
        os.chmod(bad_workdir / "otp.bin", 0o600)
        bad_flash = bytearray(FLASH_SIZE)
        bad_flash[WINDOW_BASE:] = restored_fs.read_bytes()
        (bad_workdir / "memory.flash").write_bytes(bad_flash)
        bad_emulator = start_emulator(emulator_path, bad_workdir)
        try:
            hid, ctap = connect()
            for name in ("alice", "eve"):
                item = credentials[name]
                client_data_hash = hashlib.sha256(
                    f"wrong-root-{name}".encode()).digest()
                try:
                    ctap.get_assertion(
                        RP["id"], client_data_hash,
                        allow_list=[{"type": "public-key",
                                     "id": item["cred_id"]}],
                    )
                except CtapError as exc:
                    check(exc.code == CtapError.ERR.NO_CREDENTIALS,
                          f"wrong root returned {exc.code:#x} for {name}")
                else:
                    raise TestFailure(
                        f"wrong root accepted the old {name} credential")
            hid.close()
        finally:
            stop_emulator(bad_emulator)
        print("PASS: restored filesystem with a different root rejects the "
              "old credentials")

        # Re-created device state: restored key pages + restored filesystem.
        restored_workdir = root / "device-restored"
        restored_workdir.mkdir()
        (restored_workdir / "otp.bin").write_bytes(restored_otp.read_bytes())
        os.chmod(restored_workdir / "otp.bin", 0o600)
        restored_flash = bytearray(FLASH_SIZE)
        restored_flash[WINDOW_BASE:] = restored_fs.read_bytes()
        (restored_workdir / "memory.flash").write_bytes(restored_flash)
        restored_emulator = start_emulator(emulator_path, restored_workdir)
        try:
            hid, ctap = connect()
            check("FIDO_2_0" in ctap.info.versions, "restored FIDO_2_0 missing")
            assert_credentials(ctap, credentials, "post-restore")
            ctap.get_info()
            hid.close()
        finally:
            stop_emulator(restored_emulator)
        print("PASS: resident and nonresident signatures verify after the "
              "re-created emulator storage/root state")
        print("PASS: no enrollment occurred; discoverable credential set is "
              "unchanged")
        print("PASS: backup protocol continuity suite")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--emulator",
        type=pathlib.Path,
        default=ROOT / "build" / "emulator" / "pico_fido",
    )
    parser.add_argument(
        "--tool",
        type=pathlib.Path,
        default=ROOT / "build" / "cardputer-host" /
        "cardputer_backup_protocol_tool",
    )
    arguments = parser.parse_args()
    try:
        run(arguments.emulator, arguments.tool)
    except (TestFailure, RuntimeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
