#!/usr/bin/env python3
"""Build an app-only Launcher package for the Cardputer ADV.

The package contains only the application image, its manifest and install
instructions. Bootloaders, partition tables, blank credential images and
eFuse scripts are deliberately excluded.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
APP_NAME = "Cardputer-FIDO-ADV.bin"
MANIFEST_NAME = "manifest.json"
INSTALL_NAME = "INSTALL.txt"
ESP_IMAGE_MAGIC = 0xE9
ESP32S3_CHIP_ID = 9
PROHIBITED_NAMES = {
    "bootloader.bin",
    "partition-table.bin",
    "ota_data_initial.bin",
}


def load_checker():
    script = ROOT / "scripts" / "cardputer" / "check-build.py"
    spec = importlib.util.spec_from_file_location("check_build", script)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def upstream_version() -> str:
    text = (ROOT / "src" / "fido" / "version.h").read_text(encoding="utf-8")
    match = re.search(r"#define\s+PICO_FIDO_VERSION\s+0x([0-9A-Fa-f]+)", text)
    if match is None:
        return "unknown"
    value = int(match.group(1), 16)
    return f"{(value >> 8) & 0xFF}.{value & 0xFF}"


def git_revision() -> str:
    try:
        return subprocess.check_output(
            ["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_app_image(path: pathlib.Path) -> None:
    data = path.read_bytes()
    if len(data) < 24:
        raise ValueError("application image is too short")
    if data[0] != ESP_IMAGE_MAGIC:
        raise ValueError("application image has no ESP image magic")
    chip_id = int.from_bytes(data[12:14], "little")
    if chip_id != ESP32S3_CHIP_ID:
        raise ValueError(f"application image targets chip {chip_id}, not ESP32-S3")
    if path.name in PROHIBITED_NAMES:
        raise ValueError(f"refusing to package {path.name}")


def package(build_dir: pathlib.Path, out_dir: pathlib.Path,
            force: bool = False) -> dict:
    checker = load_checker()
    errors = checker.check_build(build_dir)
    if errors:
        raise ValueError("; ".join(errors))

    app = build_dir / "pico_fido.bin"
    if not app.is_file():
        raise ValueError("missing pico_fido.bin")
    validate_app_image(app)

    if out_dir.exists() and any(out_dir.iterdir()):
        if not force:
            raise ValueError(f"output directory is not empty: {out_dir}")
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    target = out_dir / APP_NAME
    shutil.copyfile(app, target)
    app_hash = sha256_file(target)

    manifest = {
        "name": "Cardputer-FIDO-ADV",
        "version": f"{upstream_version()}-cardputer-adv",
        "target": "esp32s3",
        "board": "M5Stack Cardputer ADV",
        "source": {
            "pico_fido": "99337e1204348921452a3ce6613e8e17617d4c7d",
            "pico_keys_sdk": "d8fce97ca761eb763869c77cc4d3b6fd0be1caf5",
            "candidate_revision": git_revision(),
        },
        "firmware": {
            "file": APP_NAME,
            "size": target.stat().st_size,
            "sha256": app_hash,
        },
        "requires": {
            "partition": "fido_store",
            "type": "0x40",
            "subtype": "0x01",
            "size": "0x102000",
        },
        "files": [
            {"name": APP_NAME, "size": target.stat().st_size, "sha256": app_hash}
        ],
        "notes": [
            "Application-only update; no bootloader, partition table, blank credential image or eFuse script is included.",
            "The fido_store partition must already exist; ordinary Launcher installation does not create it.",
            "Hardware validation is not implied by this package.",
        ],
    }
    (out_dir / MANIFEST_NAME).write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    (out_dir / INSTALL_NAME).write_text(
        "Cardputer FIDO ADV - app-only package\n"
        "\n"
        "1. Back up the device flash and partition table outside this repository.\n"
        "2. Confirm that a type 0x40 / subtype 0x01 fido_store partition of\n"
        "   0x102000 bytes already exists. Do not create it from this package.\n"
        "3. Install Cardputer-FIDO-ADV.bin through the supported Launcher\n"
        "   app-install flow for the pinned Launcher version.\n"
        "4. Reconnect USB and verify a real browser registration and login.\n"
        "\n"
        "This package does not include a bootloader, partition table, credential\n"
        "database or eFuse provisioning step. It is not a hardware guarantee.\n",
        encoding="utf-8",
    )

    produced = sorted(path.name for path in out_dir.iterdir())
    prohibited = PROHIBITED_NAMES.intersection(produced)
    if prohibited:
        raise ValueError(f"prohibited package payloads: {sorted(prohibited)}")
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=pathlib.Path,
                        default=ROOT / "build" / "cardputer-adv")
    parser.add_argument("--out", type=pathlib.Path,
                        default=ROOT / "dist" / "cardputer-adv")
    parser.add_argument("--force", action="store_true",
                        help="replace a non-empty output directory")
    args = parser.parse_args(argv)
    try:
        manifest = package(args.build, args.out, args.force)
    except (OSError, ValueError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    print(f"PACKAGED: {args.out / APP_NAME}")
    print(f"SHA256: {manifest['firmware']['sha256']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
