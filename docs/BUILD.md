# Building and testing

## Host baseline (unmodified upstream)

The pinned Pico FIDO tree was configured and built on Linux with CMake 3.28,
Ninja 1.11, GCC 13.3, Python 3.14 and Git 2.43. `pico-keys-sdk/cmake/deps.cmake`
fetched mbedTLS v3.6.7 and tinycbor v0.6.1 into `pico-keys-sdk/third-party/`.

```sh
cmake -S . -B build/upstream-host -G Ninja -DENABLE_EMULATION=ON
cmake --build build/upstream-host --target \
  fido_object_provider_test fido_object_authorization_test \
  fido_resident_container_test fido_oath_container_test fido_otp_container_test
ctest --test-dir build/upstream-host --output-on-failure
```

Observed: all five upstream container/provider tests passed. The default
emulator build still selects the Linux OTP backend and needs TPM 2.0
development headers (`tss2/tss2_esys.h` from `libtss2-dev`). For a host-only
session the fork adds `ENABLE_DETERMINISTIC_OTP`, backed by
`pico-keys-sdk/src/otp/otp_file.c` and `PICO_KEYS_OTP_FILE`, so no TPM is
required. Do not run `tests/start-up-and-test.sh`: it modifies system Python
paths and starts services.

## Native Cardputer tests

These tests do not need ESP-IDF, M5Unified, TPM, network or a real
authenticator. They compile the platform-independent Cardputer logic with
AddressSanitizer and UndefinedBehaviorSanitizer:

The backup tests also compile the SDK's pinned Mbed TLS sources. If
`pico-keys-sdk/third-party/mbedtls` has not been fetched yet, configure the
deterministic emulator below first. The independent AES-GCM oracle uses
Python `cryptography` (also a dependency of the pinned `fido2` client); select
that environment with `-DPython3_EXECUTABLE=/path/to/venv/bin/python` when
configuring the native tests.

```sh
cmake -S tests/cardputer -B build/cardputer-host -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCARDPUTER_SANITIZERS=ON
cmake --build build/cardputer-host
ctest --test-dir build/cardputer-host --output-on-failure -R '^cardputer_'
```

Expected GREEN: exit 0, all selected tests pass, no sanitizer diagnostics.
The original native checks cover
(`cardputer_model`, `cardputer_settings`, `cardputer_audio`, `cardputer_text`,
`cardputer_view`, `cardputer_key_store`, `cardputer_flash_layout`,
`cardputer_presence_bridge`, `cardputer_credential_service`). Additional
`cardputer_backup_*` tests exercise encrypted full-store recovery, malformed
and unauthenticated files, NOR flash failures, key-page commit ordering and an
independent Python AES-GCM/PBKDF2 oracle. The KDF uses the production work
factor, so the sanitized crypto scenarios take longer than UI unit tests.

The audio test models the pinned M5Unified 0.2.13 speaker contract, where
`playRaw()` reports success even when `begin()` fails, and checks that a
pending tone is kept until the speaker actually starts. It does not verify
physical audio output.

The audio checks also cover mute discarding old queued feedback, rapid input
not overwriting a playing sample buffer, and the mixer-start busy deadline.
`cardputer_display` and `cardputer_display_fallback` exercise the production
board renderer with recording panel/canvas adapters: unchanged-frame caching,
text-field boundaries, hidden status expiry during prompts, sleep/wake, and
direct rendering when canvas allocation fails. These checks establish draw
and scheduling behavior, not physical flicker or real-time audio latency.

The view test exercises the production LCD layout on a recording display:
240x135 bounds, nonoverlapping text, minimum 2x Font0 size, menu navigation,
setting values, identity rows, destructive warnings and confirmation hints.
It does not verify physical LCD contrast, display flicker or speaker quality.

The Saved Keys adapter regression uses the host FIDO headers and real identity
hashing with an in-memory resident backend. After configuring the emulator below:

