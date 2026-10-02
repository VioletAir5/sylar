# 8 分钟现场演示

以下命令在 WSL Ubuntu 的仓库根目录执行。现有环境分别使用 `build-core` 和 `build-release`；后一目录配置了可选扩展。提前完成构建，现场以请求和测试演示为主。

## 会前准备

```bash
cd /home/wzt/sylar
cmake --build build-core -j4
ctest --test-dir build-core -N
git log -6 --oneline
```

新环境可按 README 安装依赖，再配置 `cmake -S . -B build-core -DCMAKE_BUILD_TYPE=Release`。若 yaml-cpp 位于自定义前缀，按 [配置文档](../configuration.md) 设置 CMAKE_PREFIX_PATH。

面试前打开 [已有架构动画](../architecture_demo.html)，以及 Fiber、IOManager、HttpServer 和测试源码。动画是设计说明；真实行为由下面的请求和测试佐证。

## 0–2 分钟：请求与容量

终端 A：

```bash
SYLAR_INSTANCE_ID=interview-a SYLAR_IO_DIAGNOSTICS=1 \
SYLAR_MAX_INFLIGHT_REQUESTS=2 SYLAR_DEMO_WORK_MS=200 \
./build-core/sylar/http_server 18080 2
```

终端 B：

```bash
curl -i http://127.0.0.1:18080/health
curl -s http://127.0.0.1:18080/instance
curl -s http://127.0.0.1:18080/metrics
```

解释：acceptor 分派连接，worker 处理请求，指标按 worker 展示。`epoll_wait` 总等待时间含空闲时间，不能当作 CPU 消耗。

## 2–4 分钟：超限与慢消费者

```bash
ctest --test-dir build-core --output-on-failure \
  -R 'HttpServerTest.RejectsOverlappingRequestsWith503|OutboundBudgetTest.SlowSocketTimesOutWithoutStoppingOtherFiber|IOManagerTest.RequestContextDeadlineAndCancellation'
```

对应讲解：在途请求超限返回 503；慢写只挂起连接 Fiber，但仍持有内存，输出预算和写 Deadline 负责约束；显式取消回到所属 IOManager 处理。测试成功不代表在所有负载下吞吐提升。

若要现场展示实际 503，可在同一个终端运行：

```bash
python3 - <<'PY'
import concurrent.futures
import urllib.error
import urllib.request

def request(_):
    try:
        with urllib.request.urlopen("http://127.0.0.1:18080/work", timeout=3) as response:
            return response.status
    except urllib.error.HTTPError as error:
        return error.code

with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
    print(list(pool.map(request, range(8))))
PY
```

预期有 200 与 503。若机器调度使某轮没有重叠，以确定性集成测试展示失败语义，不改口宣称固定拒绝比例。

## 4–6 分钟：故障和资源回收

```bash
python3 tests/test_http_fault_matrix.py ./build-core/sylar/http_server
```

解释固定种子、代理分段和延迟、40 次 RST、故障后的健康检查与 fd 回收。一次 fd 回到基线只验证短时用例，长时间 RSS 与故障时 p99 还要另测。

## 6–8 分钟：跨副本事件与边界

已准备可选 Redis 构建时：

```bash
ctest --test-dir build-release --output-on-failure \
  -R RedisCluster.BroadcastReplayAndGap
```

测试自动启动隔离 Redis 和两个进程，验证广播、一个副本退出后的另一副本可用、重启补读、裁剪缺口和 Redis 不可用时的拒绝。当前每订阅连接独立 XREAD，最多 8 个阻塞订阅/副本；不能把这个演示说成已经支撑大规模 WebSocket 集群。

没有可选依赖时，展示 [测试源码](../../tests/test_redis_stream_cluster.py)与[协议文档](../redis_stream_cluster.md)，明确当前机器未执行该环节。

完成后在终端 A 按 Ctrl+C。服务会停止监听并限时排空。

## 性能问题的备用演示

现场不必临时重新测所有历史结果。可打开 [echo](../echo_benchmark.md)、[HTTP](../http_worker_diagnostics.md)与[日志](../benchmark.md)记录，解释负载条件和统计方式。会前需要复测时：

```bash
python3 bench/compare_echo.py \
  --server ./build-release/sylar/echo_server \
  --client ./build-release/bench/sylar_echo_bench \
  --workers 0,4 --repeats 3
```

该命令需要构建时启用 SYLAR_BUILD_BENCHMARKS。它会启动自己的服务器与多个客户端，数据仍是同机测量。结果变化时保留新结果和环境，不为了复现旧数字选择性汇报。
