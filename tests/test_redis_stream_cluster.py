#!/usr/bin/env python3
"""Two WebSocket replicas share a bounded Redis Stream and replay by ID."""

import os
import select
import socket
import subprocess
import sys
import tempfile
import time

from test_websocket_app import frame, handshake, response_frame


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def ready(process, port):
    readable, _, _ = select.select([process.stdout], [], [], 3)
    if not readable or str(port) not in process.stdout.readline():
        raise RuntimeError("replica did not start")


def start_replica(executable, listen_port, redis_port):
    env = os.environ.copy()
    env["SYLAR_STREAM_RETENTION"] = "3"
    process = subprocess.Popen(
        [executable, str(listen_port), str(redis_port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
    )
    ready(process, listen_port)
    return process


def connect(port):
    sock = socket.create_connection(("127.0.0.1", port), 2)
    sock.settimeout(3)
    reader = handshake(sock)
    return sock, reader


def send(sock, reader, message):
    sock.sendall(frame(1, message.encode()))
    opcode, payload = response_frame(reader)
    assert opcode == 129, (opcode, payload)
    return payload.decode()


def stop(process):
    if process and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def main(executable):
    redis_port, port_a, port_b = free_port(), free_port(), free_port()
    with tempfile.TemporaryDirectory() as directory:
        redis = subprocess.Popen(
            ["redis-server", "--bind", "127.0.0.1", "--port", str(redis_port),
             "--save", "", "--appendonly", "no", "--dir", directory],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        )
        a = b = None
        try:
            deadline = time.monotonic() + 3
            while True:
                try:
                    with socket.create_connection(("127.0.0.1", redis_port), .2):
                        break
                except OSError:
                    if time.monotonic() > deadline:
                        raise RuntimeError("Redis did not start")
                    time.sleep(.02)
            a = start_replica(executable, port_a, redis_port)
            b = start_replica(executable, port_b, redis_port)
            subscriber, reader_b = connect(port_b)
            subscriber.sendall(frame(1, b"SUB 0-0"))
            local, reader_local = connect(port_a)
            local.sendall(frame(1, b"SUB 0-0"))
            publisher, reader_a = connect(port_a)
            first = send(publisher, reader_a, "PUB first")
            assert first.startswith("ACK "), first
            message = response_frame(reader_b)[1].decode()
            assert message == "EVENT " + first[4:] + " first", message
            assert response_frame(reader_local)[1].decode() == message
            subscriber.close()
            stop(b)
            b = None
            second = send(publisher, reader_a, "PUB second")
            assert second.startswith("ACK "), second
            assert response_frame(reader_local)[1].decode() == (
                "EVENT " + second[4:] + " second"
            )
            b = start_replica(executable, port_b, redis_port)
            replay, reader_b = connect(port_b)
            replay.sendall(frame(1, ("SUB " + first[4:]).encode()))
            message = response_frame(reader_b)[1].decode()
            assert message == "EVENT " + second[4:] + " second", message
            replay.close()
            for index in range(3):
                assert send(publisher, reader_a, f"PUB trim{index}").startswith("ACK ")
            gap, gap_reader = connect(port_b)
            assert send(gap, gap_reader, "SUB " + first[4:]) == (
                "GAP replay-window-expired"
            )
            gap.close()
            stop(redis)
            assert send(publisher, reader_a, "PUB unavailable") == (
                "ERROR redis-unavailable"
            )
            local.close()
            publisher.close()
        finally:
            stop(b)
            stop(a)
            stop(redis)


if __name__ == "__main__":
    main(sys.argv[1])
