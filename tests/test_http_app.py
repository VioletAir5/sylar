#!/usr/bin/env python3
"""End-to-end HTTP app test with workers and graceful shutdown."""

import select
import socket
import subprocess
import sys


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def main(executable):
    port = free_port()
    server = subprocess.Popen(
        [executable, str(port), "2"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        readable, _, _ = select.select([server.stdout], [], [], 2)
        if not readable:
            raise RuntimeError("HTTP server did not report readiness")
        line = server.stdout.readline()
        if str(port) not in line or "workers=2" not in line:
            raise RuntimeError(f"unexpected startup: {line!r}")

        with socket.create_connection(
            ("127.0.0.1", port), timeout=2
        ) as client:
            client.settimeout(2)
            client.sendall(
                b"GET /health HTTP/1.1\r\nHost: test\r\n\r\n"
                b"POST /echo HTTP/1.1\r\nHost: test\r\n"
                b"Content-Length: 7\r\nConnection: close\r\n"
                b"\r\npayload"
            )
            parts = []
            while chunk := client.recv(4096):
                parts.append(chunk)
            wire = b"".join(parts)
            if wire.count(b"HTTP/1.1 200 OK") != 2:
                raise RuntimeError(f"unexpected responses: {wire!r}")
            if b"\r\n\r\nok\n" not in wire or not wire.endswith(
                b"\r\n\r\npayload"
            ):
                raise RuntimeError(f"wrong HTTP bodies: {wire!r}")

        server.terminate()
        if server.wait(timeout=2) != 0:
            raise RuntimeError(server.stderr.read())
    finally:
        if server.poll() is None:
            server.kill()
            server.wait()


if __name__ == "__main__":
    main(sys.argv[1])
