"""CTAPHID session/reconnect regression cases against the host emulator.

These are synthetic HID report tests. A pass does not claim an electrical USB
reconnect test; that remains a hardware gate.
"""
from __future__ import annotations

import os
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from protocol_test import CtapHidTcp, Device, Emulator  # noqa: E402

INIT = CtapHidTcp.INIT
CBOR = CtapHidTcp.CBOR
PING = 0x01
BROADCAST = 0xFFFFFFFF
ERR_INVALID_CHANNEL = 0x0B
ERR_TIMEOUT = 0x05


def expect_error(hid: CtapHidTcp, cid: int, cmd: int, data: bytes,
                 code: int) -> None:
    try:
        hid.call(cmd, data, cid=cid)
    except RuntimeError as exc:
        if f"{code:#x}" not in str(exc):
            raise AssertionError(f"expected error {code:#x}, got {exc}") from exc
        return
    raise AssertionError(f"expected CTAPHID error {code:#x}")


def get_info(hid: CtapHidTcp) -> bytes:
    device = Device(hid, 0x04)
    payload = device.call(CBOR, b"\x04")
    assert payload.startswith(b"\xa1") or payload, "empty GetInfo response"
    return payload


def run_reconnect_suite(emulator_path: pathlib.Path, cycles: int = 100) -> None:
    emulator = Emulator(emulator_path)
    try:
        emulator.start()

        hid = CtapHidTcp()
        info = hid.init()
        assert info["cid"] not in (0, BROADCAST)
        assert get_info(hid)
        print("PASS: broadcast INIT allocates a channel and GetInfo responds")

        nonce = os.urandom(8)
        _, cmd, payload = hid.call(INIT, nonce, cid=hid.cid)
        assert (cmd & 0x7F) == INIT
        assert payload[:8] == nonce
        assert int.from_bytes(payload[8:12], "big") == hid.cid
        print("PASS: allocated-CID INIT keeps the same channel and echoes nonce")

        expect_error(hid, 0, PING, b"bad", ERR_INVALID_CHANNEL)
        expect_error(hid, BROADCAST, PING, b"bad", ERR_INVALID_CHANNEL)
        print("PASS: invalid channel IDs are rejected")

        hid.send_initial(hid.cid, CBOR, 8000, b"\x04")
        _, rcmd, rpayload = hid.read_message()
        assert (rcmd & 0x7F) == CtapHidTcp.ERROR, f"expected length error, got {rcmd:#x}"
        assert rpayload[0] == 0x03, f"expected INVALID_LEN, got {rpayload[0]:#x}"
        assert get_info(hid), "channel unusable after length error"
        print("PASS: oversized HID reports are rejected")

        _, _, malformed = hid.call(CBOR, b"\x01\xff")
        assert len(malformed) == 1 and malformed[0] != 0, malformed
        assert get_info(hid), "channel unusable after malformed CBOR"
        print("PASS: malformed CBOR returns a protocol error and recovers")

        hid.send_initial(hid.cid, CBOR, 100, b"\x04" + b"\xa0" * 56)
        hid.send_initial(hid.cid, 0x11, 0, b"")
        time.sleep(0.05)
        assert get_info(hid), "channel unusable after cancel"
        print("PASS: CTAPHID cancel clears a partial request")

        # Start a fragmented request, then let it time out without its tail.
        hid.send_initial(hid.cid, CBOR, 100, b"\x04" + b"\xa0" * 56)
        time.sleep(0.7)
        _, rcmd, rpayload = hid.read_message()
        assert (rcmd & 0x7F) == CtapHidTcp.ERROR, f"expected timeout, got {rcmd:#x}"
        assert rpayload[0] == ERR_TIMEOUT, f"expected timeout, got {rpayload[0]:#x}"
        assert get_info(hid), "channel unusable after fragment timeout"
        print("PASS: incomplete fragments expire and the channel recovers")

        # Close a channel in the middle of a fragmented request. The next
        # session must start clean rather than consuming the stale tail.
        stale = CtapHidTcp()
        stale.init()
        stale.send_initial(stale.cid, CBOR, 100, b"\x04" + b"\xa0" * 56)
        stale.close()
        fresh = CtapHidTcp()
        fresh.init()
        assert get_info(fresh)
        fresh.close()
        hid.close()
        print("PASS: disconnect discards the stale request")

        for index in range(cycles):
            cycle = CtapHidTcp()
            cycle.init()
            assert get_info(cycle), f"GetInfo failed in cycle {index}"
            cycle.close()
        print(f"PASS: {cycles} request/INIT/reconnect cycles")
    finally:
        emulator.stop()


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", type=pathlib.Path,
                        default=pathlib.Path("build/emulator/pico_fido"))
    parser.add_argument("--cycles", type=int, default=100)
    args = parser.parse_args()
    if not args.emulator.is_file():
        print(f"FAIL: emulator not found: {args.emulator}", flush=True)
        return 1
    run_reconnect_suite(args.emulator, args.cycles)
    print("PASS: CTAPHID reconnect/session suite")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
