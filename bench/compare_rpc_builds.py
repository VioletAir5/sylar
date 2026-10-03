"""Paired RPC comparison across builds/settings; no profiler attached."""
import argparse
from datetime import datetime
import hashlib
import http.client
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import threading

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from rpc_process import start, stop

parser = argparse.ArgumentParser()
parser.add_argument("--before-build", type=Path, default=Path("build-release"))
parser.add_argument("--after-build", type=Path, default=Path("build-release"))
parser.add_argument("--before-pollers", type=int, default=2)
parser.add_argument("--after-pollers", type=int, default=2)
parser.add_argument("--requests", type=int, default=20000)
parser.add_argument("--concurrency", default="16,32")
parser.add_argument("--rounds", type=int, default=5)
parser.add_argument("--modes", default="grpc-sync,fiber-worker,gateway-worker")
parser.add_argument("--trace-capacity", type=int, default=0)
parser.add_argument("--trace-export-interval-ms", type=int, default=0)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
levels = [int(value) for value in args.concurrency.split(",")]
modes = args.modes.split(",")
if not modes or any(mode not in ("grpc-sync", "fiber-worker", "gateway-worker") for mode in modes):
    parser.error("invalid comparison mode")
if not levels or any(not 1 <= n <= 32 for n in levels):
    parser.error("concurrency must be 1..32; backend business capacity is 32")
if not max(levels) <= args.requests <= 1000000 or not 1 <= args.rounds <= 10:
    parser.error("invalid request/round count")
if not all(1 <= n <= 32 for n in (args.before_pollers, args.after_pollers)):
    parser.error("poller retention must be 1..32")
builds = {"before": args.before_build.resolve(), "after": args.after_build.resolve()}
limits = {"before": args.before_pollers, "after": args.after_pollers}
if args.trace_export_interval_ms and (args.trace_export_interval_ms < 10 or args.modes != "gateway-worker" or not args.trace_capacity):
    parser.error("export stress needs gateway-worker, enabled tracing and interval >=10ms")
if not 0 <= args.trace_capacity <= 65536:
    parser.error("trace-capacity must be 0..65536")
def resources(process):
    folder = Path(f"/proc/{process.pid}")
    stat = (folder / "stat").read_text().split()
    status = dict(line.split(":", 1) for line in (folder / "status").read_text().splitlines() if ":" in line)
    return {"cpu_seconds": (int(stat[13]) + int(stat[14])) / os.sysconf("SC_CLK_TCK"),
            "rss_kib": int(status.get("VmRSS", "0").split()[0]),
            "threads": int(status["Threads"]), "fds": len(list((folder / "fd").iterdir()))}
records = []
metadata = {"recorded_at": datetime.now().astimezone().isoformat(),
            "kernel": platform.platform(), "cpu_affinity": sorted(os.sched_getaffinity(0)),
            "grpc": subprocess.check_output(["pkg-config", "--modversion", "grpc++"], text=True).strip(),
            "protobuf": subprocess.check_output(["protoc", "--version"], text=True).strip(),
            "builds": {}, "results": records,
            "note": "paired alternating before/after; same-host closed-loop; no profiling; "
                    "retries disabled; trace capacity recorded; server CPU includes client startup/warmup; peaks sampled",
            "trace_capacity": args.trace_capacity,
            "trace_export_interval_ms": args.trace_export_interval_ms}
for variant, build in builds.items():
    metadata["builds"][variant] = {
        "path": str(build), "max_pollers": limits[variant],
        "sha256": {name: hashlib.sha256((build / name).read_bytes()).hexdigest()
                   for name in ("sylar/catalog_server", "sylar/rpc_gateway", "bench/sylar_rpc_bench")}}
args.output.parent.mkdir(parents=True, exist_ok=True)
for concurrency in levels:
    for round_number in range(args.rounds):
        for mode in modes:
            for variant in (("before", "after") if round_number % 2 == 0 else ("after", "before")):
                build = builds[variant]
                children = []
                env = {"SYLAR_TRACE_CAPACITY": str(args.trace_capacity), "SYLAR_RPC_RETRIES": "0",
                       "SYLAR_RPC_MAX_INFLIGHT": str(concurrency), "SYLAR_RPC_NOTIFICATION": "worker",
                       "SYLAR_RPC_TIMEOUT_MS": "2000", "SYLAR_IO_DIAGNOSTICS": "1",
                       "SYLAR_GRPC_MAX_POLLERS": str(limits[variant])}
                try:
                    catalog, port = start(build / "sylar/catalog_server", 0, "paired", 0, env=env)
                    children.append(catalog)
                    endpoint = f"127.0.0.1:{port}"
                    client_mode = mode
                    if mode == "gateway-worker":
                        gateway, port = start(build / "sylar/rpc_gateway", 0, endpoint, env=env)
                        children.append(gateway)
                        endpoint = f"127.0.0.1:{port}"
                        client_mode = "http"
                    baseline = {p.pid: resources(p) for p in children}
                    peaks = {p.pid: dict(baseline[p.pid]) for p in children}
                    halt = threading.Event()
                    def sample():
                        while not halt.wait(.005):
                            for process in children:
                                try:
                                    current = resources(process)
                                    for key in ("rss_kib", "threads", "fds"):
                                        peaks[process.pid][key] = max(peaks[process.pid][key], current[key])
                                except OSError:
                                    pass
                    sampler = threading.Thread(target=sample)
                    sampler.start()
                    observation = {"exports": 0, "errors": 0, "bytes": 0}
                    def observe():
                        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
                        try:
                            while not halt.wait(args.trace_export_interval_ms / 1000):
                                connection.request("GET", "/traces")
                                response = connection.getresponse()
                                payload = response.read()
                                observation["exports"] += 1
                                observation["bytes"] += len(payload)
                                if response.status != 200:
                                    observation["errors"] += 1
                        except Exception:
                            observation["errors"] += 1
                        finally:
                            connection.close()
                    observer = threading.Thread(target=observe) if args.trace_export_interval_ms else None
                    if observer:
                        observer.start()
                    try:
                        run = subprocess.run(
                            [str(build / "bench/sylar_rpc_bench"), client_mode, endpoint,
                             str(args.requests), str(concurrency)],
                            capture_output=True, text=True, timeout=60)
                    finally:
                        halt.set()
                        sampler.join()
                        if observer:
                            observer.join()
                    result = json.loads(run.stdout)
                    result["trace_observer"] = observation
                    result.update(variant=variant, case=mode, round=round_number + 1, servers=[])
                    for process in children:
                        current = resources(process)
                        result["servers"].append({
                            "role": "catalog" if process == catalog else "gateway",
                            "cpu_seconds_including_client_warmup":
                                current["cpu_seconds"] - baseline[process.pid]["cpu_seconds"],
                            "peak_rss_kib": max(current["rss_kib"], peaks[process.pid]["rss_kib"]),
                            "thread_baseline": baseline[process.pid]["threads"],
                            "thread_peak": max(current["threads"], peaks[process.pid]["threads"]),
                            "fd_peak": max(current["fds"], peaks[process.pid]["fds"])})
                    records.append(result)
                    # Preserve measurements even if a later group fails.
                    args.output.write_text(json.dumps(metadata, indent=2) + "\n")
                    print(f"c{concurrency} {mode} {variant}: {result['rps']:.0f} rps, "
                          f"P99 {result['p99_us']:.0f} us, errors {result['failed']}", flush=True)
                    if run.returncode or result["failed"] or observation["errors"]:
                        raise RuntimeError("paired workload failed; raw results retained")
                finally:
                    for child in reversed(children):
                        stop(child)
print("Saved", args.output)
