# HTTP/1.1 keep-alive 初步基准

构建 Release 版本后运行：

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
python3 bench/http_benchmark.py --server ./build-release/sylar/http_server --workers 4 --clients 100 --requests 100
```

压测脚本在回环地址启动 HTTP 示例，预先建立全部连接；每条连接串行请求 `/health`，验证状态码和响应体。计时不包含建连，每轮共 10,000 次请求。脚本记录每次往返延迟、服务端 CPU 时间和 VmHWM。以下数字是在同一台 WSL Ubuntu 主机上的单次串行运行，Python asyncio 客户端与服务端共享 CPU；没有预热、独立压测机或多轮统计。

| 工作线程 | 连接 | 每连接请求 | 吞吐 req/s | p50 | p99 | 服务端 CPU | VmHWM |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 100 | 100 | 15941 | 6.255 ms | 9.619 ms | 0.220 s | 7104 KiB |
| 4 | 100 | 100 | 11380 | 8.334 ms | 13.380 ms | 0.410 s | 6768 KiB |
| 4 | 500 | 20 | 9490 | 50.510 ms | 64.562 ms | 0.500 s | 11948 KiB |

这些 Python 客户端数据保留作为历史记录；客户端解释器开销使其不能直接代表服务端上限。

## 原生 epoll 客户端

使用仓库内的 C++ epoll 压测端，对固定的 `GET /health` 响应逐字节校验，每条 keep-alive 连接同一时刻只有一个未完成请求。先建立全部 TCP 连接，再开始计时。运行方式：

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DSYLAR_BUILD_BENCHMARKS=ON
cmake --build build-release -j --target sylar_http_bench http_server
python3 bench/http_benchmark.py \
  --server ./build-release/sylar/http_server \
  --native-client ./build-release/bench/sylar_http_bench \
  --workers 1 --clients 100 --requests 1000
# 将 --workers 改成 4，分别重复三次
```

2026-09-29 在同一台 WSL2 Ubuntu 主机测得：Linux 6.18.33.2-microsoft-standard-WSL2、AMD Ryzen 7 8845H（分配 16 个逻辑 CPU）、GCC 13.1.0、Release 构建。每轮 100 个连接、每连接 1000 次请求，共 10 万次；压测端与服务端共享 CPU。以下为三轮吞吐的中位数，括号为范围；延迟和资源列取中位吞吐对应的一轮。没有预热。

| 工作线程 | 吞吐中位数（范围） | p50 | p99 | 服务端 CPU | VmHWM |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 78,809.5 req/s（77,190.7–88,650.2） | 1,245.69 µs | 2,077.95 µs | 1.160 s | 7,160 KiB |
| 4 | 53,403.8 req/s（46,288.6–54,188.3） | 1,838.29 µs | 3,713.97 µs | 2.040 s | 6,868 KiB |

另外，在 500 个连接、每连接 200 次请求的一次探索性测量中，1 与 4 工作线程分别为 45,475.6 和 41,575.4 req/s。当前这个小响应、单机回环、每连接串行请求的负载下，增加工作线程没有提升吞吐；原因仍需 profiling 验证，不能据此推论其他负载的扩展性。服务端 CPU 统计从客户端建连前开始，吞吐计时从建连后开始，两者统计区间不同。后续应独立部署压测端，固定 CPU 亲和性，预热并增加重复次数；对 HTTP 解析、调度与 I/O 唤醒开销做 profiling。
