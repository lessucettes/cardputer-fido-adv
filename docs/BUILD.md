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

```sh
cmake -S tests/cardputer -B build/cardputer-host -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCARDPUTER_SANITIZERS=ON
cmake --build build/cardputer-host
ctest --test-dir build/cardputer-host --output-on-failure -R '^cardputer_'
```

Expected GREEN: exit 0, all selected tests pass, no sanitizer diagnostics.
Observed on the current candidate: eight native tests pass
(`cardputer_model`, `cardputer_settings`, `cardputer_text`,
`cardputer_view`, `cardputer_key_store`, `cardputer_flash_layout`, `cardputer_presence_bridge`,
`cardputer_credential_service`).

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
