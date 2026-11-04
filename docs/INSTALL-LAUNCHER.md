# Launcher installation and updates

## Current status

The app-only package builds and its policy checks pass. The Launcher
first-install procedure and the claim that a Launcher update preserves a
custom type-`0x40` `fido_store` partition have **not** been demonstrated on a
real device yet. Do not treat this document as a hardware guarantee.

## Required layout

The firmware expects exactly one partition:

| Field | Value |
|---|---|
| Label | `fido_store` |
| Type | `0x40` |
| Subtype | `0x01` |
| Size | `0x102000` (1,056,768 bytes) |
| Alignment | `0x1000` |

The two key pages occupy the first `0x2000` bytes. The remaining `0x100000`
bytes are the SDK filesystem. The firmware refuses to start with a missing,
wrong-type, wrong-size, overlapping or out-of-flash partition.

`config/cardputer_adv/partitions-reference.csv` is a **build fixture**. It is
not the partition table of any specific board and must never be flashed as-is.

## Before any write

1. Obtain the approved test device and explicit approval for a one-time
   partition-table operation, if one is needed.
2. Read the actual flash size and the real partition table with read-only
   `esptool`/`espefuse` commands. Never select the first detected serial port.
3. Create a full flash backup outside this repository and verify its size and
   hash.
4. Run the read-only planner against the backed-up table:

```sh
python3 scripts/cardputer/launcher-layout.py \
  --table /path/to/backed-up-partition-table.bin \
  --flash-size 0x800000
```

The tool proposes an aligned free region without moving existing partitions.
It refuses when no region fits and performs no device writes.

5. Review the old and proposed tables side by side. If there is no safe free
   region, stop. Do not shrink apps/data or replace Launcher automatically.

## First install

Ordinary Launcher app installation creates app/FAT/SPIFFS partitions, not this
custom FIDO partition. A first install therefore requires either:

- a tested Launcher profile that can create `fido_store`, or
- a separately approved one-time partition-table provisioning step generated
  from the real backup.

After the partition exists, install only the application image through the
supported Launcher flow. The normal package contains:

- `Cardputer-FIDO-ADV.bin`
- `manifest.json`
- `INSTALL.txt`

It does **not** contain a bootloader, partition table, blank credential
database, eFuse script or SD-card data.

## Updates and other-app round trip

Normal firmware updates must be app-only through Launcher. Before claiming
support, perform all of the following on the recorded candidate:

1. Register throwaway credentials and verify signed assertions.
2. Update the app through the supported Launcher procedure.
3. Launch another approved benign app and return.
4. Reboot and authenticate with the **same** credential identity.
5. Confirm the key-store bytes/profile were preserved.

A bootable app image alone is not evidence of Launcher compatibility. Arbitrary
Launcher repartition operations remain explicitly destructive and unsupported.

## Recovery

If the device does not enumerate after an update, stop and use the verified
full-flash backup and documented recovery procedure. Do not regenerate keys or
erase `fido_store` to make the device boot; that destroys the credential
identity.
