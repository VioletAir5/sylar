#!/usr/bin/env python3
"""Run ws/wss client against the local echo server."""

import select
import socket
import subprocess
import sys
import tempfile
from pathlib import Path


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def one_mode(client, server_executable, cert=None, key=None):
    port = free_port()
    command = [server_executable, str(port), "2"]
    if cert:
        command += ["--tls", str(cert), str(key)]
    server = subprocess.Popen(
        command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True,
    )
    try:
        ready, _, _ = select.select([server.stdout], [], [], 3)
        if not ready or str(port) not in server.stdout.readline():
            raise RuntimeError("WebSocket server failed to start")
        scheme = "wss" if cert else "ws"
        host = "localhost" if cert else "127.0.0.1"
        command = [client, f"{scheme}://{host}:{port}/ws", "client-echo"]
        if cert:
            command += ["--ca", str(cert)]
        result = subprocess.run(
            command, capture_output=True, text=True, timeout=10,
            check=True,
        )
        if result.stdout != "client-echo\n":
            raise RuntimeError(f"unexpected client output: {result.stdout!r}")
        server.terminate()
        if server.wait(timeout=3) != 0:
            raise RuntimeError(server.stderr.read())
    finally:
        if server.poll() is None:
            server.kill()
            server.wait()


def main(client, server):
    one_mode(client, server)
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
        one_mode(client, server, cert, key)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
