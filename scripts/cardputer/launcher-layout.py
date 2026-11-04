#!/usr/bin/env python3
"""Read-only planner for a dedicated fido_store partition.

Input is a backed-up IDF partition-table binary and the actual flash size.
The tool never writes a device, never moves existing partitions and never
overwrites a bootloader, app, NVS or otadata region.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import struct

MAGIC = 0xAA50
END_MAGIC = 0xEBEB
ENTRY_SIZE = 32
STORE_SIZE = 0x102000
ALIGN = 0x1000
TABLE_END = 0x9000
LABEL_LEN = 16


class LayoutError(ValueError):
    pass


def _u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def parse_table(data: bytes) -> list[dict]:
    if len(data) < ENTRY_SIZE:
        raise LayoutError("partition table is too short")
    entries: list[dict] = []
    body = bytearray()
    position = 0
    while position + ENTRY_SIZE <= len(data):
        magic = struct.unpack_from("<H", data, position)[0]
        if magic == END_MAGIC:
            stored = data[position + ENTRY_SIZE : position + ENTRY_SIZE + 16]
            if len(stored) != 16:
                raise LayoutError("missing partition table MD5")
            if hashlib.md5(bytes(body)).digest() != stored:
                raise LayoutError("partition table MD5 mismatch")
            return entries
        if magic != MAGIC:
            raise LayoutError(f"bad partition entry magic at {position:#x}")
        entry = data[position : position + ENTRY_SIZE]
        body.extend(entry)
        raw_label = entry[12:28].split(b"\x00", 1)[0]
        try:
            label = raw_label.decode("ascii")
        except UnicodeDecodeError as exc:
            raise LayoutError("non-ASCII partition label") from exc
        entries.append(
            {
                "label": label,
                "type": entry[2],
                "subtype": entry[3],
                "offset": _u32(entry, 4),
                "size": _u32(entry, 8),
                "flags": _u32(entry, 28),
            }
        )
        position += ENTRY_SIZE
    raise LayoutError("partition table has no end marker")


def validate_entries(entries: list[dict], flash_size: int) -> None:
    labels = set()
    ranges = []
    for entry in entries:
        if entry["label"] in labels:
            raise LayoutError(f"duplicate label: {entry['label']}")
        labels.add(entry["label"])
        if entry["size"] == 0:
            raise LayoutError(f"zero-size partition: {entry['label']}")
        end = entry["offset"] + entry["size"]
        if end < entry["offset"] or end > flash_size:
            raise LayoutError(f"partition outside flash: {entry['label']}")
        ranges.append((entry["offset"], end, entry["label"]))
    ranges.sort()
    for previous, current in zip(ranges, ranges[1:]):
        if current[0] < previous[1]:
            raise LayoutError(
                f"overlap between {previous[2]} and {current[2]}"
            )


def plan_layout(entries: list[dict], flash_size: int) -> dict:
    if flash_size <= TABLE_END:
        raise LayoutError("flash is too small for a partition table")
    validate_entries(entries, flash_size)
    for entry in entries:
        if entry["label"] == "fido_store":
            valid = (
                entry["type"] == 0x40
                and entry["subtype"] == 0x01
                and entry["size"] == STORE_SIZE
                and entry["offset"] % ALIGN == 0
            )
            return {
                "action": "keep" if valid else "invalid-existing",
                "flash_size": flash_size,
                "offset": entry["offset"],
                "size": entry["size"],
                "existing": entries,
                "proposed": entries,
            }

    cursor = TABLE_END
    for entry in sorted(entries, key=lambda item: item["offset"]):
        candidate = (cursor + ALIGN - 1) & ~(ALIGN - 1)
        if candidate + STORE_SIZE <= entry["offset"]:
            proposed = list(entries)
            proposed.append(
                {
                    "label": "fido_store",
                    "type": 0x40,
                    "subtype": 0x01,
                    "offset": candidate,
                    "size": STORE_SIZE,
                    "flags": 0,
                }
            )
            return {
                "action": "create",
                "flash_size": flash_size,
                "offset": candidate,
                "size": STORE_SIZE,
                "existing": entries,
                "proposed": proposed,
            }
        cursor = max(cursor, entry["offset"] + entry["size"])
    candidate = (cursor + ALIGN - 1) & ~(ALIGN - 1)
    if candidate + STORE_SIZE > flash_size:
        raise LayoutError("no free aligned region for fido_store")
    proposed = list(entries)
    proposed.append(
        {
            "label": "fido_store",
            "type": 0x40,
            "subtype": 0x01,
            "offset": candidate,
            "size": STORE_SIZE,
            "flags": 0,
        }
    )
    return {
        "action": "create",
        "flash_size": flash_size,
        "offset": candidate,
        "size": STORE_SIZE,
        "existing": entries,
        "proposed": proposed,
    }


def to_csv(entries: list[dict]) -> str:
    lines = ["# Name, Type, SubType, Offset, Size, Flags"]
    for entry in sorted(entries, key=lambda item: item["offset"]):
        lines.append(
            f"{entry['label']}, {entry['type']:#04x}, {entry['subtype']:#04x}, "
            f"{entry['offset']:#08x}, {entry['size']:#08x}, {entry['flags']:#x}"
        )
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog="This command performs no device or partition-table writes.",
    )
    parser.add_argument("--table", required=True, type=pathlib.Path,
                        help="backed-up partition-table binary (read-only)")
    parser.add_argument("--flash-size", required=True, type=lambda value: int(value, 0),
                        help="actual flash capacity in bytes, e.g. 0x800000")
    parser.add_argument("--json-out", type=pathlib.Path,
                        help="write the proposed report here (optional)")
    parser.add_argument("--csv-out", type=pathlib.Path,
                        help="write the proposed CSV here (optional)")
    args = parser.parse_args(argv)

    try:
        data = args.table.read_bytes()
        entries = parse_table(data)
        report = plan_layout(entries, args.flash_size)
    except (OSError, LayoutError) as exc:
        print(f"FAIL: {exc}")
        return 1

    report["table"] = str(args.table)
    report["read_only"] = True
    text = json.dumps(report, indent=2)
    print(text)
    if args.json_out is not None:
        args.json_out.write_text(text + "\n", encoding="utf-8")
    if args.csv_out is not None:
        args.csv_out.write_text(to_csv(report["proposed"]), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