```sh
cmake --build build/emulator --target fido_cardputer_credentials_test
ctest --test-dir build/emulator --output-on-failure -R '^fido_cardputer_credentials_test$'
```

## Isolated CTAP2 protocol tests

The host emulator is driven over its TCP HID socket. This exercises CTAPHID
framing, CBOR, registration, assertions and resident-credential storage through
the pinned upstream stack. It is not an electrical USB or Cardputer-keyboard
test.

```sh
cmake -S . -B build/emulator -G Ninja \
  -DENABLE_EMULATION=ON -DENABLE_DETERMINISTIC_OTP=ON
cmake --build build/emulator --target pico_fido

python3 -m venv .venv-protocol
.venv-protocol/bin/pip install -r tests/cardputer/requirements-protocol.txt
.venv-protocol/bin/python scripts/cardputer/run-protocol-tests.py \
  --emulator build/emulator/pico_fido --reconnect-cases
```

The suite starts and stops its own emulator in a temporary directory, verifies
assertion signatures with the independent `fido2` client, checks two accounts
under one RP, RP mismatch, persistence across an emulator restart, and the
synthetic reconnect/fragment-timeout cases. Observed on the current candidate:
all cases pass. Hardware USB reconnect, browser interaction and the physical
presence prompt remain separate manual gates.

### Encrypted-backup credential continuity

After building the native targets and emulator, run:

```sh
.venv-protocol/bin/python tests/cardputer/backup_protocol_test.py \
  --emulator build/emulator/pico_fido \
  --tool build/cardputer-host/cardputer_backup_protocol_tool
```

This registers two resident credentials and one nonresident credential, exports
the generated filesystem and roots through the production backup codec, applies
the encrypted snapshot through the production restore engine, recreates the
emulator's storage/root state and verifies the original IDs and assertion
signatures without enrolling again. A different-root control must fail.

The host emulator has a different filesystem address space and a separate OTP
file. The test explicitly bridges those test-only layouts; it does not prove
the ESP partition adapter, physical USB, SD card, keyboard or interrupted-write
behavior on a real board. Run protocol suites sequentially: they share a fixed
TCP listener.

## Firmware build

The ADV firmware build requires ESP-IDF v5.5.3 and the ESP32-S3 toolchain:

```sh
. "$HOME/esp/esp-idf-v5.5.3/export.sh"
bash scripts/cardputer/build.sh
python3 scripts/cardputer/check-build.py build/cardputer-adv
python3 scripts/cardputer/package.py --build build/cardputer-adv \
  --out dist/cardputer-adv
```

Observed on the current candidate:

- ESP-IDF v5.5.3, target ESP32-S3.
- M5Unified 0.2.13 and M5GFX 0.2.36 from the component registry, pinned in
  `dependencies.lock`.
- `build/cardputer-adv/pico_fido.bin` produced, size `0xd9220`.
- Build-policy verifier prints `PASS: Cardputer ADV build policy`.
- App-only package is written to `dist/cardputer-adv/`.

The build script refuses to flash, erase or monitor. A reference partition CSV
under `config/cardputer_adv/` is a build fixture only; it is not the partition
table of any particular board.

If `export.sh` runs inside an outer Python virtual environment, ESP-IDF may
refuse to create its own environment. Run it with a system Python for the
tooling bootstrap, for example:

```sh
env -u VIRTUAL_ENV -u PYTHONPATH -u PYTHONHOME PATH=/usr/bin:/bin:/usr/local/bin \
  bash -c '. "$HOME/esp/esp-idf-v5.5.3/export.sh" && bash scripts/cardputer/build.sh'
```

## Release checks

```sh
bash scripts/cardputer/release-check.sh
```

This runs native tests, Python policy/package tests, the firmware build, the
policy verifier and packaging. It requires `IDF_PATH`; a missing toolchain is
a failed gate, not a successful skip. Hardware validation remains a separate
manual gate.
