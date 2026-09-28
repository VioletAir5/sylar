#!/usr/bin/env python3
"""Compare echo worker counts using parallel C++ load processes."""

import argparse
import os
import re
import select
import socket
import subprocess
import time


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def server_cpu(pid):
    with open(f"/proc/{pid}/stat", encoding="ascii") as stream:
        stat = stream.read()
    fields = stat[stat.rfind(")") + 2 :].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def peak_rss_kib(pid):
    with open(f"/proc/{pid}/status", encoding="ascii") as status:
        for line in status:
            if line.startswith("VmHWM:"):
                return int(line.split()[1])
    raise RuntimeError("VmHWM unavailable")


def run_case(args, workers):
    port = free_port()
    command = [args.server, str(port)]
    if workers:
        command.append(str(workers))
    server = subprocess.Popen(
        command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )
    try:
        readable, _, _ = select.select([server.stdout], [], [], 5)
        if not readable:
            raise RuntimeError("server did not report readiness")
        ready = server.stdout.readline()
        if str(port) not in ready:
            raise RuntimeError(f"server did not start: {ready!r}")
        warmup = subprocess.run(
            [args.client, str(port), "100", "10", str(args.bytes)],
            capture_output=True,
            text=True,
            timeout=15,
        )
        if warmup.returncode:
            raise RuntimeError(warmup.stderr)

        for repeat in range(1, args.repeats + 1):
            before_cpu = server_cpu(server.pid)
            start = time.perf_counter()
            clients = [
                subprocess.Popen(
                    [
                        args.client,
                        str(port),
                        str(args.clients_per_loader),
                        str(args.requests),
                        str(args.bytes),
                    ],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                for _ in range(args.loaders)
            ]
            try:
                for client in clients:
                    output, error = client.communicate(timeout=30)
                    if client.returncode:
                        raise RuntimeError(error or output)
                    expected = args.clients_per_loader * args.requests
                    if f"completed={expected}" not in output:
                        raise RuntimeError(output)
            finally:
                for client in clients:
                    if client.poll() is None:
                        client.kill()
                        client.wait()

            elapsed = time.perf_counter() - start
            cpu = server_cpu(server.pid) - before_cpu
            rss = peak_rss_kib(server.pid)
            total = args.loaders * args.clients_per_loader * args.requests
            print(
                f"workers={workers} run={repeat} completed={total} "
                f"rps={total / elapsed:.0f} wall={elapsed:.3f}s "
                f"server_cpu={cpu:.3f}s peak_rss={rss}KiB",
                flush=True,
            )
        server.terminate()
        if server.wait(timeout=3) != 0:
            raise RuntimeError(server.stderr.read())
    finally:
        if server.poll() is None:
            server.kill()
            server.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True)
    parser.add_argument("--client", required=True)
    parser.add_argument("--workers", default="0,4")
    parser.add_argument("--loaders", type=int, default=4)
    parser.add_argument("--clients-per-loader", type=int, default=250)
    parser.add_argument("--requests", type=int, default=100)
    parser.add_argument("--bytes", type=int, default=64)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    counts = [int(value) for value in args.workers.split(",")]
    if (
        not counts
        or any(value < 0 or value > 128 for value in counts)
        or min(
            args.loaders,
            args.clients_per_loader,
            args.requests,
            args.bytes,
            args.repeats,
        )
        < 1
    ):
        parser.error("invalid benchmark parameters")
    for workers in counts:
        run_case(args, workers)


if __name__ == "__main__":
    main()
