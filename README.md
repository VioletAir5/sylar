# sylar

借鉴 [sylar](https://github.com/sylar-yin/sylar) 的模块划分，以 C++23 重写的 Linux 协程服务器框架。日志、配置、Fiber、调度、定时器、网络 I/O 和 HTTP 已形成可运行、可测试的请求链路。

## 核心模块

- **日志与配置**：同步多 Sink 日志、格式化输出、YAML 配置与日志级别适配。
- **Fiber 与调度**：Boost.Context 有栈 Fiber；每个 I/O 工作线程独占 Scheduler，Fiber 不跨线程恢复。
- **事件与时间**：epoll 读写等待、eventfd 跨线程投递、单调时钟定时器与 I/O 超时；`BlockingExecutor` 把阻塞任务移出 I/O 线程。
- **Hook**：可选的 Fiber 感知 POSIX socket 读写、连接、接受连接与睡眠拦截；约束见 [Hook 文档](docs/hook.md)。
- **网络与序列化**：IPv4、IPv6、Unix 地址；TCP/UDP RAII Socket、Stream 与 ByteArray。
- **TCP 服务**：IPv4、IPv6、Unix 多地址非阻塞监听、连接分派、可配置空闲超时、SIGINT/SIGTERM 停机。
- **ZooKeeper**：会话连接与 znode CRUD、子节点查询；见 [ZooKeeper 文档](docs/zookeeper.md)。
- **MySQL**：预编译读写、二进制参数与本地隔离实例测试；见 [MySQL 文档](docs/mysql.md)。
- **Redis**：二进制安全命令、类型化应答与本地实例测试；通过阻塞任务池使用，见 [Redis 文档](docs/redis.md)。
- **SMTP**：SMTP 客户端、收件人应答校验、DATA 转义和隐式 TLS；见 [SMTP 文档](docs/smtp.md)。
- **SQLite3**：RAII 连接、预编译语句、事务与轻量单表 ORM；同步调用约束见 [SQLite 文档](docs/sqlite.md)。
- **HTTP/HTTPS 与 WebSocket**：HTTP/1.1 请求与响应、keep-alive、chunked 分帧、TLS 客户端与服务端、Servlet 路由，以及 ws/wss 握手和文本、二进制帧；HTTP 服务可在同端口升级 WebSocket。

架构与接口见 [协程网络架构](docs/architecture.md)、[日志](docs/logging.md)、[配置](docs/configuration.md)。

## 构建和运行

需要 CMake 3.23、C++23 编译器、fmt、yaml-cpp、Boost.Context、OpenSSL、SQLite3、hiredis、mysqlclient、ZooKeeper C 客户端、GoogleTest 和 Python 3。非系统路径下的 yaml-cpp 配置方法见[配置文档](docs/configuration.md)。

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/sylar/echo_server --config conf/echo.yml
./build/sylar/http_server 8080 4
./build/sylar/http_client http://127.0.0.1:8080/health
./build/sylar/ws_echo_server 8081 4
./build/sylar/sqlite_demo
```

也可以用 `./build/sylar/echo_server 9000 4` 指定端口与 I/O 工作线程数。按 Ctrl+C 或发送 SIGTERM 停止服务。示例配置见 [conf/echo.yml](conf/echo.yml)。HTTP 示例提供 /、/health 和 /echo，可用 `curl http://127.0.0.1:8080/health` 检查。HTTPS 可用 `http_server 8443 4 --tls cert.pem key.pem` 启动，客户端用 `http_client https://127.0.0.1:8443/health --ca cert.pem` 验证自签名证书；普通 CA 证书无需 `--ca`。

## 验证与性能

Release 构建的 100 项测试通过，包含 Fiber、socketpair、定时器、TCP 回环、跨线程投递和应用停机。C++ epoll 压测工具与复现命令见 [TCP echo 基准](docs/echo_benchmark.md)，HTTP/1.1 的独立测量见 [HTTP 基准](docs/http_benchmark.md)。在同一台 WSL 主机、4 个并行压测进程、1000 个连接、每轮 10 万次 64 字节回显的测试中，4 工作线程版本约 **31 万请求/秒**（10 轮中位数）；单线程版本约 **4.2 万请求/秒**（3 轮中位数）。压测端和服务端同机运行，该数据不代表独立压测机上的极限性能。

当前交付范围和已知限制见[开发计划](docs/roadmap.md)。原版 sylar 的完整模块对照与剩余工作见[模块状态](docs/module_status.md)。WebSocket 协议边界见[WebSocket 文档](docs/websocket.md)。其他外部服务集成仍在开发中；Hook 尚未覆盖全部 POSIX 调用。
