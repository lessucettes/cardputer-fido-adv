"""Isolated CTAP2 protocol checks against the host emulator.

Requires the fido2 Python package (pinned in the command printed by the
launcher). This is a report/session test suite, not an electrical USB test.
"""
from __future__ import annotations

import hashlib
import os
import pathlib
import shutil
import socket
import struct
import subprocess
import tempfile
import time

try:
    from fido2.ctap import CtapError
    from fido2.ctap2 import Ctap2
    from fido2.ctap2.credman import CredentialManagement
    from fido2.ctap2.pin import ClientPin, PinProtocolV1
except ModuleNotFoundError:  # optional protocol-test dependency
    CtapError = Ctap2 = CredentialManagement = ClientPin = PinProtocolV1 = None

RP = {"id": "example.com", "name": "Example RP"}
KEY_PARAMS = [{"type": "public-key", "alg": -7}]


class CtapHidTcp:
    INIT = 0x06
    CBOR = 0x10
    ERROR = 0x3F
    KEEPALIVE = 0x3B

    def __init__(self, host: str = "127.0.0.1", port: int = 35962,
                 timeout: float = 10.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.cid = 0xFFFFFFFF

    def close(self) -> None:
        self.sock.close()

    def _send(self, cid: int, cmd: int, data: bytes) -> None:
        remaining = bytes(data)
        total = len(remaining)
        first = True
        seq = 0
        while True:
            if first:
                chunk = remaining[:57]
                remaining = remaining[57:]
                header = struct.pack(">IBH", cid, cmd | 0x80, total)
            else:
                chunk = remaining[:59]
                remaining = remaining[59:]
                header = struct.pack(">IB", cid, seq & 0x7F)
                seq += 1
            frame = header + chunk
            frame += b"\x00" * (64 - len(frame))
            self.sock.sendall(struct.pack(">H", len(frame)) + frame)
            first = False
            if not remaining:
                break

    def send_initial(self, cid: int, cmd: int, total: int, chunk: bytes) -> None:
        header = struct.pack(">IBH", cid, cmd | 0x80, total)
        frame = header + chunk[:57]
        frame += b"\x00" * (64 - len(frame))
        self.sock.sendall(struct.pack(">H", len(frame)) + frame)

    def send_cont(self, cid: int, seq: int, chunk: bytes) -> None:
        header = struct.pack(">IB", cid, seq & 0x7F)
        frame = header + chunk[:59]
        frame += b"\x00" * (64 - len(frame))
        self.sock.sendall(struct.pack(">H", len(frame)) + frame)

    def _recv_exact(self, count: int) -> bytes:
        chunks = []
        while count:
            chunk = self.sock.recv(count)
            if not chunk:
                raise EOFError("emulator closed the connection")
            chunks.append(chunk)
            count -= len(chunk)
        return b"".join(chunks)

    def _recv_frame(self) -> bytes:
        length = struct.unpack(">H", self._recv_exact(2))[0]
        return self._recv_exact(length)

    def read_message(self) -> tuple[int, int, bytes]:
        frame = self._recv_frame()
        cid, cmd, bcnt = struct.unpack(">IBH", frame[:7])
        if not (cmd & 0x80):
            raise ValueError("expected initial frame")
        payload = frame[7:7 + bcnt]
        while len(payload) < bcnt:
            cont = self._recv_frame()
            _, seq = struct.unpack(">IB", cont[:5])
            if seq & 0x80:
                raise ValueError("expected continuation frame")
            payload += cont[5:5 + min(59, bcnt - len(payload))]
        return cid, cmd, payload

    def call(self, cmd: int, data: bytes, cid: int | None = None) -> tuple[int, int, bytes]:
        self._send(self.cid if cid is None else cid, cmd, data)
        while True:
            rcid, rcmd, payload = self.read_message()
            if (rcmd & 0x7F) == self.KEEPALIVE:
                continue
            if (rcmd & 0x7F) == self.ERROR:
                raise RuntimeError(f"CTAPHID error {payload[0]:#x}")
            return rcid, rcmd, payload

    def init(self) -> dict:
        nonce = os.urandom(8)
        _, cmd, payload = self.call(self.INIT, nonce, cid=0xFFFFFFFF)
        if (cmd & 0x7F) != self.INIT or payload[:8] != nonce:
            raise RuntimeError("INIT response mismatch")
        self.cid = struct.unpack(">I", payload[8:12])[0]
        return {
            "cid": self.cid,
            "protocol": payload[12],
            "version": payload[13:16],
            "capabilities": payload[16],
        }


class Device:
    def __init__(self, hid: CtapHidTcp, capabilities: int):
        self.hid = hid
        self.capabilities = capabilities
        self._cid = hid.cid

    def call(self, cmd, data, event=None, on_keepalive=None, cid=None):
        del event, on_keepalive
        _, rcmd, payload = self.hid.call(cmd, data, cid or self._cid)
        if (rcmd & 0x7F) != (cmd & 0x7F):
            raise RuntimeError(f"unexpected response {rcmd:#x} for {cmd:#x}")
        return payload

    def send(self, cmd, data, event=None, on_keepalive=None, cid=None):
        del event, on_keepalive
        self.hid._send(cid or self._cid, cmd, data)


class Emulator:
    def __init__(self, path: pathlib.Path, workdir: pathlib.Path | None = None):
        self.path = path.resolve()
        self.workdir = workdir or pathlib.Path(tempfile.mkdtemp(prefix="cardputer-ctap-"))
        self.process: subprocess.Popen | None = None

    def start(self) -> None:
        env = dict(os.environ)
        env["PICO_KEYS_OTP_FILE"] = str(self.workdir / "otp.bin")
        log = open(self.workdir / "emulator.log", "ab")
        self.process = subprocess.Popen(
            [str(self.path)], cwd=self.workdir, env=env,
            stdout=log, stderr=subprocess.STDOUT,
        )
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            try:
                probe = socket.create_connection(("127.0.0.1", 35962), timeout=0.2)
                probe.close()
                return
            except OSError:
                if self.process.poll() is not None:
                    raise RuntimeError("emulator exited before listening")
                time.sleep(0.05)
        raise RuntimeError("emulator did not listen on 127.0.0.1:35962")

    def stop(self) -> None:
        if self.process is not None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
            self.process = None


def connect() -> tuple[CtapHidTcp, Ctap2]:
    if Ctap2 is None:
        raise RuntimeError(
            "the fido2 package is required for protocol tests; install "
            "tests/cardputer/requirements-protocol.txt"
        )
    hid = CtapHidTcp()
    info = hid.init()
    ctap = Ctap2(Device(hid, info["capabilities"]))
    return hid, ctap


def create_user(name: str) -> dict:
    return {"id": name.encode(), "name": name, "displayName": name.title()}


def run_protocol_suite(emulator_path: pathlib.Path,
                       reconnect_cases: bool = False,
                       keep_workdir: bool = False) -> None:
    emulator = Emulator(emulator_path)
    try:
        emulator.start()
        hid, ctap = connect()
        assert "FIDO_2_0" in ctap.info.versions, "FIDO_2_0 missing"
        assert ctap.info.options.get("rk") is True, "resident keys unavailable"
        print("PASS: GetInfo reports CTAP2 and resident-key support")

        alice = create_user("alice")
        mc = ctap.make_credential(
            hashlib.sha256(b"register-alice").digest(), RP, alice, KEY_PARAMS,
            options={"rk": True},
        )
        cred_id = bytes(mc.auth_data.credential_data.credential_id)
        assert mc.auth_data.rp_id_hash == hashlib.sha256(RP["id"].encode()).digest()
        print("PASS: MakeCredential returns a resident credential")

        cdh = hashlib.sha256(b"login-alice").digest()
        assertion = ctap.get_assertion(
            RP["id"], cdh, allow_list=[{"type": "public-key", "id": cred_id}]
        )
        assertion.verify(cdh, mc.auth_data.credential_data.public_key)
        print("PASS: GetAssertion signature verifies")

        try:
            ctap.make_credential(
                hashlib.sha256(b"register-alice-again").digest(), RP, alice,
                KEY_PARAMS,
                exclude_list=[{"type": "public-key", "id": cred_id}],
            )
        except CtapError as exc:
            assert exc.code == CtapError.ERR.CREDENTIAL_EXCLUDED, exc
        else:
            raise AssertionError("exclusion list was ignored")
        print("PASS: existing-credential exclusion rejects a duplicate")

        bob = create_user("bob")
        ctap.make_credential(
            hashlib.sha256(b"register-bob").digest(), RP, bob, KEY_PARAMS,
            options={"rk": True},
        )
        assertions = ctap.get_assertions(RP["id"], hashlib.sha256(b"login-both").digest())
        assert len(assertions) == 2, f"expected two accounts, got {len(assertions)}"
        ids = {item.user["id"] for item in assertions if item.user}
        assert ids == {b"alice", b"bob"}, ids
        print("PASS: two accounts under one RP are both discoverable")

        eve = create_user("eve")
        eve_mc = ctap.make_credential(
            hashlib.sha256(b"register-eve-nonresident").digest(), RP, eve,
            KEY_PARAMS, options={"rk": False},
        )
        eve_id = bytes(eve_mc.auth_data.credential_data.credential_id)
        discovered = ctap.get_assertions(
            RP["id"], hashlib.sha256(b"login-eve-discovery").digest()
        )
        assert all(item.user["id"] != b"eve" for item in discovered if item.user), \
            "nonresident credential was discoverable"
        eve_cdh = hashlib.sha256(b"login-eve").digest()
        eve_assertion = ctap.get_assertion(
            RP["id"], eve_cdh, allow_list=[{"type": "public-key", "id": eve_id}]
        )
        eve_assertion.verify(eve_cdh, eve_mc.auth_data.credential_data.public_key)
        print("PASS: nonresident credential signs only when explicitly listed")

        try:
            ctap.make_credential(
                hashlib.sha256(b"register-eve-again").digest(), RP, eve,
                KEY_PARAMS,
                exclude_list=[{"type": "public-key", "id": eve_id}],
            )
        except CtapError as exc:
            assert exc.code == CtapError.ERR.CREDENTIAL_EXCLUDED, exc
        else:
            raise AssertionError("nonresident exclusion list was ignored")
        print("PASS: nonresident exclusion is rejected")

        wrong_rejected = False
        try:
            ctap.get_assertion("wrong.example", hashlib.sha256(b"wrong-rp").digest())
        except Exception:
            wrong_rejected = True
        assert wrong_rejected, "wrong RP unexpectedly returned an assertion"
        print("PASS: wrong RP is rejected")

        ctap.get_assertion(
            RP["id"], hashlib.sha256(b"login-after-error").digest(),
            allow_list=[{"type": "public-key", "id": cred_id}]
        )
        print("PASS: session remains usable after a protocol error")
        hid.close()

        emulator.stop()
        emulator.start()
        hid, ctap = connect()
        cdh = hashlib.sha256(b"login-after-restart").digest()
        assertion = ctap.get_assertion(
            RP["id"], cdh, allow_list=[{"type": "public-key", "id": cred_id}]
        )
        assertion.verify(cdh, mc.auth_data.credential_data.public_key)
        hid.close()
        print("PASS: credential survives emulator restart")

        if reconnect_cases:
            for index in range(20):
                hid = CtapHidTcp()
                info = hid.init()
                assert info["cid"] != 0
                device = Device(hid, info["capabilities"])
                info_payload = device.call(0x10, b"\x04")
                assert info_payload, "GetInfo after reconnect"
                hid.close()
            print("PASS: 20 CTAPHID reconnect/INIT cycles")
    finally:
        emulator.stop()
        if not keep_workdir:
            shutil.rmtree(emulator.workdir, ignore_errors=True)


def run_pin_suite(emulator_path: pathlib.Path,
                  keep_workdir: bool = False) -> None:
    emulator = Emulator(emulator_path)
    try:
        emulator.start()
        hid, ctap = connect()

        carol = ctap.make_credential(
            hashlib.sha256(b"pin-register-carol").digest(), RP,
            create_user("carol"), KEY_PARAMS, options={"rk": True},
        )
        dave = ctap.make_credential(
            hashlib.sha256(b"pin-register-dave").digest(), RP,
            create_user("dave"), KEY_PARAMS, options={"rk": True},
        )
        carol_id = bytes(carol.auth_data.credential_data.credential_id)
        dave_id = bytes(dave.auth_data.credential_data.credential_id)

        pin = ClientPin(ctap, PinProtocolV1())
        pin.set_pin("123456")
        try:
            pin.get_pin_token("000000", permissions=ClientPin.PERMISSION.CREDENTIAL_MGMT)
        except CtapError as exc:
            assert exc.code == CtapError.ERR.PIN_INVALID, exc
        else:
            raise AssertionError("wrong PIN was accepted")
        retries = pin.get_pin_retries()
        assert retries[0] == 7, retries
        print("PASS: PIN is set and a wrong PIN consumes one retry")

        token = pin.get_pin_token(
            "123456", permissions=ClientPin.PERMISSION.CREDENTIAL_MGMT
        )
        cm = CredentialManagement(ctap, PinProtocolV1(), token)
        assert cm.get_metadata()[1] == 2
        rps = cm.enumerate_rps()
        assert any(item[3]["id"] == RP["id"] for item in rps), rps
        creds = cm.enumerate_creds(hashlib.sha256(RP["id"].encode()).digest())
        assert len(creds) == 2, len(creds)
        print("PASS: credential management enumerates RPs and credentials")

        cm.delete_cred({"type": "public-key", "id": carol_id})
        assert cm.get_metadata()[1] == 1
        remaining = cm.enumerate_creds(hashlib.sha256(RP["id"].encode()).digest())
        assert len(remaining) == 1, len(remaining)
        try:
            ctap.get_assertion(
                RP["id"], hashlib.sha256(b"deleted-login").digest(),
                allow_list=[{"type": "public-key", "id": carol_id}],
            )
        except CtapError as exc:
            assert exc.code == CtapError.ERR.NO_CREDENTIALS, exc
        else:
            raise AssertionError("deleted credential still asserted")
        print("PASS: resident credential deletion removes only the target")

        cdh = hashlib.sha256(b"remaining-login").digest()
        ga_token = pin.get_pin_token(
            "123456", permissions=ClientPin.PERMISSION.GET_ASSERTION
        )
        assertion = ctap.get_assertion(
            RP["id"], cdh,
            allow_list=[{"type": "public-key", "id": dave_id}],
            pin_uv_param=PinProtocolV1().authenticate(ga_token, cdh),
            pin_uv_protocol=1,
        )
        assertion.verify(cdh, dave.auth_data.credential_data.public_key)
        print("PASS: remaining credential signs after PIN authentication")

        pin.change_pin("123456", "654321")
        try:
            pin.get_pin_token("123456", permissions=ClientPin.PERMISSION.CREDENTIAL_MGMT)
        except CtapError as exc:
            assert exc.code == CtapError.ERR.PIN_INVALID, exc
        else:
            raise AssertionError("old PIN survived change_pin")
        print("PASS: PIN change invalidates the old PIN")
        hid.close()

        emulator.stop()
        emulator.start()
        hid, ctap = connect()
        pin = ClientPin(ctap, PinProtocolV1())
        token = pin.get_pin_token(
            "654321", permissions=ClientPin.PERMISSION.CREDENTIAL_MGMT
        )
        cm = CredentialManagement(ctap, PinProtocolV1(), token)
        assert cm.get_metadata()[1] == 1
        assert len(cm.enumerate_creds(hashlib.sha256(RP["id"].encode()).digest())) == 1
        hid.close()
        print("PASS: changed PIN and deleted credential survive restart")
    finally:
        emulator.stop()
        if not keep_workdir:
            shutil.rmtree(emulator.workdir, ignore_errors=True)


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", type=pathlib.Path,
                        default=pathlib.Path("build/emulator/pico_fido"))
    parser.add_argument("--reconnect-cases", action="store_true")
    args = parser.parse_args()
    if not args.emulator.is_file():
        print(f"FAIL: emulator not found: {args.emulator}", flush=True)
        return 1
    run_protocol_suite(args.emulator, args.reconnect_cases)
    print("PASS: isolated CTAP2 protocol suite")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
