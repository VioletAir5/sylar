"""Repeat a fixed read-only RPC workload; save raw results and process resources."""
import argparse
from datetime import date
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import threading
import time
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tests"))
from rpc_process import start,stop

parser=argparse.ArgumentParser()
parser.add_argument("--build",type=Path,default=Path("build-release"))
parser.add_argument("--requests",type=int,default=5000)
parser.add_argument("--concurrency",type=int,default=16)
parser.add_argument("--rounds",type=int,default=3)
parser.add_argument("--delay-ms",type=int,default=0)
parser.add_argument("--output",type=Path,default=Path("docs/performance/rpc_results.json"))
args=parser.parse_args()
assert 1<=args.rounds<=10 and 0<=args.delay_ms<=1000
build=args.build.resolve()
base_env={"SYLAR_TRACE_CAPACITY":"0","SYLAR_RPC_RETRIES":"0","SYLAR_RPC_MAX_INFLIGHT":str(args.concurrency),
          "SYLAR_IO_DIAGNOSTICS":"1"}
def resources(process):
    folder=Path(f"/proc/{process.pid}")
    stat=(folder/"stat").read_text().split()
    rss=0
    for line in (folder/"status").read_text().splitlines():
        if line.startswith("VmRSS:"):rss=int(line.split()[1])
    return {"cpu_seconds":(int(stat[13])+int(stat[14]))/os.sysconf("SC_CLK_TCK"),
            "rss_kib":rss,"fds":len(list((folder/"fd").iterdir()))}
records=[]
for round_number in range(args.rounds):
    modes=["grpc-sync","fiber-eventfd","fiber-worker","gateway-eventfd","gateway-worker"]
    # Alternate order to reduce systematic warm/order bias.
    if round_number%2:modes.reverse()
    for mode in modes:
        children=[]
        try:
            catalog,backend_port=start(build/"sylar/catalog_server",0,"benchmark",args.delay_ms,env=base_env)
            children.append(catalog)
            endpoint=f"127.0.0.1:{backend_port}"
            benchmark_mode=mode
            if mode.startswith("gateway"):
                gateway,gateway_port=start(build/"sylar/rpc_gateway",0,endpoint,
                    env={**base_env,"SYLAR_RPC_NOTIFICATION":"eventfd" if mode.endswith("eventfd") else "worker",
                         "SYLAR_RPC_TIMEOUT_MS":"2000"})
                children.append(gateway);endpoint=f"127.0.0.1:{gateway_port}";benchmark_mode="http"
            before={p.pid:resources(p) for p in children}
            peaks={p.pid:dict(before[p.pid]) for p in children}
            halt=threading.Event()
            def sample():
                while not halt.wait(.005):
                    for process in children:
                        try:
                            current=resources(process)
                            for key in ("rss_kib","fds"):peaks[process.pid][key]=max(peaks[process.pid][key],current[key])
                        except OSError:pass
            sampler=threading.Thread(target=sample);sampler.start()
            try:
                run=subprocess.run([str(build/"bench/sylar_rpc_bench"),benchmark_mode,endpoint,
                    str(args.requests),str(args.concurrency)],capture_output=True,text=True,timeout=60)
            finally:halt.set();sampler.join()
            assert run.returncode==0,(mode,run.stdout,run.stderr)
            result=json.loads(run.stdout)
            result.update(case=mode,round=round_number+1,delay_ms=args.delay_ms,servers=[])
            for process in children:
                current=resources(process)
                result["servers"].append({"role":"catalog" if process==catalog else "gateway",
                    "cpu_seconds_including_client_warmup":current["cpu_seconds"]-before[process.pid]["cpu_seconds"],
                    "peak_rss_kib":max(current["rss_kib"],peaks[process.pid]["rss_kib"]),
                    "fd_baseline":before[process.pid]["fds"],
                    "fd_peak":max(current["fds"],peaks[process.pid]["fds"])})
            records.append(result)
            print(f"{mode}: {result['rps']:.0f} rps, P99 {result['p99_us']:.0f} us, errors {result['failed']}",flush=True)
        finally:
            for process in reversed(children):stop(process)
report={"recorded_on":date.today().isoformat(),"kernel":platform.platform(),"compiler":subprocess.check_output(["c++","--version"],text=True).splitlines()[0],
    "grpc":subprocess.check_output(["pkg-config","--modversion","grpc++"],text=True).strip(),
    "protobuf":subprocess.check_output(["protoc","--version"],text=True).strip(),
    "cpu_count":os.cpu_count(),"note":"same-host loopback; closed-loop clients; 10 warmup calls per lane; tracing/retries disabled; server CPU includes warmup; fd peaks sampled",
    "results":records}
args.output.parent.mkdir(parents=True,exist_ok=True)
args.output.write_text(json.dumps(report,indent=2)+"\n")
print("Saved",args.output)
