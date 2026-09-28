#!/usr/bin/env python3
"""MySQL adapter against an isolated temporary MySQL 8 instance."""

import subprocess
import sys
import tempfile
import time
from pathlib import Path


def main(executable):
    with tempfile.TemporaryDirectory(
        prefix="sylar-mysql-"
    ) as directory:
        base = Path(directory)
        data = base / "data"
        data.mkdir()
        socket = base / "mysql.sock"
        init_log = base / "init.log"
        server_log = base / "server.log"
        init = subprocess.run(
            [
                "mysqld", "--no-defaults",
                "--initialize-insecure",
                f"--datadir={data}",
                f"--log-error={init_log}",
            ],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        if init.returncode != 0:
            raise RuntimeError(
                f"MySQL init failed: {init.stderr}"
                f" {init_log.read_text()}"
            )
        server = subprocess.Popen(
            [
                "mysqld", "--no-defaults",
                f"--datadir={data}",
                f"--socket={socket}",
                f"--pid-file={base / 'mysql.pid'}",
                f"--log-error={server_log}",
                "--skip-networking",
                "--mysqlx=0",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if server.poll() is not None:
                    raise RuntimeError(
                        f"MySQL exited: {server_log.read_text()}"
                    )
                ready = subprocess.run(
                    [
                        "mysqladmin", "--no-defaults",
                        f"--socket={socket}",
                        "-uroot", "ping",
                    ],
                    capture_output=True,
                    text=True,
                    timeout=2,
                    check=False,
                )
                if ready.returncode == 0:
                    break
                time.sleep(0.1)
            else:
                raise RuntimeError(
                    f"MySQL not ready: {server_log.read_text()}"
                )
            result = subprocess.run(
                [executable, str(socket)],
                capture_output=True,
                text=True,
                timeout=10,
                check=False,
            )
            if result.returncode != 0 or (
                result.stdout.strip() != "mysql ok"
            ):
                raise RuntimeError(
                    f"MySQL adapter failed: {result!r}"
                    f" {server_log.read_text()[-2000:]}"
                )
            server.terminate()
            if server.wait(timeout=10) != 0:
                raise RuntimeError(
                    server_log.read_text()
                )
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()


if __name__ == "__main__":
    main(sys.argv[1])