# Testing a Cardputer ADV over USB

This procedure is for a **real M5Stack Cardputer ADV connected to a computer
by a USB data cable**. It checks the installed firmware, keyboard, display,
browser interaction and reconnect behavior. It does not flash or repartition
the device.

**Current status: hardware UNTESTED.** Host tests, firmware builds and package
checks have passed; they do not establish that the board works. Record only
results you actually observe. Use disposable test credentials, not important
accounts.

## Before starting

- The FIDO firmware must already be installed and running. If not, stop here
  and read [Launcher installation and updates](INSTALL-LAUNCHER.md). First
  installation is a separate task and is not yet hardware-validated.
- The existing `fido_store` partition must be valid. A `Storage error` screen
  is a failure to investigate, not a reason to erase or regenerate the store.
- Use a browser with USB security-key support and allow access to the device.
  Disconnect other external security keys to avoid testing the wrong one.
- Run the commands below from the repository root, on the same computer as
  the browser. Python 3 is sufficient for the local web server.
- Note the installed application revision/hash, OS, browser version and
  whether the battery keeps the ADV running when USB is unplugged.

Do not flash `config/cardputer_adv/partitions-reference.csv`, erase flash or
burn eFuses to perform these checks. The reference table is only a build
fixture. Flash backups contain device secrets; keep them outside Git.

## 1. Connect and check the controls

1. Connect the ADV directly to the computer with a known-good data cable.
2. Start the FIDO app if the board boots into Launcher.
3. Check that the menu contains `Saved Keys`, `Settings`, `Power Off`, with one
   large selected card visible at a time and no startup password. On the main
   menu, `USB connected` means the host has enumerated the device;
   `USB disconnected` means it has not. Settings and saved-key screens must not
   repeat this global status: they work locally without USB. The host should
   detect a USB HID security key; the app need not expose a serial terminal.
4. Navigate the menu with these physical keys. No Fn modifier is needed:

   | Action | Key |
   |---|---|
   | Up / down | `;` / `.` |
   | Left / right in a settings editor | `,` / `/` |
   | Select, save or confirm | Enter |
   | Back or cancel | Del (the backspace key) |

If USB detection fails, try another data cable/port and check host HID access
permissions before diagnosing CTAP. On Linux, `lsusb` can show whether the
device appears when connected; appearance alone does not prove FIDO works.
Do not run the browser as root to bypass a permissions problem.

## 2. Start the local browser test

```sh
python3 tests/cardputer/browser/server.py --bind 127.0.0.1 --port 8765
```

Open **http://localhost:8765** on that computer. Do not substitute
`http://127.0.0.1:8765`, another port or a `file://` URL: the fixture uses RP ID
`localhost` and the fixed origin `http://localhost:8765`. Stop the server with
Ctrl+C when finished.

The browser may offer a built-in authenticator, a password manager, a phone
or an external security key. **Choose the USB security key and require a
prompt on the ADV itself.** The page does not force external-key selection.
A result from another authenticator is not a Cardputer test result.

### Register and sign in

1. Leave a disposable name such as `test-user` in the account field and click
   **Register**. Expect `Create passkey?` on the ADV, with RP `localhost`.
2. Release all ADV keys, then press Enter once. Expect the page to display
   `"ok": true`, `"kind": "create"` and a `credentialId`. Keep that ID locally
   for comparison after reconnect/restart.
3. Click **Sign in (discoverable)**, select the test credential if asked, and
   confirm the new ADV prompt with a fresh Enter. Expect `"ok": true`,
   `"kind": "get"` and the same credential ID. Prefer a single test credential
   for this first run so account selection is unambiguous.

The page requests `residentKey: "preferred"`; successful discoverable sign-in
shows that this credential is discoverable. Registration alone does not prove
it. If the browser requests a FIDO PIN, enter it only in the browser/OS
security-key dialog. Enter on the ADV confirms presence; it is not a PIN or
user verification.

### What a successful page result means

