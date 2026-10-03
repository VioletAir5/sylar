# 模块状态

以 [上游 sylar README](https://github.com/sylar-yin/sylar) 的 13 个教程模块为基础对照。Redis、MySQL 和 ZooKeeper 默认不构建，可用同名 `SYLAR_WITH_*` CMake 选项分别开启。这里的“已实现”表示本仓库有可用接口及对应测试；不表示与上游 API 完全兼容。

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
| HTTP | 部分 | HTTP/1.1 服务端、客户端、keep-alive、chunked、TLS、HEAD、无内容响应和 1xx 客户端处理；已有 Python 与原生 epoll HTTP 基准，仍需完善协议边界与性能 |
| Servlet | 已实现 | 精确和前缀路由、FunctionServlet、404 回退 |

上游仓库简介还提到 WebSocket、HTTPS、SMTP、MySQL、SQLite3、ORM、Redis、ZooKeeper 等扩展。其中 HTTPS 已有证书验证的客户端与服务端，WebSocket 已有 ws/wss 服务端与客户端、HTTP 同端口升级、帧解析与回显测试；SQLite3 已有预编译语句、事务与持久化测试。SMTP 已有明文、隐式 TLS 与 STARTTLS 客户端及本地协议测试；Redis 已有本地实例测试；MySQL 已有预编译参数、RAII 事务和隔离实例测试；ZooKeeper 已有基础 znode API 与隔离实例测试；ORM 已有 SQLite 单表实体映射、CRUD、连接池和边界测试；尚无关系映射。跨模块改动按协议基础、Hook 与生命周期、扩展协议、外部服务适配的顺序推进。

## 验收原则

每个新增模块至少满足：公开接口、回环或协议测试、异常和停机路径测试、Release 构建通过、README 的可运行示例。性能数据沿用 [TCP echo 基准](echo_benchmark.md) 的环境与方法记录；HTTP 单独建立基准，不直接沿用 echo 吞吐量。

## 部署层

HTTP 示例现可无状态多副本运行；Kubernetes 清单提供三副本 Deployment、Service、探针与滚动更新设置。本机双进程测试覆盖副本身份与单副本退出后的独立可用性。镜像构建和真实 Kubernetes 运行尚未验收，已有 Redis Streams 跨副本 WebSocket 演示与重启补读测试，每副本最多 8 个阻塞订阅；共享状态一致性与无损连接迁移未实现。验证范围见 [Redis 双副本演示](redis_stream_cluster.md)与[多副本部署](distributed_deployment.md)。

## gRPC 微服务扩展

通过 `SYLAR_WITH_GRPC=ON` 开启，默认核心不依赖 gRPC。已有 versioned Protobuf 契约、独立 Catalog 服务、HTTP 网关、共享 channel 和 worker Completion 适配。请求携带 deadline、协作取消、request ID 与 traceparent；在途调用占用总容量与下游保留容量直到 CQ 完成。

已实现端点原子更新、UNAVAILABLE 摘除、后台限时探测恢复、共享预算的只读重试、ZooKeeper 临时注册与周期发现、有界最近 span JSON，以及直接 RPC / 网关双通知模式基准。18 项 gRPC C++ 测试、进程故障矩阵、真实 ZooKeeper 恢复和基准 smoke 已通过；2026-10-03 核心 133/133、仅 gRPC 152/152、所有扩展与基准 159/159。

服务端仍为 gRPC 同步 handler。尚无流式 RPC、写请求幂等与去重、服务认证、mTLS、生产 ACL 和 OTLP exporter。Compose 与 CI 已配置但本机未运行 Docker、远端 CI 未确认。架构见 [gRPC 教程](grpc.md)，行为与配置见 [治理手册](rpc_governance.md)，性能结论见 [RPC 基准](rpc_benchmark.md)。
