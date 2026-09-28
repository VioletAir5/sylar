#!/usr/bin/env python3
"""End-to-end echo load generator; run against an already started server."""

import argparse
import asyncio
import math
import statistics
import time


async def run_client(reader, writer, payload, requests, latencies):
    try:
        for _ in range(requests):
            started = time.perf_counter_ns()
            writer.write(payload)
            await writer.drain()
            response = await asyncio.wait_for(
                reader.readexactly(len(payload)), timeout=10
            )
            if response != payload:
                raise RuntimeError("echo payload mismatch")
            latencies.append((time.perf_counter_ns() - started) / 1_000_000)
    finally:
        writer.close()
        await writer.wait_closed()


async def benchmark(args):
    connections = await asyncio.gather(
        *(
            asyncio.wait_for(
                asyncio.open_connection(args.host, args.port), timeout=10
            )
            for _ in range(args.clients)
        )
    )
    payload = b"x" * args.bytes
    latencies = []
    started = time.perf_counter()
    await asyncio.gather(
        *(
            run_client(reader, writer, payload, args.requests, latencies)
            for reader, writer in connections
        )
    )
    elapsed = time.perf_counter() - started
    latencies.sort()
    completed = args.clients * args.requests

    def percentile(fraction):
        return latencies[math.ceil(fraction * completed) - 1]

    print(
        f"clients={args.clients} requests/client={args.requests} "
        f"payload={args.bytes}B completed={completed}"
    )
    print(
        f"elapsed={elapsed:.3f}s throughput={completed / elapsed:.0f} req/s "
        f"payload={completed * args.bytes / elapsed / 1024 / 1024:.2f} MiB/s/direction"
    )
    print(
        f"latency_ms p50={statistics.median(latencies):.3f} "
        f"p99={percentile(0.99):.3f} max={latencies[-1]:.3f}"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--clients", type=int, default=100)
    parser.add_argument("--requests", type=int, default=100)
    parser.add_argument("--bytes", type=int, default=64)
    args = parser.parse_args()
    if (
        args.clients < 1
        or args.requests < 1
        or args.bytes < 1
        or args.bytes > 1_000_000
    ):
        parser.error("clients, requests, and bytes must be positive")
    asyncio.run(benchmark(args))


if __name__ == "__main__":
    main()
