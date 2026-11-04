#!/usr/bin/env python3
"""Localhost-only WebAuthn fixture for the Cardputer ADV hardware gate.

The server binds loopback, issues one-time challenges and checks the
clientData origin/challenge plus the RP ID hash in authenticator data. It does
not contact external sites and does not claim full signature verification.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import ipaddress
import json
import pathlib
import secrets
import sys
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = pathlib.Path(__file__).resolve().parent
RP_ID = "localhost"
ORIGIN = "http://localhost:8765"
CHALLENGES: dict[str, bytes] = {}


def b64url_decode(value: str) -> bytes:
    padded = value + "=" * (-len(value) % 4)
    return base64.urlsafe_b64decode(padded)


def b64url_encode(value: bytes) -> str:
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode("ascii")


class Handler(BaseHTTPRequestHandler):
    server_version = "CardputerFidoFixture/1.0"

    def log_message(self, format: str, *args) -> None:
        sys.stderr.write("%s - %s\n" % (self.address_string(), format % args))

    def _json(self, status: HTTPStatus, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802
        if self.path in ("/", "/index.html"):
            body = (HERE / "index.html").read_bytes()
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == "/challenge":
            challenge_id = secrets.token_urlsafe(12)
            challenge = secrets.token_bytes(32)
            CHALLENGES[challenge_id] = challenge
            if len(CHALLENGES) > 64:
                CHALLENGES.clear()
            self._json(
                HTTPStatus.OK,
                {
                    "id": challenge_id,
                    "challenge": b64url_encode(challenge),
                    "rpId": RP_ID,
                    "origin": ORIGIN,
                },
            )
            return
        self._json(HTTPStatus.NOT_FOUND, {"ok": False, "reason": "not found"})

    def do_POST(self) -> None:  # noqa: N802
        if self.path != "/verify":
            self._json(HTTPStatus.NOT_FOUND, {"ok": False, "reason": "not found"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            payload = json.loads(self.rfile.read(length))
            challenge_id = payload["challengeId"]
            expected = CHALLENGES.pop(challenge_id)
            kind = payload["kind"]
            credential = payload["credential"]
            client_data = json.loads(b64url_decode(credential["response"]["clientDataJSON"]))
        except (KeyError, ValueError, TypeError, json.JSONDecodeError) as exc:
            self._json(HTTPStatus.BAD_REQUEST, {"ok": False, "reason": f"malformed: {exc}"})
            return

        expected_type = "webauthn.create" if kind == "create" else "webauthn.get"
        if client_data.get("type") != expected_type:
            self._json(HTTPStatus.BAD_REQUEST, {"ok": False, "reason": "wrong clientData type"})
            return
        if b64url_decode(client_data.get("challenge", "")) != expected:
            self._json(HTTPStatus.BAD_REQUEST, {"ok": False, "reason": "challenge mismatch"})
            return
        if client_data.get("origin") != ORIGIN:
            self._json(HTTPStatus.BAD_REQUEST, {"ok": False, "reason": "origin mismatch"})
            return

        if kind == "get":
            auth_data = b64url_decode(credential["response"]["authenticatorData"])
            if len(auth_data) < 32 or auth_data[:32] != hashlib.sha256(RP_ID.encode()).digest():
                self._json(HTTPStatus.BAD_REQUEST, {"ok": False, "reason": "RP ID hash mismatch"})
                return

        self._json(
            HTTPStatus.OK,
            {
                "ok": True,
                "kind": kind,
                "credentialId": credential.get("id"),
                "note": (
                    "challenge/origin/RP checked; verify the signature with a "
                    "separate host FIDO library before trusting it"
                ),
            },
        )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args(argv)
    try:
        if not ipaddress.ip_address(args.bind).is_loopback:
            parser.error("the fixture must bind a loopback address")
    except ValueError:
        parser.error("invalid bind address")
    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    print(f"http://localhost:{args.port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
