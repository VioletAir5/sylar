# RPC 基准：完成通知与网关对照

测量时间：2026-10-03。原始 15 组结果保存在 [rpc_results.json](performance/rpc_results.json)。目标是验证 per-call eventfd 改为 worker 完成队列后的资源变化，并区分直接 RPC 与 HTTP 网关成本。

## 复现

需要已有核心依赖、gRPC 开发包和 Python 3：

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DSYLAR_WITH_GRPC=ON -DSYLAR_BUILD_BENCHMARKS=ON
cmake --build build-release -j
python3 bench/run_rpc_benchmark.py --build build-release \
  --requests 5000 --concurrency 16 --rounds 3 \
  --output docs/performance/rpc_results.json
```

非系统 yaml-cpp 需配置 `CMAKE_PREFIX_PATH`，见[配置文档](configuration.md)。脚本自动启动并停止回环后端与网关，使用临时端口，不依赖固定的已运行服务。每组使用新进程；第二轮反转模式顺序。`--delay-ms` 可增加业务延迟，测量慢服务下的队列和资源表现。

单独运行某种客户端：

```bash
./build-release/sylar/catalog_server 50051 bench
./build-release/bench/sylar_rpc_bench fiber-worker 127.0.0.1:50051 5000 16
# 第一参数也支持 grpc-sync、fiber-eventfd、http
```

工具源码为 [bench_rpc.cpp](../bench/bench_rpc.cpp)、[编排脚本](../bench/run_rpc_benchmark.py)。`GrpcBenchmark.Smoke` 使用每种模式 200 次请求、并发 4、1 轮，仅验证基准能运行及无错误，不设性能阈值。

## 方法

主机为同一台 WSL Ubuntu 22.04，GCC 13 Release，gRPC 1.30.2、Protobuf 3.12.4。测试无业务延迟，5000 次计时请求，并发 16，每条并发通道先预热 10 次，3 轮。性能测量单独运行，没有与构建或故障测试并行；所有 15 组错误数为 0。

| 模式 | 客户端 | 服务路径 |
| --- | --- | --- |
| grpc-sync | 16 个阻塞调用线程 | 直接 Catalog |
| fiber-eventfd | 2 个 IOManager worker、16 个 Fiber | 直接 Catalog，旧完成 fd |
| fiber-worker | 2 个 IOManager worker、16 个 Fiber | 直接 Catalog，worker Completion |
| gateway-eventfd | 16 条 HTTP keep-alive 连接 | 2 worker 网关 → Catalog，旧通知 |
| gateway-worker | 16 条 HTTP keep-alive 连接 | 2 worker 网关 → Catalog，worker 通知 |

各模式复用 channel 或 HTTP 连接。网关关闭 trace 记录和应用重试，启用 I/O 诊断。服务端使用 gRPC 同步 handler。负载是闭环：收到响应后才发下一个请求，因此过载下的尾延迟可能被低估；未做 coordinated omission 修正。不同客户端的线程模型不同，不能把模式差异全部归因于单个系统调用。

客户端记录 RPS、P50/P95/P99、计时窗口进程 CPU、RSS 高水位和 1 ms 采样 fd 峰值。服务端通过 /proc 每 5 ms 采样 CPU、RSS、fd；服务端 CPU 窗口包含客户端启动与预热，不能与客户端 CPU 直接相减。fd 峰值为采样值，极短暂尖峰可能遗漏。epoll 诊断计数包含预热，计时请求不包含预热。

## 实测：三轮中位数

| 模式 | 请求/秒 | P99 µs | 客户端 CPU 秒 | 客户端 fd 基线 → 峰值 |
| --- | ---: | ---: | ---: | --- |
| grpc-sync | 10674.59 | 2175.03 | 0.48 | 11 → 11 |
| fiber-eventfd | 10515.73 | 2133.59 | 0.41 | 15 → 31 |
| fiber-worker | 10362.97 | 2249.59 | 0.39 | 15 → 15 |
| gateway-eventfd | 9993.28 | 2352.14 | 0.14 | 20 → 20 |
| gateway-worker | 10139.26 | 2324.97 | 0.14 | 20 → 20 |

HTTP 模式表中的客户端是压测进程，不是网关。五种模式的客户端 RSS 高水位中位数均为 17984 KiB。

| 网关模式 | 网关 CPU 秒 | 网关 RSS 峰值 KiB | 网关 fd 基线 → 峰值 |
| --- | ---: | ---: | --- |
| gateway-eventfd | 0.58 | 13364 | 15 → 50 |
| gateway-worker | 0.52 | 13412 | 15 → 34 |

直接 Fiber eventfd 模式每轮有 10320 次 epoll_ctl：`(5000 + 16×10) × 2`，对应添加和删除 RPC 等待 fd；worker 模式为 0。该值只来自直接 RPC 客户端诊断，不包括 gRPC 库内部机制，也不代表整个系统没有 epoll 操作。

Catalog CPU 中位数约 1.44..1.52 秒，高于各客户端 CPU；商品逻辑很短，当前结果包含 gRPC 传输和同步 handler 的成本。原始记录还保留 P50/P95、每轮延迟和后端 RSS、fd。

## 如何解释结果

完成队列优化减少了 fd 与每调用 epoll 注册：直接 Fiber 峰值 31 → 15，网关峰值 50 → 34。网关保留的其他 fd 包括 HTTP 连接、监听、信号、工作线程与 gRPC 连接。

吞吐量和 P99 没有一致提升：直接 worker 稍慢、P99 稍高，网关 worker 稍快。只有 3 轮、同机 WSL、短时闭环测量，不能推导统计显著性，也不能承诺“协程比线程快”或生产极限 QPS。当前可支持的结论是减少通知资源开销，并在测量负载下保持相近吞吐量。

后续有价值的实验是独立压测机、固定 CPU/内核环境、长时间开环输入、增加并发与延迟、分别开启 tracing 和故障重试。这里记录了已有证据，未把这些实验标为完成。其他 TCP echo 的约 31 万请求/秒是另一协议与负载，不能用作本 RPC 的指标。

## 扩大请求量的复测

2026-10-03 又运行并发 16（5 轮）和 32（3 轮），每模式每轮 2 万次，共 80 万次计时请求、40 组全部零错误。[复测报告](performance/rpc_20261003.md)包含各轮范围、尾延迟、CPU/RSS/fd，以及两档原始数据；上文保留第一次 5000 请求短测。
