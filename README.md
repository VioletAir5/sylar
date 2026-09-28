# sylar

借鉴 [sylar](https://github.com/sylar-yin/sylar) 的模块划分，以 C++23 重写的 Linux 协程 TCP 服务框架核心。日志、配置、Fiber、调度、定时器和网络 I/O 现在形成一条可运行、可测试的请求链路。

## 核心模块

- **日志与配置**：同步多 Sink 日志、格式化输出、YAML 配置与日志级别适配。
- **Fiber 与调度**：Boost.Context 有栈 Fiber；每个 I/O 工作线程独占 Scheduler，Fiber 不跨线程恢复。
- **事件与时间**：epoll 读写等待、eventfd 跨线程投递、单调时钟定时器与 I/O 超时。
- **TCP 服务**：非阻塞 IPv4 监听、连接分派、可配置空闲超时、SIGINT/SIGTERM 停机。echo 示例把上述模块串联起来。

架构与接口见 [协程网络架构](docs/architecture.md)、[日志](docs/logging.md)、[配置](docs/configuration.md)。

## 构建和运行

需要 CMake 3.23、C++23 编译器、fmt、yaml-cpp、Boost.Context、GoogleTest 和 Python 3。非系统路径下的 yaml-cpp 配置方法见[配置文档](docs/configuration.md)。

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/sylar/echo_server --config conf/echo.yml
```

也可以用 `./build/sylar/echo_server 9000 4` 指定端口与 I/O 工作线程数。按 Ctrl+C 或发送 SIGTERM 停止服务。示例配置见 [conf/echo.yml](conf/echo.yml)。

## 验证与性能

Release 构建的 52 项测试通过，包含 Fiber、socketpair、定时器、TCP 回环、跨线程投递和应用停机。C++ epoll 压测工具与复现命令见 [TCP echo 基准](docs/echo_benchmark.md)。在同一台 WSL 主机、4 个并行压测进程、1000 个连接、每轮 10 万次 64 字节回显的测试中，4 工作线程版本约 **31 万请求/秒**（10 轮中位数）；单线程版本约 **4.2 万请求/秒**（3 轮中位数）。压测端和服务端同机运行，该数据不代表独立压测机上的极限性能。

当前交付范围和已知限制见[开发计划](docs/roadmap.md)。目前支持 IPv4 TCP；HTTP、TLS、透明 I/O hook 不在这版核心范围。
