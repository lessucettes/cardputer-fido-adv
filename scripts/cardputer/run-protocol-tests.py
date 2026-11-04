#!/usr/bin/env python3
"""Run the isolated CTAP2 protocol suite against the host emulator.

The emulator is a report/session harness: it exercises the pinned CTAP stack
over TCP, not the Cardputer USB peripheral or its physical keyboard/display.

One-time setup (needs network access):

    python3 -m venv .venv-protocol
    .venv-protocol/bin/pip install -r tests/cardputer/requirements-protocol.txt

Then:

    .venv-protocol/bin/python scripts/cardputer/run-protocol-tests.py \
        --reconnect-cases
"""
from __future__ import annotations

import argparse
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "cardputer"))

try:
    from protocol_test import run_pin_suite, run_protocol_suite
    from reconnect_test import run_reconnect_suite
except ModuleNotFoundError as exc:  # pragma: no cover - operator guidance
    if exc.name == "fido2":
        print(
            "error: the fido2 package is not installed for this interpreter.\n"
            "create the pinned test environment:\n"
            "  python3 -m venv .venv-protocol\n"
            "  .venv-protocol/bin/pip install -r "
            "tests/cardputer/requirements-protocol.txt",
            file=sys.stderr,
        )
        raise SystemExit(2)
    raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--emulator", type=pathlib.Path,
        default=ROOT / "build" / "emulator" / "pico_fido",
    )
    parser.add_argument(
        "--reconnect-cases", action="store_true",
        help="exercise 20 additional CTAPHID INIT/reconnect cycles",
    )
    parser.add_argument(
        "--keep-workdir", action="store_true",
        help="keep the temporary emulator flash/OTP directory for inspection",
    )
    args = parser.parse_args()
    if not args.emulator.is_file():
        print(f"error: emulator not found: {args.emulator}", file=sys.stderr)
        print(
            "build it first with:\n"
            "  cmake -S . -B build/emulator -G Ninja "
            "-DENABLE_EMULATION=ON -DENABLE_DETERMINISTIC_OTP=ON\n"
            "  cmake --build build/emulator --target pico_fido",
            file=sys.stderr,
        )
        return 1
    run_protocol_suite(
        args.emulator,
        reconnect_cases=False,
        keep_workdir=args.keep_workdir,
    )
    run_pin_suite(args.emulator, keep_workdir=args.keep_workdir)
    if args.reconnect_cases:
        run_reconnect_suite(args.emulator)
    print("PASS: isolated CTAP2 protocol suite")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
