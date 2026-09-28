#!/usr/bin/env python3
"""Redis adapter against an isolated local Redis process."""

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
    with tempfile.TemporaryDirectory() as directory:
        server = subprocess.Popen(
            [
                "redis-server",
                "--bind", "127.0.0.1",
                "--port", str(port),
                "--save", "",
                "--appendonly", "no",
                "--protected-mode", "yes",
                "--dir", directory,
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                if server.poll() is not None:
                    raise RuntimeError(
                        f"Redis exited: {server.stderr.read()}"
                    )
                try:
                    with socket.create_connection(
                        ("127.0.0.1", port), timeout=0.2
                    ):
                        break
                except OSError:
                    time.sleep(0.02)
            else:
                raise RuntimeError("Redis did not become ready")

            result = subprocess.run(
                [executable, "127.0.0.1", str(port)],
                capture_output=True,
                text=True,
                timeout=5,
                check=False,
            )
            if result.returncode != 0 or (
                result.stdout.strip() != "redis ok"
            ):
                raise RuntimeError(
                    f"Redis adapter failed: {result!r}"
                )
            server.terminate()
            if server.wait(timeout=3) != 0:
                raise RuntimeError(
                    server.stderr.read()
                )
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()


if __name__ == "__main__":
    main(sys.argv[1])