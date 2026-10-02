#!/usr/bin/env python3
"""Compare fixed and adaptive request limits under a controlled loopback load."""

import argparse
import asyncio
import os
import socket
import statistics
import time

REQUEST = b"GET /work HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


async def one(port):
    started = time.perf_counter()
    reader, writer = await asyncio.open_connection("127.0.0.1", port)
    try:
        writer.write(REQUEST)
        await writer.drain()
        head = await reader.readuntil(b"\r\n\r\n")
        status = int(head.split(b" ", 2)[1])
        await reader.read()
        return status, (time.perf_counter() - started) * 1000
    finally:
        writer.close()
        await writer.wait_closed()


async def trial(args, adaptive):
    port = free_port()
    env = os.environ.copy()
    env["SYLAR_MAX_INFLIGHT_REQUESTS"] = str(args.limit)
    env["SYLAR_ADAPTIVE_CONCURRENCY"] = str(int(adaptive))
    env["SYLAR_DEMO_WORK_MS"] = str(args.work_ms)
    server = await asyncio.create_subprocess_exec(
        args.server, str(port), str(args.workers),
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
        env=env,
    )
    try:
        ready = await asyncio.wait_for(server.stdout.readline(), 3)
        if str(port).encode() not in ready:
            raise RuntimeError(f"server failed: {ready!r}")
        latencies = []
        accepted = rejected = 0
        start = time.perf_counter()
        for _ in range(args.waves):
            results = await asyncio.wait_for(
                asyncio.gather(*(one(port) for _ in range(args.clients))),
                args.timeout,
            )
            for status, latency in results:
                if status == 200:
                    accepted += 1
                    latencies.append(latency)
                elif status == 503:
                    rejected += 1
                else:
                    raise RuntimeError(f"unexpected HTTP {status}")
        elapsed = time.perf_counter() - start
        sorted_ms = sorted(latencies)
        p99 = sorted_ms[int((len(sorted_ms) - 1) * .99)] if sorted_ms else 0
        print(
            f"adaptive={int(adaptive)} accepted={accepted} rejected={rejected} "
            f"elapsed_s={elapsed:.3f} accepted_req_s={accepted / elapsed:.1f} "
            f"mean_ms={statistics.mean(latencies) if latencies else 0:.2f} "
            f"p99_ms={p99:.2f}"
        )
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
    parser.add_argument("--clients", type=int, default=64)
    parser.add_argument("--waves", type=int, default=20)
    parser.add_argument("--work-ms", type=int, default=20)
    parser.add_argument("--limit", type=int, default=16)
    parser.add_argument("--timeout", type=float, default=10)
    args = parser.parse_args()
    if min(args.workers, args.clients, args.waves, args.work_ms, args.limit) < 1:
        parser.error("all integer load parameters must be positive")
    async def run():
        await trial(args, False)
        await trial(args, True)
    asyncio.run(run())


if __name__ == "__main__":
    main()
