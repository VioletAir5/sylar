#!/usr/bin/env python3
"""HTTPS verification, bad peers, and graceful shutdown."""

import select
import socket
import ssl
import subprocess
import sys
import tempfile
from pathlib import Path

CRLF = bytes((13, 10))


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def client(executable, url, *args):
    return subprocess.run(
        [executable, url, *args],
        capture_output=True, text=True, timeout=5, check=False,
    )


def main(server_executable, client_executable):
    with tempfile.TemporaryDirectory() as directory:
        cert = Path(directory) / "cert.pem"
        key = Path(directory) / "key.pem"
        subprocess.run(
            [
                "openssl", "req", "-x509", "-newkey", "rsa:2048",
                "-nodes", "-days", "1", "-subj", "/CN=localhost",
                "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
                "-keyout", str(key), "-out", str(cert),
            ],
            check=True, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, timeout=5,
        )
        port = free_port()
        server = subprocess.Popen(
            [server_executable, str(port), "2",
             "--tls", str(cert), str(key)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            readable, _, _ = select.select([server.stdout], [], [], 3)
            if not readable:
                raise RuntimeError("HTTPS server did not report readiness")
            line = server.stdout.readline()
            if "https_server" not in line or str(port) not in line:
                raise RuntimeError(f"unexpected startup: {line!r}")

            with socket.create_connection(
                ("127.0.0.1", port), timeout=2
            ) as plain:
                plain.sendall(b"GET / HTTP/1.1" + CRLF + CRLF)

            context = ssl.create_default_context(cafile=str(cert))
            with socket.create_connection(
                ("127.0.0.1", port), timeout=2
            ) as raw:
                with context.wrap_socket(
                    raw, server_hostname="localhost"
                ) as verified:
                    verified.sendall(
                        b"GET /health HTTP/1.1" + CRLF
                        + b"Host: localhost" + CRLF
                        + b"Connection: close" + CRLF + CRLF
                    )
                    wire = b""
                    while chunk := verified.recv(4096):
                        wire += chunk
                    if b"HTTP/1.1 200 OK" not in wire or not wire.endswith(
                        CRLF + CRLF + b"ok" + bytes((10,))
                    ):
                        raise RuntimeError(f"bad response: {wire!r}")

            url = f"https://127.0.0.1:{port}/health"
            trusted = client(client_executable, url, "--ca", str(cert))
            if trusted.returncode != 0 or "ok" not in trusted.stdout:
                raise RuntimeError(f"trusted client failed: {trusted!r}")
            untrusted = client(client_executable, url)
            if untrusted.returncode == 0:
                raise RuntimeError("untrusted certificate was accepted")

            server.terminate()
            if server.wait(timeout=2) != 0:
                raise RuntimeError(server.stderr.read())
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])