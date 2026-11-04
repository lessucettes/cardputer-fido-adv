# Hardware validation

## Candidate status

| Gate | Result |
|---|---|
| Native Cardputer tests (ASan/UBSan) | PASS |
| Firmware build, ESP-IDF v5.5.3, ESP32-S3 | PASS |
| Build-policy verifier | PASS |
| App-only package | PASS |
| Real ADV board, keyboard, display, USB, browser, Launcher | **UNTESTED** |

The software result is a prototype. No hardware acceptance criterion is met
until it is observed on a recorded candidate and device.

## Record fields

Use `scripts/cardputer/hardware-record.py` with an approved device label and
candidate revision:

```sh
python3 scripts/cardputer/hardware-record.py \
  --revision <candidate> --device <approved label> \
  --app-sha256 <package hash> --idf v5.5.3 \
  --scenario "boot=PASS" --scenario "usb_reconnect=UNTESTED"
```

The tool refuses missing revision/device fields and accepts only
`PASS`, `FAIL`, `BLOCKED` or `UNTESTED`. Never put keys, PINs, account names,
raw flash dumps or master secrets in a repository report.

## Gate 1 - identify, back up, approve layout

- [ ] Actual flash capacity/mode recorded.
- [ ] Installed Launcher version recorded.
- [ ] Full flash backup created outside Git, size/hash verified.
- [ ] Real partition table read without writes.
- [ ] eFuse status read without burning.
- [ ] Read-only planner produced a free-region proposal.
- [ ] Old/new tables reviewed; no existing range moved or deleted.
- [ ] Separate approval obtained for one-time table provisioning, if needed.

## Gate 2 - boot, keyboard and display

- [ ] No password at first or repeated boot.
- [ ] No eFuse change.
- [ ] ADV TCA8418 keyboard works.
- [ ] Every physical key wakes the screen, including modifiers and Del.
- [ ] Waking Enter/Del performs no action.
- [ ] Held/chorded keys cannot approve.
- [ ] Native Up/Down/Left/Right mapping is usable and documented.
- [ ] Owned text is English only.
- [ ] Labels fit at 240x135; long RP IDs can be inspected.
- [ ] Volume/brightness/timeout persist across restart.
- [ ] Keyboard wake still works after backlight-off.

## Gate 3 - browser registration/login/PIN/deletion

Start the localhost-only fixture:

```sh
python3 tests/cardputer/browser/server.py --bind 127.0.0.1 --port 8765
```

The page talks only to loopback and checks challenge, origin and RP ID hash.
It deliberately does not claim full signature verification; use an independent
host FIDO library for that.

- [ ] Localhost WebAuthn fixture registers a credential.
- [ ] Independent host verifier checks the signature.
- [ ] Resident and non-resident accounts work.
- [ ] Repeated registration with `excludeList` shows no create prompt.
- [ ] Two accounts under one RP both work.
- [ ] PIN-required operations follow browser policy.
- [ ] Enter/Del/cancel/timeout behave as specified.
- [ ] Saved Keys list is metadata-only; deletion is confirmed and persistent.
- [ ] Signed authentication succeeds after reboot.

## Gate 4 - USB reconnect and power

- [ ] 20 consecutive unplug/replug cycles per tested OS/browser.
- [ ] Pending prompt and canceled request do not survive reconnect.
- [ ] Real signed assertion after each reconnect.
- [ ] No Wrong Nonce or channel mismatch.
- [ ] Power Off on battery and on USB.
- [ ] No reboot into Launcher; no corruption on next boot.
- [ ] Volume 0 is silent; screen-off does not stop CTAP.

## Gate 5 - Launcher update and interrupted writes

- [ ] Throwaway credentials created and signatures verified.
- [ ] App update through the supported Launcher procedure.
- [ ] Other benign app launched and returned.
- [ ] Same credentials authenticate after the round trip and reboot.
- [ ] Controlled power interruption during registration/deletion/settings.
- [ ] Result is a valid pre/post transaction or an explicit storage error.
- [ ] eFuse status unchanged after all tests.

## Stop conditions

Stop and report rather than continuing if:

- the partition layout has no safe free region;
- a test requires weakening a documented security policy;
- the device reports a storage error or regenerates an identity;
- an eFuse value changed;
- a Launcher operation would move or delete existing data.
