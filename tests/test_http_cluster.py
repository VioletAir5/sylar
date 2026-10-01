#!/usr/bin/env python3
"""Run two independent HTTP replicas and verify health and identity."""

import http.client
import os
import select
import socket
import subprocess
import sys


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def get(port, path):
    connection = http.client.HTTPConnection(
        "127.0.0.1", port, timeout=2
    )
    try:
        connection.request("GET", path)
        response = connection.getresponse()
        return response.status, response.read().decode()
    finally:
        connection.close()


def start(executable, name, port):
    env = os.environ.copy()
    env["SYLAR_INSTANCE_ID"] = name
    process = subprocess.Popen(
        [executable, str(port), "2"],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        readable, _, _ = select.select([process.stdout], [], [], 3)
        if not readable:
            raise RuntimeError(f"{name} did not start")
        line = process.stdout.readline()
        if f"instance={name}" not in line:
            raise RuntimeError(f"{name} startup: {line!r}")
        return process
    except BaseException:
        if process.poll() is None:
            process.kill()
        process.wait()
        raise


def main(executable):
    ports = [free_port(), free_port()]
    while ports[0] == ports[1]:
        ports[1] = free_port()
    processes = []
    try:
        for index, port in enumerate(ports):
            processes.append(
                start(executable, f"replica-{index + 1}", port)
            )
        for index, port in enumerate(ports):
            assert get(port, "/health") == (200, "ok\n")
            assert get(port, "/ready") == (200, "ready\n")
            assert get(port, "/instance") == (
                200, f"replica-{index + 1}\n"
            )
        processes[0].terminate()
        if processes[0].wait(timeout=3) != 0:
            raise RuntimeError("first replica failed to stop")
        assert get(ports[1], "/instance") == (200, "replica-2\n")
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == "__main__":
    main(sys.argv[1])
