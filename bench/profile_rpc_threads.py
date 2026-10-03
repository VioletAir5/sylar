"""Count backend thread creation; strace timings are not performance results."""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from rpc_process import start

parser = argparse.ArgumentParser()
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--requests", type=int, default=1000)
parser.add_argument("--concurrency", type=int, default=4)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--max-thread-creations", type=int)
parser.add_argument("--max-pollers", type=int)
args = parser.parse_args()
if not shutil.which("strace"):
    parser.error("strace is required for the diagnostic")
if not 1 <= args.concurrency <= 16 or not args.concurrency <= args.requests <= 10000:
    parser.error("diagnostic requires 1..16 concurrency and <=10000 requests")
build = args.build.resolve()
env = {"SYLAR_TRACE_CAPACITY": "0"}
if args.max_pollers is not None:
    if not 1 <= args.max_pollers <= 32:
        parser.error("max-pollers must be 1..32")
    env["SYLAR_GRPC_MAX_POLLERS"] = str(args.max_pollers)
with tempfile.TemporaryDirectory(prefix="sylar-thread-profile-") as folder:
    counts = Path(folder) / "clones.txt"
    trace, port = start(
        "strace", "-f", "-c", "-e", "trace=clone,clone3",
        "-o", counts, build / "sylar/catalog_server", 0, "profile", 0,
        env=env)
    child = None
    try:
        child = int(Path(f"/proc/{trace.pid}/task/{trace.pid}/children").read_text().split()[0])
        run = subprocess.run(
            [str(build / "bench/sylar_rpc_bench"), "grpc-sync",
             f"127.0.0.1:{port}", str(args.requests), str(args.concurrency)],
            capture_output=True, text=True, timeout=60)
        workload = json.loads(run.stdout)
    finally:
        if child:
            os.kill(child, signal.SIGTERM)
        else:
            trace.terminate()
        try:
            trace.wait(timeout=10)
        except subprocess.TimeoutExpired:
            if child:
                os.kill(child, signal.SIGKILL)
            trace.kill()
            trace.wait()
            raise
    summary = counts.read_text()
    creations = errors = 0
    for line in summary.splitlines():
        fields = line.split()
        if fields and fields[-1] in ("clone", "clone3"):
            calls = int(fields[3])
            failed = int(fields[4]) if len(fields) == 6 else 0
            creations += calls - failed
            errors += failed
    report = {
        "build": str(build), "requests": args.requests,
        "concurrency": args.concurrency, "max_pollers_override": args.max_pollers,
        "thread_creations": creations,
        "thread_creation_errors": errors, "workload": workload,
        "strace_summary": summary,
        "note": "includes server startup/shutdown and 10 warmup requests per lane; "
                "ptrace perturbs scheduling; diagnostic timings are not benchmark evidence"}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Thread creations: {creations}, errors: {errors}, RPC errors: {workload['failed']}")
    if run.returncode or workload["failed"] or errors:
        sys.exit("diagnostic workload failed; exclude from performance conclusions")
    if args.max_thread_creations is not None and creations > args.max_thread_creations:
        sys.exit(f"thread churn exceeds diagnostic threshold {args.max_thread_creations}")
