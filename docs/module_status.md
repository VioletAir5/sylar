# 模块状态

以 [上游 sylar README](https://github.com/sylar-yin/sylar) 的 13 个教程模块为基础对照。这里的“已实现”表示本仓库有可用接口及对应测试；不表示与上游 API 完全兼容。

| 模块 | 状态 | 本仓库实现 |
| --- | --- | --- |
| 日志 | 已实现 | 同步 Logger、格式化器、多 Sink |
| 配置 | 已实现 | YAML 类型化配置、变更监听 |
| 线程 | 部分 | 使用 C++ 标准同步原语和 IOManagerPool；无上游 pthread 包装 API |
| Fiber | 已实现 | Boost.Context 有栈 Fiber |
| 协程调度 | 部分 | 每工作线程独立 Scheduler；不提供跨线程迁移 Fiber |
| I/O 协程调度 | 已实现 | epoll、eventfd、定时器、取消和超时 |
| Hook | 部分 | 可选 POSIX socket 读写、连接、接受连接与睡眠拦截；调用范围见 [Hook](hook.md) |
| Socket | 已实现 | IPv4、IPv6、Unix 地址，TCP/UDP RAII Socket，选项与地址查询 |
| ByteArray | 已实现 | 固定宽度、Varint、字符串、文件读写 |
| TcpServer | 已实现 | 多地址监听、多线程分派、统一停机 |
| Stream | 已实现 | Stream 与 SocketStream |
| HTTP | 部分 | HTTP/1.1 服务端、客户端、keep-alive、chunked、TLS；已有初步 HTTP 基准，仍需完善协议边界与性能 |
| Servlet | 已实现 | 精确和前缀路由、FunctionServlet、404 回退 |

上游仓库简介还提到 WebSocket、HTTPS、SMTP、MySQL、SQLite3、ORM、Redis、ZooKeeper 等扩展。其中 HTTPS 已有证书验证的客户端与服务端，WebSocket 已有 ws/wss 服务端、帧解析与回显测试；SQLite3 已有预编译语句、事务与持久化测试。SMTP 已有客户端和本地协议测试；Redis 已有本地实例测试；MySQL、ORM、ZooKeeper 尚未移植。跨模块改动按协议基础、Hook 与生命周期、扩展协议、外部服务适配的顺序推进。

## 验收原则

每个新增模块至少满足：公开接口、回环或协议测试、异常和停机路径测试、Release 构建通过、README 的可运行示例。性能数据沿用 [TCP echo 基准](echo_benchmark.md) 的环境与方法记录；HTTP 单独建立基准，不直接沿用 echo 吞吐量。