This is a **smoke test**, not a complete WebAuthn verifier. The server checks
the one-time challenge and client-data type/origin. For sign-in it also checks
the RP ID hash. It does **not** verify assertion signatures, parse registration
attestation, retain credential public keys or check signed UP/UV flags.

The current page cannot explicitly test non-resident keys, `excludeList` or
required UV. Clicking Register again generates a new user ID; it is not a
duplicate-registration/exclusion test. Do not mark these scenarios or real-USB
signature verification as passed. They need a separate hardware-capable test
harness; the repository's TCP emulator suite is not that harness.

## 3. Check confirmation, cancellation and timeout

Use the existing test credential and start a separate sign-in for each row.
Wait for the ADV prompt before acting, except for the held-key case.

| Action | Expected result |
|---|---|
| Press a fresh Enter | This request completes successfully. |
| Press Del | The request is denied; it must not report success. |
| Cancel the browser security-key dialog | The prompt clears and cannot approve a later request. |
| Press nothing | The ADV prompt times out (normally 30 seconds); the browser request fails rather than succeeding. Allow up to its 60-second timeout. |
| Hold Enter before starting the request; keep it held | No approval. Release **all** keys, then press Enter again to approve. Repeat with Enter held together with another key. |

After each failed/canceled request, release all keys, dismiss any remaining
browser dialog, and make a new request. It must need a new confirmation.
Exact browser error text varies; denial is not itself a firmware crash.

## 4. Check screen blanking and settings

1. Open Settings. Change `Volume` or `Brightness` using `,` / `/` and save with
   Enter. Reopen the setting and check the saved value. Del should discard an
   unsaved edit. Restore your preferred values after the test.
2. Set `Screen Timeout` to `15 s`, save, return to the menu and leave it idle.
   Expect the screen to blank. This is display-only, not shutdown or a lock.
3. Wake it with Enter, then repeat with Del, a letter and a modifier such as
   Shift. The waking press must not select, delete or navigate. Release every
   key before the next action; a held/chorded waking press must stay consumed.
4. While the screen is blank, start a browser sign-in. The ADV must show the
   request and still require a fresh confirmation; screen timeout must not
   stop USB authentication.
5. At `Volume` 0, key presses and requests must be silent. With a nonzero volume,
   check the short feedback for navigation, Enter, Del, letters and modifiers,
   including a waking press. Held keys must not repeat the sound. Check the
   first increase from 0 and that Del restores the previous volume/brightness
   after an unsaved edit. A request should produce a notification tone. After
   a normal restart, saved volume, brightness and timeout must remain unchanged.

Check menu/prompt legibility on the actual display. List labels have 17 columns;
prompt identity rows have 19, with an ellipsis for overflow. Key details wrap
RP/account text across two 19-column lines per field, but there is no full-text
viewer. **Long RP IDs cannot be fully inspected in this UI**; record this
limitation instead of passing a long-name readability check.

## 5. Reconnect and restart

Keep the local server running. Use the credential from section 2; do not
register a replacement to make a failing persistence test pass.

1. With no request pending, unplug and reconnect USB. If the ADV remains on
   battery power, leave it running. Start a new sign-in: it must reach the ADV,
   need fresh Enter and return the original credential ID.
2. Repeat for 20 cycles on each OS/browser combination being reported. Record
   failures, channel/nonce errors and how recovery was achieved, not just the
   final successful cycle.
3. Start a sign-in and unplug USB while its prompt is pending. If the ADV stays
   powered by its battery, check that the pending prompt is canceled. Pressing
   Enter afterward must not approve it. Cancel any remaining browser dialog,
   reconnect and start a **new** sign-in. It must require fresh confirmation.
4. Restart the ADV normally, launch FIDO again if necessary, and sign in with
   the same credential. This checks persistence across restart, not just USB
   reconnect. Also recheck saved settings.

If unplugging powers the whole board off, note that fact: the cycle does not
test detach cancellation while the firmware remains running. Unplugging USB
while the battery supplies power is **not** a power-loss test.

