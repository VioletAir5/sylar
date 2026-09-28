# TCP echo 初始基准

## 运行

先编译 Release 版本，并在一个终端启动 `./build-release/sylar/echo_server 9000`。再运行：

```bash
python3 bench/echo_benchmark.py --port 9000 --clients 100 --requests 100 --bytes 64
python3 bench/echo_benchmark.py --port 9000 --clients 500 --requests 20 --bytes 64
```

脚本先建立所有连接，再让每个连接串行发送请求并验证回显，所有连接并发运行。耗时从连接完成后开始计，不含连接建立。延迟包含 Python asyncio 客户端、内核回环与服务端；每组总计 10000 次请求。

## 2026-09-28 初始结果

环境：同一台 WSL Ubuntu 主机上的回环 TCP；GCC 13.1.0，CMake Release，64 字节消息。单次运行，未控制 CPU 频率或后台负载。

| 并发连接 | 每连接请求 | 完成请求 | 吞吐（请求/秒） | p50 | p99 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 100 | 100 | 10000 | 12725 | 7.819 ms | 10.426 ms |
| 500 | 20 | 10000 | 12640 | 39.102 ms | 49.099 ms |

这两组数字只证明当前实现能处理这些并发连接并提供后续版本的同机对照。Python 客户端可能成为瓶颈；尚未测量服务端 CPU、内存和最大吞吐，也未对比其他框架。发布性能结论前，需要独立压测机、预热、重复运行和资源指标。
