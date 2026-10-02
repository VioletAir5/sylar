#!/usr/bin/env python3
"""Reproducible keep-alive HTTP/1.1 loopback benchmark."""

import argparse
import asyncio
import os
import socket
import time
from pathlib import Path

CRLF = bytes((13, 10))
REQUEST = (
    b"GET /health HTTP/1.1" + CRLF
    + b"Host: localhost" + CRLF + CRLF
)


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def cpu_seconds(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().split()
    ticks = os.sysconf("SC_CLK_TCK")
    return (int(fields[13]) + int(fields[14])) / ticks


def peak_kib(pid):
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith("VmHWM:"):
            return int(line.split()[1])
    return 0


async def client(reader, writer, count):
    samples = []
    try:
        for _ in range(count):
            start = time.perf_counter_ns()
            writer.write(REQUEST)
            await writer.drain()
            header = await reader.readuntil(CRLF + CRLF)
            if not header.startswith(b"HTTP/1.1 200 OK" + CRLF):
                raise RuntimeError(f"unexpected status: {header!r}")
            length = None
            for line in header.split(CRLF):
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":", 1)[1])
            if length is None or length > 1024:
                raise RuntimeError("missing or invalid Content-Length")
            body = await reader.readexactly(length)
            if body != b"ok" + bytes((10,)):
                raise RuntimeError(f"unexpected body: {body!r}")
            samples.append((time.perf_counter_ns() - start) / 1e6)
        return samples
    finally:
        writer.close()
        await writer.wait_closed()


async def read_diagnostics(port):
    reader, writer = await asyncio.open_connection(
        "127.0.0.1", port
    )
    try:
        writer.write(
            b"GET /metrics HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"Connection: close\r\n\r\n"
        )
        await writer.drain()
        wire = await reader.read()
        head, body = wire.split(b"\r\n\r\n", 1)
        if not head.startswith(b"HTTP/1.1 200 OK"):
            raise RuntimeError(f"metrics failed: {head!r}")
        for line in body.decode().splitlines():
            if line.startswith("sylar_io_"):
                print(line)
    finally:
        writer.close()
        await writer.wait_closed()


async def benchmark(args):
    port = free_port()
    server_env = os.environ.copy()
    if args.diagnostics:
        server_env["SYLAR_IO_DIAGNOSTICS"] = "1"
    server = await asyncio.create_subprocess_exec(
        args.server, str(port), str(args.workers),
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
        env=server_env,
    )
    try:
        line = await asyncio.wait_for(server.stdout.readline(), 3)
        if str(port).encode() not in line:
            raise RuntimeError(f"server failed to start: {line!r}")
        if args.native_client:
            before_cpu = cpu_seconds(server.pid)
            runner = await asyncio.create_subprocess_exec(
                args.native_client, str(port), str(args.clients),
                str(args.requests),
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )
            try:
                stdout, stderr = await asyncio.wait_for(
                    runner.communicate(), args.timeout
                )
            except asyncio.TimeoutError:
                runner.kill()
                await runner.wait()
                raise
            if runner.returncode != 0:
                raise RuntimeError(
                    f"native HTTP benchmark failed: {stderr.decode()}"
                )
            after_cpu = cpu_seconds(server.pid)
            peak = peak_kib(server.pid)
            print(
                f"workers={args.workers} "
                f"server_cpu_s={after_cpu - before_cpu:.3f} "
                f"server_vmhwm_kib={peak}"
            )
            print(stdout.decode().strip())
            if args.diagnostics:
                await read_diagnostics(port)
            return
        connections = await asyncio.wait_for(
            asyncio.gather(*[
                asyncio.open_connection("127.0.0.1", port)
                for _ in range(args.clients)
            ]),
            10,
        )
        before_cpu = cpu_seconds(server.pid)
        started = time.perf_counter()
        nested = await asyncio.wait_for(
            asyncio.gather(*[
                client(reader, writer, args.requests)
                for reader, writer in connections
            ]),
            args.timeout,
        )
        elapsed = time.perf_counter() - started
        after_cpu = cpu_seconds(server.pid)
        peak = peak_kib(server.pid)
        latencies = sorted(
            sample for group in nested for sample in group
        )
        total = len(latencies)
        p50 = latencies[int(0.50 * (total - 1))]
        p99 = latencies[int(0.99 * (total - 1))]
        print(
            f"workers={args.workers} clients={args.clients} "
            f"requests={total} elapsed_s={elapsed:.3f} "
            f"req_s={total / elapsed:.0f} "
            f"p50_ms={p50:.3f} p99_ms={p99:.3f} "
            f"server_cpu_s={after_cpu - before_cpu:.3f} "
            f"server_vmhwm_kib={peak}"
        )
        if args.diagnostics:
            await read_diagnostics(port)
    finally:
        if server.returncode is None:
            server.terminate()
            try:
                await asyncio.wait_for(server.wait(), 3)
            except asyncio.TimeoutError:
                server.kill()
                await server.wait()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--clients", type=int, default=100)
    parser.add_argument("--requests", type=int, default=100)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--native-client")
    parser.add_argument("--diagnostics", action="store_true")
    args = parser.parse_args()
    if (args.workers < 1 or args.clients < 1 or
            args.requests < 1 or args.clients > 2000):
        parser.error("workers, clients and requests must be positive")
    asyncio.run(benchmark(args))


if __name__ == "__main__":
    main()