## 6. Saved Keys and deletion (disposable credentials only)

Create a separate disposable credential (for example, `delete-test`) using
Register and note its credential ID. Keep the original section 2 credential
for persistence and Power Off checks; delete only the additional test key.

1. Open Saved Keys and select that additional credential. Only resident
   credentials are listed; absence of a non-resident credential is not a defect.
2. Open its details, press Enter (`ENT:DEL`) to open deletion confirmation,
   then press Del at the confirmation.
   A subsequent sign-in with that credential must still work.
3. Repeat, confirming deletion with a fresh Enter. Refresh the list by leaving
   and reopening Saved Keys. After restart, that credential must remain absent
   and must not sign in. Other credentials must remain usable.

A `Protected key` row deliberately hides metadata and refuses local deletion.
Record local deletion as BLOCKED for that credential; do not weaken its UV
policy. Use a known removable disposable credential for this test. If several
keys exist for `localhost`, another key signing in is not evidence that the
deleted one survived: compare credential IDs.

## 7. Power Off

With no browser request pending, select Power Off. Test once with USB connected
and once on battery only. After each attempt, record:

- whether the display turns off or an error appears;
- with USB connected, whether a **new** sign-in can still reach the ADV;
- whether the board sleeps, loses power or reboots into Launcher;
- the action needed to restart it and whether the original credential and
  settings survive.

A dark screen alone is not a pass. Power Off must stop servicing new requests
until an explicit restart; cancel the browser's failed attempt before restarting.
The firmware calls M5Unified's `powerOff()`, which may use deep sleep. Do not
describe that as physical power cutoff, particularly with USB attached. Neither
this behavior nor the restart method has yet been demonstrated on an ADV.

## Separate tests: installation, updates and interrupted writes

These are **not prerequisites for repeating the USB checks on an already
installed device**.

- **First installation:** follow [INSTALL-LAUNCHER.md](INSTALL-LAUNCHER.md).
  Inspect the real layout and keep a verified private flash backup before any
  write. If a safe layout/provisioning method is not established, stop; do not
  copy the reference table or move existing partitions.
- **Launcher update:** use disposable credentials and an established app-only
  update procedure. Record the Launcher version, original credential ID and
  settings; update, run another benign app, return to FIDO and restart. The
  same credential must still sign in. A booting app alone is insufficient.
- **Interrupted writes:** leave UNTESTED unless you have a disposable device
  state, a verified recovery route and a controlled way to remove **all** power
  sources during a write. Reset, USB detach and total power loss are different
  experiments. Do not improvise battery wiring or perform this with important
  credentials. Check for a valid pre/post operation state or an explicit storage
  error, never a silently replaced identity. The repository does not provide
  a validated physical power-interruption setup.

## Record the result

Record the installed image's SHA-256 (not a package/archive hash), its source
revision, device label, OS/browser, battery/USB conditions, steps and observations.
Keep the credential ID locally if needed; do not put PINs, real account names,
device secrets or raw flash dumps in a repository report.

`scripts/cardputer/hardware-record.py` can format a JSON worksheet. For example,
the following creates an **untested** worksheet, not a test result:

```sh
python3 scripts/cardputer/hardware-record.py \
  --revision "$(git rev-parse HEAD)" --device "cardputer-adv-1" \
  --scenario "usb_smoke=UNTESTED" --scenario "usb_reconnect=UNTESTED"
```

Use that revision only if it matches the installed image. Supply `--app-sha256`
and `--host` with the actual image hash and OS/browser description. Mark observed
success as PASS, an observed failure as FAIL, a missing prerequisite/tool as
BLOCKED, and an unattempted test as UNTESTED. Keep full signature/PIN-policy
verification separate from a successful browser smoke test.

Stop testing on a storage error, unexpected loss of existing credentials,
approval without a fresh confirmation, or a destructive layout/eFuse operation.
Preserve the evidence and investigate; do not erase the store to hide a failure.
