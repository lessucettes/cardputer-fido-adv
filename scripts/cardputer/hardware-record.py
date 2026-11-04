#!/usr/bin/env python3
"""Create a secret-free hardware validation worksheet.

The record accepts only provenance, device identity, hashes and PASS/FAIL
scenario results. It refuses to run without a revision and a device label and
never accepts key material, flash dumps or account names.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

SCENARIO_RE = re.compile(r"^[a-z0-9][a-z0-9 _-]*=(PASS|FAIL|BLOCKED|UNTESTED)$")


def parse_scenarios(values: list[str]) -> dict[str, str]:
    scenarios: dict[str, str] = {}
    for value in values:
        if not SCENARIO_RE.match(value):
            raise ValueError(f"invalid scenario result: {value}")
        name, result = value.split("=", 1)
        scenarios[name] = result
    return scenarios


def build_record(args: argparse.Namespace) -> dict:
    if not args.revision.strip():
        raise ValueError("candidate revision is required")
    if not args.device.strip():
        raise ValueError("approved device label is required")
    return {
        "candidate_revision": args.revision,
        "device": args.device,
        "board": "M5Stack Cardputer ADV",
        "app_sha256": args.app_sha256 or "UNRECORDED",
        "sdk_versions": {
            "pico_fido": "99337e1204348921452a3ce6613e8e17617d4c7d",
            "pico_keys_sdk": "d8fce97ca761eb763869c77cc4d3b6fd0be1caf5",
            "idf": args.idf or "UNRECORDED",
        },
        "launcher_version": args.launcher or "UNRECORDED",
        "host_platforms": args.host or [],
        "efuse_before": args.efuse_before or "NOT READ",
        "efuse_after": args.efuse_after or "NOT READ",
        "scenarios": parse_scenarios(args.scenario),
        "secret_policy": "No keys, PINs, account names, raw flash or dumps.",
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog="Manual hardware approval is still required before any write.",
    )
    parser.add_argument("--revision", required=True,
                        help="candidate Git revision or package revision")
    parser.add_argument("--device", required=True,
                        help="approved test-device label (never a secret)")
    parser.add_argument("--app-sha256", help="packaged application SHA-256")
    parser.add_argument("--idf", help="ESP-IDF version")
    parser.add_argument("--launcher", help="installed Launcher version")
    parser.add_argument("--host", action="append", default=[],
                        help="tested OS/browser pair, repeatable")
    parser.add_argument("--efuse-before", help="read-only eFuse summary")
    parser.add_argument("--efuse-after", help="read-only eFuse summary")
    parser.add_argument("--scenario", action="append", default=[],
                        help="name=PASS|FAIL|BLOCKED|UNTESTED, repeatable")
    parser.add_argument("--output", type=pathlib.Path,
                        help="write JSON here; stdout otherwise")
    args = parser.parse_args(argv)
    try:
        record = build_record(args)
    except ValueError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    text = json.dumps(record, indent=2) + "\n"
    if args.output is None:
        print(text, end="")
    else:
        args.output.write_text(text, encoding="utf-8")
        print(f"WROTE: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
