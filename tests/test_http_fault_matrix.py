#!/usr/bin/env python3
"""Deterministic loopback faults with fd and recovery checks."""

import os
import random
import select
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

SEED = 20261002
REQUEST = b"GET /health HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n"


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def fd_count(pid):
    return len(list(Path(f"/proc/{pid}/fd").iterdir()))


def fetch(port):
    with socket.create_connection(("127.0.0.1", port), 2) as client:
        client.settimeout(2)
        client.sendall(REQUEST)
        client.shutdown(socket.SHUT_WR)
        wire = bytearray()
        while data := client.recv(4096):
            wire.extend(data)
    return bytes(wire)


def active_connections(port):
    request = b"GET /metrics HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n"
    with socket.create_connection(("127.0.0.1", port), 2) as client:
        client.settimeout(2)
        client.sendall(request)
        wire = bytearray()
        while data := client.recv(4096):
            wire.extend(data)
    for line in bytes(wire).decode().splitlines():
        if line.startswith("sylar_tcp_active_connections "):
            return int(line.split()[1])
    raise AssertionError("active connection metric missing")


def proxy_once(port, trace):
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    proxy_port = listener.getsockname()[1]
    errors = []

    def worker():
        try:
            with listener:
                client, _ = listener.accept()
                with client, socket.create_connection(("127.0.0.1", port), 2) as upstream:
                    client.settimeout(2)
                    upstream.settimeout(2)
                    request = bytearray()
                    while data := client.recv(4096):
                        request.extend(data)
                    rng = random.Random(SEED)
                    offset = 0
                    while offset < len(request):
                        size = rng.randint(1, 7)
                        trace.append(size)
                        upstream.sendall(request[offset:offset + size])
                        offset += size
                        time.sleep(.001)
                    upstream.shutdown(socket.SHUT_WR)
                    while data := upstream.recv(4096):
                        for index in range(0, len(data), 3):
                            client.sendall(data[index:index + 3])
                            time.sleep(.001)
        except BaseException as error:
            errors.append(error)

    thread = threading.Thread(target=worker, daemon=True)
    thread.start()
    wire = fetch(proxy_port)
    thread.join(timeout=3)
    if thread.is_alive() or errors:
        raise AssertionError(f"proxy failed: {errors!r}")
    return wire


def main(executable):
    port = free_port()
    process = subprocess.Popen(
        [executable, str(port), "2"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True,
    )
    trace = []
    try:
        readable, _, _ = select.select([process.stdout], [], [], 3)
        if not readable or str(port) not in process.stdout.readline():
            raise RuntimeError("HTTP server did not start")
        baseline = fd_count(process.pid)
        assert fetch(port).endswith(b"\r\n\r\nok\n")
        assert proxy_once(port, trace).endswith(b"\r\n\r\nok\n")
        for _ in range(40):
            client = socket.create_connection(("127.0.0.1", port), 2)
            client.sendall(b"POST /echo HTTP/1.1\r\nHost: local\r\n")
            client.setsockopt(
                socket.SOL_SOCKET, socket.SO_LINGER,
                struct.pack("ii", 1, 0),
            )
            client.close()
        deadline = time.monotonic() + 3
        while fd_count(process.pid) > baseline + 2:
            if time.monotonic() > deadline:
                raise AssertionError("fd count did not recover")
            time.sleep(.02)
        assert active_connections(port) <= 2
        assert fetch(port).endswith(b"\r\n\r\nok\n")
        print(f"seed={SEED} chunks={trace} baseline_fd={baseline} final_fd={fd_count(process.pid)}")
    except BaseException:
        print(f"seed={SEED} chunks={trace}", file=sys.stderr)
        raise
    finally:
        if process.poll() is None:
            process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


if __name__ == "__main__":
    main(sys.argv[1])
