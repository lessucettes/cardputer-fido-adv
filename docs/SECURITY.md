# Security and explicit limits

## Threat model

This device is a programmable microcontroller, not a secure element.

- Device secrets are generated randomly on first provisioning and stored in
  internal flash. They are not password-wrapped and are not protected by eFuse,
  Secure Boot or flash encryption.
- An attacker with physical access, a flash dump or the ability to install
  other firmware can read or replace the secrets and clone the authenticator.
- CRC32 detects accidental corruption and torn writes. It is **not** an
  authenticity mechanism against deliberate tampering.
- Logical deletion of credentials is not guaranteed forensic erasure.

## Anti-provisioning policy

The ADV build must never contain a reachable eFuse write path:

- `otp_esp32.c` is not compiled for `CARDPUTER_ADV`;
- `key_store_esp32.c` provides the platform API and returns an error from
  `otp_platform_enable_secure_boot()`;
- `scripts/cardputer/check-build.py` rejects a map containing `otp_esp32.c`,
  enabled Secure Boot, enabled flash encryption, flash/UART core dumps, a
  wrong target or a missing replacement key store.

No startup password, unlock screen, KDF or locked state exists. Anyone holding
the powered device can confirm requests and use local Saved Keys/deletion.
This is an explicit usability decision, not an oversight.

## PIN and user verification

Browser-requested FIDO PIN/UV behavior is preserved. A local Enter
confirmation is user presence (UP), not user verification (UV). The firmware
does not generate PINs automatically, bypass PINs or claim that every site
works without a FIDO PIN.

## Secrets and logging

- Key record contents are never logged.
- The firmware disables flash and UART core dumps in the reference profile
  because task stacks can contain PINs, signatures and key material.
- `bootloader_random_enable()` is used around first key generation so the
  ESP32-S3 hardware entropy source is active without WiFi/BLE.
- Local credential metadata crosses the UI boundary as RP ID, account name and
  a SHA-256 identity only. Private scalars, seeds, PIN tokens and raw blobs do
  not.

## Recovery limits

- A power cut during the first key provisioning can leave an erased or
  partially written store. The device fails closed or reports an empty store;
  it never silently invents a new identity over non-empty data.
- Losing or reformatting the device loses the credentials. Register a second
  independent authenticator for important accounts.
- There is no backup/export feature in v1.

## Reporting

This is a prototype. A hardware finding is not a supported security claim
until it is reproduced on the recorded candidate revision and entered in
`docs/HARDWARE-VALIDATION.md`.
