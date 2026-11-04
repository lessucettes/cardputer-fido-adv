# Design

## Layers

- `src/fido/` - pinned Pico FIDO protocol core. Only narrow Cardputer hooks
  were added: startup/board hooks, ADV presence wait, reset/selection prompt
  kinds and the resident-credential adapter.
- `pico-keys-sdk/` - vendored, pinned SDK. ADV changes are limited to:
  - replacing the ESP32 eFuse OTP backend with `key_store_esp32.c`;
  - mapping the SDK flash filesystem past the two key pages;
  - exposing board/UI hooks in `main.c`.
- `src/cardputer/` - platform-independent UI/storage logic and the ESP32
  adapters:
  - `model` - menu, requests, settings editors and keyboard freshness;
  - `presence_bridge` - generation-gated decisions, two-stage reset;
  - `key_store` - committed key records, CRC and recovery policy;
  - `flash_layout` - partition validation and offset translation;
  - `credential_service` - serialized local list/delete requests;
  - `board`/`input`/`display`/`audio`/`power` - M5Unified adapters.

Native tests compile the platform-independent files with ASan/UBSan and do not
need a board, ESP-IDF, TPM or network.

## Storage

`fido_store` is type `0x40`, subtype `0x01`, size `0x102000`:

- two 4096-byte key-record pages at the start;
- `0x100000` bytes for the existing SDK flash filesystem at offset `0x2000`.

Key record format (little endian, bytes, not a dumped C struct):

| Offset | Bytes | Value |
|---|---:|---|
| 0 | 8 | ASCII `CFKEY001` |
| 8 | 4 | version 1 |
| 12 | 4 | payload length 64 |
| 16 | 32 | `otp_key_1` random master wrapping secret |
| 48 | 32 | `otp_key_2` valid secp256k1 private scalar |
| 80 | 4 | CRC32/ISO-HDLC over bytes 0..79 |
| 84 | 4 | commit marker `0x43464B31`, written last |

Both pages are mirrored. `EMPTY` is returned only when both key pages and the
whole filesystem are erased. One valid record is `READY`; two different valid
records or any non-erased data without a valid record is `CORRUPT`. Loading
never writes. Provisioning never overwrites existing or corrupt data.

The filesystem portion is the only region passed to the SDK. Logical SDK
offsets are translated by `+0x2000`; the key pages are not reachable through
the filesystem API.

## Presence

`wait_button_pressed_timeout()` on ADV starts a request in `PresenceBridge` and
pumps the UI until the bridge returns `confirmed`, `cancelled` or `timed_out`.
The SDK's weak board/UI/storage hooks are overridden in `ui.cpp`, so
`picokey_board_init()` initializes the controller before key generation,
`picokey_ui_task()` keeps the menu serviced on the owner loop, and a storage
failure halts before USB is installed.
The bridge:

- copies and bounds the RP ID and account name;
- ties every decision to a generation;
- consumes a decision exactly once;
- blocks held/waking keys until every key is released;
- requires two separate fresh Enters for a host reset;
- shows `Check existing key?` for exclusion checks;
- gives Del priority over a simultaneous Enter;
- cancels a pending prompt when USB detaches.

The ADV build defines `FORCE_BUTTON_WAIT`, so a core request that asks for UP
gets a 30-second prompt even though the board has no boot-button gesture.
Requests that do not ask for UP remain silent.

## Local credentials

`credential_service` queues one local list/delete request at a time and runs it
only when no CTAP operation is busy. Delete identities are SHA-256 digests of
the upstream credential ID, re-resolved at execution time. A stale identity
returns "not found"; it cannot delete whichever credential now occupies the
slot. Credentials requiring UV appear as an opaque `Protected key` row: their
RP/account metadata is not shown and local deletion is refused rather than
weakening their policy.

## Explicit non-goals

- Secure element or trusted boot claims.
- Password-protected local storage.
- Backup/export of secrets.
- Support for arbitrary Launcher repartitioning.
