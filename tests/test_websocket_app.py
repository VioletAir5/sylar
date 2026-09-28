#!/usr/bin/env python3
"""Raw RFC 6455 handshake and frame tests for ws and wss echo apps."""

import base64
import hashlib
import select
import socket
import ssl
import subprocess
import sys
import tempfile
from pathlib import Path

CRLF = bytes((13, 10))
KEY = b"dGhlIHNhbXBsZSBub25jZQ=="
GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class Reader:
    def __init__(self, sock):
        self.sock = sock
        self.buffer = b""

    def take(self, count):
        while len(self.buffer) < count:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("unexpected WebSocket EOF")
            self.buffer += chunk
        result, self.buffer = self.buffer[:count], self.buffer[count:]
        return result

    def until(self, delimiter):
        while delimiter not in self.buffer:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("unexpected handshake EOF")
            self.buffer += chunk
        end = self.buffer.index(delimiter) + len(delimiter)
        result, self.buffer = self.buffer[:end], self.buffer[end:]
        return result


def frame(opcode, payload=b"", final=True, masked=True):
    first = opcode | (128 if final else 0)
    mask_bit = 128 if masked else 0
    if len(payload) <= 125:
        header = bytes((first, mask_bit | len(payload)))
    elif len(payload) <= 65535:
        header = bytes((first, mask_bit | 126)) + len(payload).to_bytes(2, "big")
    else:
        header = bytes((first, mask_bit | 127)) + len(payload).to_bytes(8, "big")
    if not masked:
        return header + payload
    mask = bytes((1, 2, 3, 4))
    encoded = bytes(value ^ mask[i % 4] for i, value in enumerate(payload))
    return header + mask + encoded


def response_frame(reader):
    first, second = reader.take(2)
    if second & 128:
        raise RuntimeError("server masked a WebSocket frame")
    length = second & 127
    if length == 126:
        length = int.from_bytes(reader.take(2), "big")
    elif length == 127:
        length = int.from_bytes(reader.take(8), "big")
    return first, reader.take(length)


def handshake(sock, extra=b"", valid=True):
    wire = (
        b"GET /ws HTTP/1.1" + CRLF
        + b"Host: localhost" + CRLF
        + b"Upgrade: websocket" + CRLF
        + b"Connection: keep-alive, Upgrade" + CRLF
        + b"Sec-WebSocket-Version: 13" + CRLF
    )
    if valid:
        wire += b"Sec-WebSocket-Key: " + KEY + CRLF
    sock.sendall(wire + CRLF + extra)
    reader = Reader(sock)
    response = reader.until(CRLF + CRLF)
    if valid:
        expected = base64.b64encode(hashlib.sha1(KEY + GUID).digest())
        if b"101 Switching Protocols" not in response or (
            b"Sec-WebSocket-Accept: " + expected
        ) not in response:
            raise RuntimeError(f"invalid upgrade: {response!r}")
    elif b"400 Bad Request" not in response:
        raise RuntimeError(f"bad request accepted: {response!r}")
    return reader


def one_mode(executable, certificate=None, private_key=None):
    port = free_port()
    command = [executable, str(port), "2"]
    if certificate:
        command += ["--tls", str(certificate), str(private_key)]
    server = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        ready, _, _ = select.select([server.stdout], [], [], 3)
        if not ready:
            raise RuntimeError("WebSocket server did not start")
        line = server.stdout.readline()
        if str(port) not in line:
            raise RuntimeError(f"unexpected startup: {line!r}")

        def connect():
            raw = socket.create_connection(("127.0.0.1", port), timeout=2)
            if not certificate:
                return raw
            context = ssl.create_default_context(cafile=str(certificate))
            return context.wrap_socket(raw, server_hostname="localhost")

        with connect() as sock:
            sock.settimeout(2)
            reader = handshake(
                sock, frame(1, b"hello")
            )
            if response_frame(reader) != (129, b"hello"):
                raise RuntimeError("pipelined text echo failed")

            sock.sendall(frame(9, b"ping"))
            if response_frame(reader) != (138, b"ping"):
                raise RuntimeError("pong failed")

            sock.sendall(frame(1, b"frag", final=False)
                         + frame(0, b"mented"))
            if response_frame(reader) != (129, b"fragmented"):
                raise RuntimeError("fragmentation failed")

            payload = bytes(range(130))
            sock.sendall(frame(2, payload))
            if response_frame(reader) != (130, payload):
                raise RuntimeError("extended binary echo failed")

            large = bytes(range(256)) * 258
            sock.sendall(frame(2, large))
            if response_frame(reader) != (130, large):
                raise RuntimeError("64-bit frame length failed")

            sock.sendall(frame(8, bytes((3, 232))))
            if response_frame(reader) != (136, bytes((3, 232))):
                raise RuntimeError("close handshake failed")

        with connect() as bad:
            bad.settimeout(2)
            handshake(bad, valid=False)

        with connect() as unmasked:
            unmasked.settimeout(2)
            handshake(unmasked)
            unmasked.sendall(frame(1, b"bad", masked=False))
            if response_frame(Reader(unmasked)) != (136, bytes((3, 234))):
                raise RuntimeError("unmasked frame was not rejected")

        server.terminate()
        if server.wait(timeout=2) != 0:
            raise RuntimeError(server.stderr.read())
    finally:
        if server.poll() is None:
            server.kill()
            server.wait()


def main(executable):
    one_mode(executable)
    with tempfile.TemporaryDirectory() as directory:
        cert = Path(directory) / "cert.pem"
        key = Path(directory) / "key.pem"
        subprocess.run(
            [
                "openssl", "req", "-x509", "-newkey", "rsa:2048",
                "-nodes", "-days", "1", "-subj", "/CN=localhost",
                "-addext", "subjectAltName=DNS:localhost",
                "-keyout", str(key), "-out", str(cert),
            ],
            check=True, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, timeout=5,
        )
        one_mode(executable, cert, key)


if __name__ == "__main__":
    main(sys.argv[1])