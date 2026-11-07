# Upstream provenance

This repository is a downstream Cardputer ADV port. It keeps upstream source
history and code in their original paths wherever possible, and vendors the
matching Pico Keys SDK so that the port can be reviewed without unpublished
submodule state.

## Frozen revisions

| Component | Repository | Revision | How it is used |
|---|---|---|---|
| Pico FIDO core | `https://github.com/polhenarejos/pico-fido` | `99337e1204348921452a3ce6613e8e17617d4c7d` | Protocol, credential storage, tests, CI baseline |
| Pico Keys SDK | `https://github.com/polhenarejos/pico-keys-sdk` | `d8fce97ca761eb763869c77cc4d3b6fd0be1caf5` | USB/APDU transport, flash filesystem, RNG, crypto helpers |
| Cardputer keyboard/TCA8418 sources | `https://github.com/gavhu10/FIDO-Cardputer` | `2b277b837dc1f936b44eca20acc3f52edb228ad3` | Imported `Keyboard/`, `common.h`, `Adafruit_TCA8418/` only |
| M5 Launcher reference | `https://github.com/bmorcelli/Launcher` | `e9d631e0cb2eaf49243fcff62235adac4f44fcb0` | Read-only analysis of installation and partition behavior |

The upstream development snapshot reports version 8.2 in `src/fido/version.h`.
The latest stable upstream release at the time of the fork was v8.0. This port
must be described as a development snapshot, not as a stable 8.2 release.

## Why the SDK is vendored

Upstream `pico-fido` tracks `pico-keys-sdk` as a gitlink. The Cardputer port
needs small, reviewable changes in the SDK (OTP replacement, flash offset
translation, presence routing). Keeping those changes inside an unpublished
submodule would make the security-relevant diff invisible. Instead the pinned
SDK tree is committed as ordinary source, and the differences from its pinned
upstream revision can be reviewed with:

```sh
git clone https://github.com/polhenarejos/pico-keys-sdk.git /tmp/pico-keys-sdk-upstream
git -C /tmp/pico-keys-sdk-upstream checkout d8fce97ca761eb763869c77cc4d3b6fd0be1caf5
diff -ru --exclude=third-party --exclude=.git /tmp/pico-keys-sdk-upstream pico-keys-sdk
```

Third-party dependencies fetched by `pico-keys-sdk/cmake/deps.cmake` are build
inputs, not part of this repository, and are ignored by Git. Their pinned
versions are recorded in `pico-keys-sdk/cmake/deps.cmake` and in
`dependencies.lock` for the ESP-IDF build.
