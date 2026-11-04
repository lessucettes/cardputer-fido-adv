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

Observed: all five upstream container/provider tests passed. Building the
complete `pico_fido` emulator target is currently blocked on the host because
the pinned SDK selects its Linux OTP backend and requires TPM 2.0 development
headers (`tss2/tss2_esys.h` from `libtss2-dev`). This is an environment
dependency of upstream, not a port defect. Do not run
`tests/start-up-and-test.sh`: it modifies system Python paths and starts
services.

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

## Firmware build

The ADV firmware build requires ESP-IDF v5.5.3 and the ESP32-S3 toolchain:

```sh
. "$HOME/esp/esp-idf-v5.5.3/export.sh"
bash scripts/cardputer/build.sh
python3 scripts/cardputer/check-build.py build/cardputer-adv
```

The build script refuses to flash, erase or monitor. A reference partition CSV
under `config/cardputer_adv/` is a build fixture only; it is not the partition
table of any particular board.
