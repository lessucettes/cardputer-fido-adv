#!/usr/bin/env python3
"""Verify a Cardputer ADV build without touching a device.

The verifier inspects only generated files. It is a policy gate, not evidence
that the firmware was flashed or that the hardware works.
"""
from __future__ import annotations

import argparse
import pathlib
import sys

PASS_LINE = "PASS: Cardputer ADV build policy"

FORBIDDEN = {
    "CONFIG_SECURE_BOOT=y": "Secure Boot must stay unprovisioned",
    "CONFIG_SECURE_FLASH_ENC_ENABLED=y": "flash encryption must stay unprovisioned",
    "CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y": "flash core dumps can retain secrets",
    "CONFIG_ESP_COREDUMP_ENABLE_TO_UART=y": "UART core dumps can retain secrets",
    "CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y": "the ADV reference profile is 8 MB",
}
REQUIRED = {
    'CONFIG_IDF_TARGET="esp32s3"': "target must be ESP32-S3",
    "CONFIG_TINYUSB_RHPORT_FS=y": "TinyUSB full-speed device stack is required",
    "CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y": "reference flash capacity is 8 MB",
}
REQUIRED_MAP = (
    "key_store_esp32.c",
    "key_store.c",
    "flash_layout.c",
    "ui.cpp",
    "picokey_board_init",
    "picokey_ui_task",
)
FORBIDDEN_MAP = ("otp_esp32.c",)


def _read(path: pathlib.Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return f"__READ_ERROR__:{exc}"


def check_build(build_dir: str | pathlib.Path) -> list[str]:
    build = pathlib.Path(build_dir)
    errors: list[str] = []
    if not build.is_dir():
        return [f"build directory does not exist: {build}"]

    sdkconfig = build / "sdkconfig"
    if not sdkconfig.is_file():
        errors.append("missing sdkconfig")
    else:
        text = _read(sdkconfig)
        for needle, reason in REQUIRED.items():
            if needle not in text:
                errors.append(f"missing {needle}: {reason}")
        for needle, reason in FORBIDDEN.items():
            if needle in text:
                errors.append(f"forbidden {needle}: {reason}")

    elf = build / "pico_fido.elf"
    binary = build / "pico_fido.bin"
    map_file = build / "pico_fido.map"
    if not elf.is_file():
        errors.append("missing ELF artifact")
    if not binary.is_file():
        errors.append("missing application .bin artifact")
    if elf.is_file() and elf.stat().st_size == 0:
        errors.append("empty ELF artifact")
    if binary.is_file() and binary.stat().st_size == 0:
        errors.append("empty application .bin artifact")

    if not map_file.is_file():
        errors.append("missing linker map")
    else:
        text = _read(map_file)
        for needle in REQUIRED_MAP:
            if needle not in text:
                errors.append(f"missing required object in map: {needle}")
        for needle in FORBIDDEN_MAP:
            if needle in text:
                errors.append(f"forbidden provisioning object in map: {needle}")

    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", nargs="?", default="build/cardputer-adv")
    args = parser.parse_args(argv)
    errors = check_build(args.build_dir)
    if errors:
        for error in errors:
            print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(PASS_LINE)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
