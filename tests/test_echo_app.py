#!/usr/bin/env python3
"""End-to-end echo app test: config, idle timeout and shutdown."""

import select
import socket
import subprocess
import sys
import tempfile
import time


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def main(executable):
    port = free_port()
    with tempfile.NamedTemporaryFile(mode="w", suffix=".yml") as config:
        config.write(
            f"server:\n  port: {port}\n  workers: 2\n"
            "  idle_timeout_ms: 200\nlog:\n  level: INFO\n"
        )
        config.flush()
        server = subprocess.Popen(
            [executable, "--config", config.name],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            readable, _, _ = select.select([server.stdout], [], [], 2)
            if not readable:
                raise RuntimeError("server did not report readiness")
            line = server.stdout.readline()
            if str(port) not in line or "workers=2" not in line:
                raise RuntimeError(f"unexpected startup: {line!r}")

            with socket.create_connection(
                ("127.0.0.1", port), timeout=2
            ) as client:
                client.settimeout(2)
                for _ in range(3):
                    client.sendall(b"x")
                    if client.recv(1) != b"x":
                        raise RuntimeError("echo mismatch")
                    time.sleep(0.05)
                time.sleep(0.3)
                if client.recv(1) != b"":
                    raise RuntimeError("idle connection did not close")

            server.terminate()
            if server.wait(timeout=2) != 0:
                raise RuntimeError(server.stderr.read())
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()


if __name__ == "__main__":
    main(sys.argv[1])
