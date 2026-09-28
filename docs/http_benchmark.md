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

这些数字证明当前实现能完成该负载并提供后续版本的同机对照。它们不代表服务端吞吐上限，也不支持四线程比单线程更慢的普遍结论。下一步应使用独立压测机、C++ 或成熟 HTTP 客户端、多轮预热与重复测量，并分析 HTTP 解析和每次 I/O 等待的开销。
