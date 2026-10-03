"""Real-process HTTP -> gRPC fault matrix; no external service required."""
import concurrent.futures
import http.client
import json
import os
import re
import select
import subprocess
import sys
import time

catalog_binary, gateway_binary = sys.argv[1:3]
children = []

def start(binary, *args, env=None):
    process = subprocess.Popen(
        [binary, *map(str, args)], stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, env={**os.environ, **(env or {})})
    children.append(process)
    deadline = time.monotonic() + 5
    lines = []
    while time.monotonic() < deadline:
        if select.select([process.stdout], [], [], .1)[0]:
            line = process.stdout.readline()
            lines.append(line)
            match = re.search(r"listening on port (\d+)", line)
            if match:
                return process, int(match.group(1))
        if process.poll() is not None:
            break
    raise AssertionError("startup failed: " + "".join(lines))

def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            code = process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise AssertionError("graceful shutdown exceeded 3 seconds")
        assert code == 0, ("shutdown failed", code, process.stdout.read())

def request(port, path="/products/1", headers=None):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    try:
        connection.request("GET", path, headers=headers or {})
        response = connection.getresponse()
        return response.status, dict(response.getheaders()), response.read().decode()
    finally:
        connection.close()

def stats(port):
    code, _, body = request(port, "/metrics")
    assert code == 200
    return {name: int(value) for name, value in (line.split() for line in body.splitlines())}

def eventually(predicate, timeout=6):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.02)
    raise AssertionError("condition did not become true")

try:
    for value in ("0", "33", "invalid"):
        invalid = subprocess.run(
            [catalog_binary, "0", "invalid-config"],
            capture_output=True, text=True, timeout=3,
            env={**os.environ, "SYLAR_GRPC_MAX_POLLERS": value})
        assert invalid.returncode != 0, value
    a, a_port = start(catalog_binary, 0, "catalog-a", 0)
    b, b_port = start(catalog_binary, 0, "catalog-b", 0,
        env={"SYLAR_GRPC_MAX_POLLERS": "8"})
    gateway, port = start(gateway_binary, 0, f"127.0.0.1:{a_port},127.0.0.1:{b_port}")
    instances = set()
    trace = "00-0123456789abcdef0123456789abcdef-0123456789abcdef-01"
    for _ in range(4):
        code, headers, body = request(port, headers={"X-Request-Id": "demo-123", "traceparent": trace})
        assert code == 200, body
        value = json.loads(body)
        assert value["request_id"] == "demo-123"
        assert value["price_cents"] == 19900
        assert headers["X-Request-Id"] == "demo-123"
        instances.add(value["instance_id"])
        assert value["trace_parent"].split("-")[1] == trace.split("-")[1]
        assert headers["traceparent"].split("-")[1] == trace.split("-")[1]
        assert value["server_elapsed_us"] >= 0
    assert instances == {"catalog-a", "catalog-b"}
    spans = json.loads(request(port, "/traces")[2])["spans"]
    by_id = {span["span_id"]: span for span in spans}
    assert any(span["kind"] == "rpc.client" and span["parent_span_id"] in by_id for span in spans)
    assert request(port, headers={"traceparent": "invalid"})[0] == 400
    assert request(port, "/products/9")[0] == 404
    assert request(port, "/products/bad")[0] == 400
    assert request(port, headers={"X-Request-Timeout-Ms": "99999"})[0] == 400
    assert request(port, "/health")[0] == 200
    a.kill()
    a.wait()
    codes = [request(port)[0] for _ in range(6)]
    assert codes == [200] * 6, codes
    assert any(item["open"] for item in json.loads(request(port, "/endpoints")[2])["endpoints"])
    assert request(port, "/health")[0] == 200
    a, _ = start(catalog_binary, a_port, "catalog-a", 0)
    def recovered():
        codes = [request(port)[0] for _ in range(2)]
        view = json.loads(request(port, "/endpoints")[2])["endpoints"]
        return codes == [200, 200] and not any(item["open"] for item in view)
    eventually(recovered)
    stop(gateway)
    stop(a)
    stop(b)
    print("PASS round robin, metadata, validation, replica failure and reconnect")

    slow, slow_port = start(catalog_binary, 0, "catalog-slow", 400)
    gateway, port = start(gateway_binary, 0, f"127.0.0.1:{slow_port}",
                          env={"SYLAR_RPC_TIMEOUT_MS": "1000", "SYLAR_RPC_MAX_INFLIGHT": "1"})
    # Establish the channel before timing/fault tests.
    assert request(port)[0] == 200
    for _ in range(3):
        code, _, body = request(port, headers={"X-Request-Timeout-Ms": "30"})
        assert code == 504, body
        eventually(lambda: stats(port)["sylar_rpc_inflight"] == 0)
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        first = executor.submit(request, port)
        eventually(lambda: stats(port)["sylar_rpc_inflight"] == 1)
        assert request(port)[0] == 503
        assert request(port, "/health")[0] == 200
        assert first.result(timeout=3)[0] == 200
    eventually(lambda: stats(port)["sylar_rpc_inflight"] == 0)
    assert stats(port)["sylar_rpc_rejected_total"] >= 1
    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
        active = executor.submit(request, port)
        eventually(lambda: stats(port)["sylar_rpc_inflight"] == 1)
        stop(gateway)
        # Graceful drain lets this short call finish.
        assert active.result(timeout=3)[0] == 200
    stop(slow)
    print("PASS deadlines, capacity, health isolation, recovery and active drain")
    slow, slow_port = start(catalog_binary, 0, "isolated-slow", 300)
    fast, fast_port = start(catalog_binary, 0, "isolated-fast", 0)
    gateway, port = start(gateway_binary, 0, f"127.0.0.1:{slow_port}",
        env={"SYLAR_RPC_SECONDARY_ENDPOINTS": f"127.0.0.1:{fast_port}",
             "SYLAR_RPC_MAX_INFLIGHT": "2", "SYLAR_RPC_TIMEOUT_MS": "1000"})
    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
        active = executor.submit(request, port)
        eventually(lambda: stats(port)["sylar_rpc_inflight"] == 1)
        assert request(port)[0] == 503
        assert request(port, "/secondary/products/1")[0] == 200
        assert request(port, "/health")[0] == 200
        assert active.result(timeout=3)[0] == 200
    stop(gateway)
    stop(slow)
    stop(fast)
    print("PASS named downstream isolation and distributed trace context")

finally:
    for child in reversed(children):
        if child.poll() is None:
            child.kill()
            child.wait()
