#!/usr/bin/env python3
"""ZooKeeper adapter against an isolated local Java server."""

import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def main(executable):
    with tempfile.TemporaryDirectory(
        prefix="sylar-zk-"
    ) as directory:
        base = Path(directory)
        data = base / "data"
        data.mkdir()
        port = free_port()
        config = base / "zoo.cfg"
        config.write_text(
            f"tickTime=2000\n"
            f"dataDir={data}\n"
            f"clientPort={port}\n"
            "maxClientCnxns=10\n"
        )
        log_path = base / "server.log"
        with log_path.open("w+") as log:
            env = os.environ.copy()
            env["JMXDISABLE"] = "true"
            env["JAVA_TOOL_OPTIONS"] = "-XX:-UseContainerSupport"
            server = subprocess.Popen(
                [
                    "/usr/share/zookeeper/bin/zkServer.sh",
                    "start-foreground", str(config),
                ],
                stdout=log,
                stderr=subprocess.STDOUT,
                env=env,
            )
            try:
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    if server.poll() is not None:
                        raise RuntimeError(
                            f"ZooKeeper exited: "
                            f"{log_path.read_text()[-2500:]}"
                        )
                    try:
                        with socket.create_connection(
                            ("127.0.0.1", port),
                            timeout=0.2,
                        ):
                            break
                    except OSError:
                        time.sleep(0.05)
                else:
                    raise RuntimeError(
                        f"ZooKeeper not ready: "
                        f"{log_path.read_text()[-2500:]}"
                    )
                result = subprocess.run(
                    [
                        executable,
                        f"127.0.0.1:{port}",
                    ],
                    capture_output=True,
                    text=True,
                    timeout=10,
                    check=False,
                )
                if result.returncode != 0 or (
                    result.stdout.strip() != "zk ok"
                ):
                    raise RuntimeError(
                        f"ZooKeeper adapter failed: "
                        f"{result!r} "
                        f"{log_path.read_text()[-2500:]}"
                    )
            finally:
                if server.poll() is None:
                    server.terminate()
                    try:
                        server.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait()


if __name__ == "__main__":
    main(sys.argv[1])