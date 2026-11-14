# microSD backup and restore

## Scope and status

The firmware implements password-encrypted snapshots of the complete
`fido_store` partition for restoration on **the same physical Cardputer ADV**.
The snapshot includes the wrapping secrets, resident registrations, stored
credential IDs, PIN records, counters and other SDK filesystem state. It does
not include Launcher, the partition table, the firmware image or NVS UI settings.

A compatible `fido_store` partition must already exist. Restoring does not create
partitions, format flash automatically or provision eFuses. If the whole flash
was erased, follow [the installation guide](INSTALL-LAUNCHER.md) to restore the
supported layout and application before importing the backup.

Host tests and a target build are not physical acceptance. Actual microSD I/O,
password entry on the keyboard, runtime/heap behavior, power-cut recovery and a
browser login after restoration must be validated on hardware before trusting
this feature with irreplaceable registrations. Never erase the only working
copy merely to try the feature.

## Save

1. Insert a FAT/FAT32 microSD card with enough free space for a new snapshot
   alongside the retained generations. The firmware never formats the card.
2. Open `Settings` -> `Backup & Restore` -> `Backup to SD`.
3. The device flushes storage and reboots into a pre-USB maintenance mode.
4. Enter a separate backup password and repeat it. Passwords accept 12 through
   64 printable ASCII characters, including spaces and shifted punctuation.
   Use a long, hard-to-guess password: the file permits offline guessing and
   cannot enforce a retry limit. This is not the WebAuthn PIN and does not add
   a startup password to ordinary use.
5. Confirm saving and leave power and the card connected until verification
   finishes. Press Enter at the result screen to reboot into normal operation.

Passwords are masked; `Del` erases the last character, `Fn+Del` cancels and
`Enter` accepts. Password entry expires after two minutes without input.
Passwords and derived encryption keys are not stored in NVS or written to SD.

The files are in the card root, with FAT 8.3 names:

- `FIDOBAK.CFB`: latest published snapshot.
- `FIDOBAK.OLD`: previous published snapshot, encrypted with the password used
  when that snapshot was made (which can differ from the newest password).
- `FIDOBAK.TMP`: in-progress encrypted output; it may be incomplete after an
  error. Restoration always verifies it fully, regardless of its name.

The new file is closed, synced, reopened and authenticated before publishing.
The current latest generation is moved to `.OLD` before `.TMP` becomes the new
latest. FAT directory updates are not power-loss atomic: retain independent
copies on another medium, not just multiple names on the same card.

The card is **not required for ordinary authentication**. Remove it after the
operation/reboot, and keep another copy of the encrypted file off the device.
Refresh backups after adding registrations or changing the PIN. A backup cannot
restore registrations created after it was saved.

## Restore

1. Install a compatible firmware and ensure the supported partition exists.
2. Insert the card and choose `Settings` -> `Backup & Restore` ->
   `Restore from SD`. A damaged key store instead exposes storage recovery
   before starting USB.
3. Select the latest, previous or temporary file and enter that file's password.
4. The firmware checks the entire encrypted file, its exact size/format and
   its device binding **before asking to replace current keys**. Wrong passwords,
   truncated/corrupt files and copies from another board are rejected.
5. Confirm replacement. Do not remove power or the card. The entire current
   FIDO state is replaced, not merged. The device checks flash writes and
   commits key pages last, then reboots after successful completion.

If an apply is interrupted, the persistent recovery guard prevents USB/CTAP
startup. The device asks to restore again; retain the card and password. It does
not silently generate another identity or resume using half-restored data.
There is no second flash bank: restoration is recoverable by retry, **not an
atomic rollback to the state before restoration**. Do not erase NVS or the key
store to bypass this guard.

If SD driver cleanup itself fails, the device reboots before another attempt
instead of reusing a possibly freed card handle. An active restore guard stays
set across that reboot.

Restoring rolls counters, PIN state, deletions and all other stored state back
to the snapshot. A deleted credential may reappear. Some relying parties can
flag an older signature counter as a cloned authenticator. A backup is not a
promise that every service accepts rollback; keep an independent recovery
method for important accounts.

## Format and security boundary

Format version 1 uses PBKDF2-HMAC-SHA256 with 600,000 iterations, a random
16-byte salt, AES-256-GCM, independent 4096-byte authenticated records and a
random nonce prefix. The authenticated header contains the format/size/KDF
constants and a public device-serial hash; each record also authenticates its
index. Headers are bounded before invoking the KDF. Plaintext is processed in
bounded RAM buffers, never a plaintext file on SD. The codec authenticates the
whole file before apply and reauthenticates every record during apply to detect
removable-media changes. A mid-apply media failure keeps the recovery guard set.

The device binding is a compatibility check, not an extra password or a secret.
Protect the file and password as authenticator key material. The firmware still
has no secure element, trusted boot or encrypted internal flash. A malicious
firmware on the Cardputer can capture the password and secrets. This mechanism
does not make the prototype resistant to physical compromise.
