#!/usr/bin/env python3
"""Exercise admission control and two-phase HTTP shutdown over real sockets."""

import http.client
import os
import select
import signal
import socket
import subprocess
import sys
import time


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def launch(executable, limit, diagnostics=False):
    port = free_port()
    env = os.environ.copy()
    env["SYLAR_MAX_CONNECTIONS"] = str(limit)
    env["SYLAR_DRAIN_TIMEOUT_MS"] = "2000"
    env["SYLAR_IO_DIAGNOSTICS"] = "1" if diagnostics else "0"
    process = subprocess.Popen(
        [executable, str(port), "2"],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    readable, _, _ = select.select([process.stdout], [], [], 3)
    if not readable or f"port {port}" not in process.stdout.readline():
        process.kill()
        process.wait()
        raise RuntimeError("HTTP server did not start")
    return process, port


def get(port, path):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
    try:
        connection.request("GET", path)
        response = connection.getresponse()
        return response.status, response.read(), dict(response.headers)
    finally:
        connection.close()


def metric(port, name):
    status, body, _ = get(port, "/metrics")
    assert status == 200
    for line in body.decode().splitlines():
        if line.startswith(name + " "):
            return int(line.split()[1])
    raise AssertionError(f"missing metric {name}")


def wait_for(predicate, seconds=2):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            if predicate():
                return
        except (OSError, AssertionError):
            pass
        time.sleep(0.02)
    raise AssertionError("condition did not become true")


def close_process(process):
    if process.poll() is None:
        process.kill()
    process.wait(timeout=3)


def test_worker_diagnostics(executable):
    process, port = launch(executable, 4, diagnostics=True)
    try:
        for _ in range(12):
            assert get(port, "/health")[0] == 200
        status, body, _ = get(port, "/metrics")
        assert status == 200
        lines = body.decode().splitlines()
        for worker in range(2):
            label = f'{{worker="{worker}"}}'
            assert (
                f"sylar_io_diagnostics_enabled{label} 1"
                in lines
            )
            posts = [
                int(line.rsplit(" ", 1)[1])
                for line in lines
                if line.startswith(f"sylar_io_posts_total{label} ")
            ]
            assert len(posts) == 1 and posts[0] > 0, lines
            wakeups = [
                int(line.rsplit(" ", 1)[1])
                for line in lines
                if line.startswith(
                    f"sylar_io_eventfd_wakeups_total{label} "
                )
            ]
            assert len(wakeups) == 1 and wakeups[0] > 0, lines
    finally:
        close_process(process)


def test_capacity_and_readiness(executable):
    process, port = launch(executable, 2)
    clients = []
    try:
        assert get(port, "/ready")[:2] == (200, b"ready\n")
        for _ in range(2):
            client = socket.create_connection(("127.0.0.1", port), 2)
            client.settimeout(2)
            clients.append(client)
        # A third accepted fd is immediately closed before a Fiber is posted.
        with socket.create_connection(
            ("127.0.0.1", port), 2
        ) as rejected:
            rejected.settimeout(2)
            assert rejected.recv(1) == b""
        clients.pop().close()
        wait_for(
            lambda: metric(
                port, "sylar_tcp_rejected_connections_total"
            ) >= 1
        )
        assert metric(port, "sylar_tcp_connection_limit") == 2
        clients.pop().close()
        process.send_signal(signal.SIGUSR1)
        wait_for(lambda: get(port, "/ready")[0] == 503)
        assert get(port, "/health")[0] == 200
        with socket.create_connection(
            ("127.0.0.1", port), 2
        ) as client:
            client.settimeout(2)
            client.sendall(b"GET /health HTTP/1.1\r\nHost: test\r\n\r\n")
            wire = b""
            while chunk := client.recv(4096):
                wire += chunk
            assert b"Connection: close\r\n" in wire
            assert wire.endswith(b"\r\n\r\nok\n")
        process.terminate()
        assert process.wait(timeout=3) == 0
    finally:
        for client in clients:
            client.close()
        close_process(process)


def test_drain_deadline_is_bounded(executable):
    process, port = launch(executable, 2)
    try:
        with socket.create_connection(
            ("127.0.0.1", port), 2
        ) as idle:
            idle.settimeout(3)
            wait_for(
                lambda: metric(
                    port, "sylar_tcp_active_connections"
                ) >= 2
            )
            started = time.monotonic()
            process.terminate()
            assert process.wait(timeout=3) == 0
            elapsed = time.monotonic() - started
            assert 1.8 <= elapsed < 3, elapsed
            assert idle.recv(1) == b""
    finally:
        close_process(process)


def test_inflight_request_finishes(executable):
    process, port = launch(executable, 4)
    try:
        with socket.create_connection(
            ("127.0.0.1", port), 2
        ) as client:
            client.settimeout(3)
            client.sendall(
                b"POST /echo HTTP/1.1\r\nHost: test\r\n"
                b"Content-Length: 7\r\n\r\npay"
            )
            wait_for(
                lambda: metric(
                    port, "sylar_tcp_active_connections"
                ) >= 2
            )
            process.send_signal(signal.SIGUSR1)
            wait_for(lambda: get(port, "/ready")[0] == 503)
            process.terminate()
            client.sendall(b"load")
            wire = b""
            while chunk := client.recv(4096):
                wire += chunk
            assert b"HTTP/1.1 200 OK" in wire, wire
            assert b"Connection: close\r\n" in wire, wire
            assert wire.endswith(b"\r\n\r\npayload"), wire
        assert process.wait(timeout=3) == 0
    finally:
        close_process(process)


if __name__ == "__main__":
    test_worker_diagnostics(sys.argv[1])
    test_capacity_and_readiness(sys.argv[1])
    test_inflight_request_finishes(sys.argv[1])
    test_drain_deadline_is_bounded(sys.argv[1])
