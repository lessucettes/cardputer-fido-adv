# Cardputer FIDO ADV

A USB FIDO2/WebAuthn authenticator for the M5Stack Cardputer ADV, built on a
pinned Pico FIDO core and its matching Pico Keys SDK.

This is a **prototype**, not a certified security key. The firmware builds and
the host-side logic is covered by native tests, but the current candidate has
**not** been run on a Cardputer ADV, a browser or a Launcher installation.

## What it is

- USB HID FIDO2/CTAP2 authenticator with CTAP1/U2F retained.
- Three-item English menu: `Saved Keys`, `Settings`, `Power Off`.
- `Enter` confirms; the physical `Del` (backspace) key cancels.
- Any keyboard key wakes the screen. A waking press is consumed and cannot
  approve, delete or navigate until every key is released.
- Settings: `Volume`, `Brightness`, `Screen Timeout`; persisted in the existing
  `nvs` partition under `cardfido_ui`.
- Resident credentials can be listed and deleted locally with a fresh
  confirmation. Non-resident credentials are not listed.

## What it is not

- No startup password, local vault PIN or unlock screen.
- No eFuse provisioning, Secure Boot activation, flash encryption, JTAG
  disabling or ROM-download disabling.
- No WiFi, BLE, USB MSC, file manager, TOTP/OATH applet or secret export.
- No backup UI. Register a second independent authenticator for important
  accounts.
- No claim of YubiKey-equivalent physical protection. Device secrets live in
  internal flash and are readable with sufficient physical/flash access.

## Building

Native logic tests (no ESP-IDF, no board):

```sh
cmake -S tests/cardputer -B build/cardputer-host -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCARDPUTER_SANITIZERS=ON
cmake --build build/cardputer-host
ctest --test-dir build/cardputer-host --output-on-failure -R '^cardputer_'
```

Firmware (ESP-IDF v5.5.3, ESP32-S3):

```sh
. "$HOME/esp/esp-idf-v5.5.3/export.sh"
bash scripts/cardputer/build.sh
python3 scripts/cardputer/check-build.py build/cardputer-adv
python3 scripts/cardputer/package.py --build build/cardputer-adv --out dist/cardputer-adv
```

The build script never flashes, erases or monitors a device.

## Installation

Only an app-only image is packaged. A pre-existing `fido_store` partition
(type `0x40`, subtype `0x01`, size `0x102000`) is required. Ordinary Launcher
app installation does not create it. See
[`docs/INSTALL-LAUNCHER.md`](docs/INSTALL-LAUNCHER.md).

The partition CSV under `config/cardputer_adv/` is a build fixture, not the
partition table of any particular board. Do not flash it without a verified
backup and an explicit hardware decision.

## Documentation

- [`docs/BUILD.md`](docs/BUILD.md) - host baseline and reproducible build
- [`docs/DESIGN.md`](docs/DESIGN.md) - architecture and storage decisions
- [`docs/SECURITY.md`](docs/SECURITY.md) - threat model and explicit limits
- [`docs/INSTALL-LAUNCHER.md`](docs/INSTALL-LAUNCHER.md) - install and update
- [`docs/HARDWARE-VALIDATION.md`](docs/HARDWARE-VALIDATION.md) - required board tests
- [`docs/UPSTREAM-README.md`](docs/UPSTREAM-README.md) - inherited Pico FIDO documentation

## License

The inherited Pico FIDO and Pico Keys SDK code is AGPL-3.0. Imported M5Stack
keyboard sources are MIT. See `LICENSE`, `pico-keys-sdk/LICENSE` and the SPDX
headers in `src/cardputer/keyboard/` and `src/cardputer/Adafruit_TCA8418/`.
