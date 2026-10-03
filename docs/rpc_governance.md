# RPC 治理：设计、运行与故障验证

本次升级完成七个方向：性能基准、端点摘除与探测、下游容量隔离、ZooKeeper 发现、有预算的只读重试、跨进程追踪、worker 完成通知。2026-10-03 在 Ubuntu 22.04 / GCC 13 Release 验证；[架构入口](grpc.md)、[性能记录](rpc_benchmark.md)、[面试讲述](interview/grpc.md)分别说明调用链、测量方法和工程问题。

## 1. 模块与接口

| 模块 | 对外操作 | 内部责任 |
| --- | --- | --- |
| `GrpcRuntime` | `unary`、`configureDownstream`、`shutdown` | CQ 生命周期、总容量、下游保留容量、通知适配、RPC span |
| `CatalogClient` | `lookup`、`updateEndpoints`、`endpointStatus`、`snapshot` | 只读重试、共享重试令牌、类型化响应 |
| `EndpointDirectory` | `update`、`select`、`observe`、`snapshot` | 原子端点快照、channel 复用、摘除、后台恢复探测 |
| `ZkDiscovery` | `watch`、`advertise` | 临时节点注册、周期发现、会话恢复、过期视图撤销 |
| `TraceRecorder` | `record`、`json` | 有界最近 span 环形记录 |
| `IOManager::Completion` | `notify`、`waitCompletion` | 跨线程一次性完成、超时取消、owner 生命周期保护 |

头文件位于 `include/rpc/`。业务端只需创建客户端并传入请求上下文：

```cpp
sylar::rpc::GrpcRuntime runtime(64);
sylar::rpc::CatalogClient::Options options;
options.downstream = "catalog";
options.capacity = 32;
options.max_retries = 1;
options.retry_budget = 4;
sylar::rpc::CatalogClient catalog(
    runtime, {"127.0.0.1:50051", "127.0.0.1:50052"}, options);
// 在所属 IOManager 的 Fiber 内调用：
auto result = catalog.lookup(io, context, 1);
```

应在开始流量前配置全部客户端与保留容量。`capacity=0` 保持旧接口的共享容量行为；显式保留容量总和不能超过 runtime 总容量。同一逻辑名称不能重配置为不同容量。runtime 析构前应停止外部调用者。

## 2. 下游隔离：慢服务不能借走快服务的名额

总限额约束实际在途 RPC，逻辑下游还有独立限额。配置两个下游各保留一个名额后，未保留的通用调用也不能消费这两个名额。否则“独立限额”只能约束慢服务自身，却不能防止其他调用耗尽整个 runtime。

准入没有应用等待队列，满额立即 `RESOURCE_EXHAUSTED`。Fiber 超时返回后，名额继续占用到 CQ 真正完成；取消不是完成证明。探测请求走目录自己的串行线程，不占用业务 runtime 名额。

网关设置 `SYLAR_RPC_SECONDARY_ENDPOINTS` 后增加 `/secondary/products/<id>`：总容量至少为 2，主目录分配向下取整的一半，第二目录得到余下容量。没有第二目录时主目录保留全部容量。`/metrics` 输出每个逻辑下游的 limit、inflight、rejected。

## 3. 故障摘除与恢复

默认累计两次 `UNAVAILABLE` 将该端点标记为 open，正常选择跳过 open 端点；普通成功会清零失败计数，其他状态保持计数。超时、取消、满额和业务错误不会触发摘除；它们可能是调用者预算太短或负载问题，不能证明机器死亡。

每个 CatalogClient 有一个探测线程。冷却 200 ms 后，以 100 ms deadline 查询只读商品 1；连续两次成功关闭 open 状态，失败重新等待冷却。探测线程不阻塞 Fiber 工作线程。析构请求停止并取消当前探测。恢复后重新进入普通轮询，当前没有渐进流量恢复策略。

端点集合通过不可变快照原子替换，保留地址复用 channel 与健康状态。已删除端点的在途调用仍持有原端点对象，不能在更新列表时销毁其 stub。空集合合法，业务返回不可用；单个快照最多 128 个不同地址。

`/endpoints` 可查看 service、address 和 open。`/ready` 检查网关未排空且主目录至少有一个非 open 端点；它不是每次请求的主动健康检查，也不检查第二目录。新发现的端点在收到失败前会被视为可选。

## 4. 有总预算的只读重试

网关默认最多追加一次尝试，CatalogClient 底层默认不重试。只能对只读 GetProduct 的 `UNAVAILABLE` 重试，并尽量换到另一个端点。业务错误、`RESOURCE_EXHAUSTED` 和超时不重试。客户端关闭 gRPC 自带重试，使应用能统一控制额外流量；机制说明见 [gRPC retry](https://grpc.io/docs/guides/retry/)。

一次逻辑请求共享原 deadline、取消信号和 request ID。退避通过 Completion 定时等待让出 Fiber，默认为 5 ms 加当前尝试序号毫秒；退避也受原预算和取消约束。没有额外端点时直接返回原错误。上限为两次追加尝试，不为每次重试重新生成 500 ms 预算。

每个 CatalogClient 共享重试令牌：默认初始 4 个，每次额外尝试消费一个，每次首次尝试成功补充一个，不超过初始容量；重试成功不补充。它约束连续故障时的放大，不表示固定百分比流量预算。没有随机抖动或跨进程统一令牌。监控提供 retries、retry_denied。

## 5. ZooKeeper 动态发现

同时开启 `SYLAR_WITH_GRPC` 与 `SYLAR_WITH_ZOOKEEPER`。注册结构如下：

```text
/sylar/services/catalog/                 持久父节点
  catalog-a                             临时节点，值为 host:port
  catalog-b                             临时节点，值为 host:port
```

实例使用实际监听端口注册，已有同名节点必须属于本会话且值一致；不会覆盖其他实例的临时节点。临时节点与会话语义参见 [ZooKeeper 编程指南](https://zookeeper.apache.org/doc/r3.8.4/zookeeperProgrammers.html)。

发现线程每约 100 ms 拉取一次完整集合并发布原子快照，所有 ZooKeeper 同步调用均在独立线程。当前是周期发现，未实现 znode watch。会话过期后创建新连接，服务重新注册。

注册中心失联期间允许短暂使用最近快照；独立过期线程在距最近成功刷新超过 1 秒后清空目录，即使刷新线程正在连接或阻塞也能撤销视图。业务及 readiness 随之不可用，`/health` 仍表示网关存活。新启动的发现网关使用空视图，不能通过静态参数绕过注册失败。过期检查周期约 50 ms，不是严格的实时保障。

本机启动示例：

```bash
# 先启动本机 ZooKeeper；开发依赖另需 libzookeeper-mt-dev。
cmake -S . -B build-rpc-zk -DCMAKE_BUILD_TYPE=Release \
  -DSYLAR_WITH_GRPC=ON -DSYLAR_WITH_ZOOKEEPER=ON
cmake --build build-rpc-zk -j
SYLAR_ZK_HOSTS=127.0.0.1:2181 ./build-rpc-zk/sylar/catalog_server 50051 catalog-a
SYLAR_ZK_HOSTS=127.0.0.1:2181 ./build-rpc-zk/sylar/catalog_server 50052 catalog-b
SYLAR_ZK_HOSTS=127.0.0.1:2181 ./build-rpc-zk/sylar/rpc_gateway 8080
```

容器需设置可被网关访问的 `SYLAR_ADVERTISE_HOST`，不是宿主默认 127.0.0.1。已有 [发现 Compose overlay](../deploy/compose.rpc.discovery.yaml)：

```bash
docker compose -f deploy/compose.rpc.yaml \
  -f deploy/compose.rpc.discovery.yaml build
docker compose -f deploy/compose.rpc.yaml \
  -f deploy/compose.rpc.discovery.yaml up -d
curl http://127.0.0.1:18080/endpoints
docker compose -f deploy/compose.rpc.yaml \
  -f deploy/compose.rpc.discovery.yaml down
```

本机实际测试使用隔离 ZooKeeper 进程，覆盖增删实例、SIGKILL 后临时节点消失、注册中心停止、过期视图、重启后的会话恢复。Docker 和多节点 ZooKeeper quorum 未在本机验收；示例沿用开放 ACL，没有增加注册鉴权。

## 6. 追踪：请求跨进程后还能找到父子关系

HTTP 接收 `traceparent`，网关生成 HTTP server span、每次 RPC 的 client span，Catalog 生成对应 server span。请求 ID 用于业务关联，trace ID 用于父子关系，两者不混用。当前严格支持 version 00、固定长度、小写十六进制和非零 trace/span ID；无效 HTTP 头返回 400。格式依据 [W3C Trace Context](https://www.w3.org/TR/trace-context/)，尚未支持 tracestate、baggage 或后续版本。

默认生成 sampled trace；收到 flags 的 sampled 位为 0 时仍传播上下文，不保存 span。span 记录 trace ID、span ID、parent span ID、name、kind、unix 起始时间、duration_us、status_code、owner_queue_us。status 是当前路径的状态值：RPC span 使用 gRPC code，HTTP span 使用 HTTP code。

```bash
curl -H 'traceparent: 00-11111111111111111111111111111111-2222222222222222-01' \
  http://127.0.0.1:8080/products/1
curl http://127.0.0.1:8080/traces
# 在启动后端时设置；收到 SIGTERM 正常停机后写文件：
SYLAR_TRACE_FILE=/tmp/catalog-traces.json ./build-grpc/sylar/catalog_server 50051 catalog-a
```

`/traces` 是网关最近 span 的 JSON，后端通过文件导出；两边用 trace ID 和 parent span ID 拼接。容量默认 4096，满时移除最旧记录并累加 dropped。容量为 0 关闭本地记录；已有 trace 仍可传播。停止前可抓取网关 JSON，停止后读取后端文件。

RPC duration 包含 channel 建连、传输和服务端等待，owner_queue_us 表示 CQ 完成至 Fiber 重新运行的等待，响应 server_elapsed_us 表示服务业务 handler 的耗时。没有单独测量纯连接建立时间。此实现为有界诊断追踪，不是 OpenTelemetry SDK 或 OTLP exporter；探测调用未加入业务 trace。

## 7. 默认 worker 通知：消除每调用 eventfd

CQ 完成后发布结果，将一次性 Completion 放入原 IOManager 的完成队列，用其已有 eventfd 唤醒。owner 线程批量处理完成通知，再恢复原 Fiber，不为每个完成创建新 Fiber。队列从空变为非空时才写唤醒 fd。

Completion 可以先通知后等待，重复 notify 被忽略；定时器、取消和完成只能结束一次等待。生命周期门控让 IOManager 销毁后的晚到通知安全失败。CQ 只发布结果和通知，不能直接在 CQ 线程恢复 Boost.Context。

`SYLAR_RPC_NOTIFICATION=eventfd` 保留旧模式用于对照；默认 `worker`。真实基准见 [RPC 性能](rpc_benchmark.md)：消除了每调用 epoll 注册，但本次吞吐量差异不足以证明所有负载更快。

## 配置速查

| 环境变量 | 默认 / 范围 | 使用者 |
| --- | --- | --- |
| SYLAR_RPC_MAX_INFLIGHT | 64 / 1..4096 | 网关 |
| SYLAR_RPC_TIMEOUT_MS | 500 / 1..2000 ms | 网关 |
| SYLAR_RPC_RETRIES | 1 / 0..2 | 网关 |
| SYLAR_RPC_RETRY_BUDGET | 4 / 0..1000 | 网关 |
| SYLAR_RPC_NOTIFICATION | worker / worker、eventfd | 网关 |
| SYLAR_RPC_SECONDARY_ENDPOINTS | 未设置 / 逗号分隔端点 | 网关 |
| SYLAR_TRACE_CAPACITY | 4096 / 0..65536 | 网关、后端 |
| SYLAR_TRACE_FILE | 未设置 / 正常停机写 JSON | 后端 |
| SYLAR_ZK_HOSTS | 未设置 / 注册中心地址 | 网关、后端 |
| SYLAR_ZK_PATH | /sylar/services/catalog | 网关、后端 |
| SYLAR_ADVERTISE_HOST | 127.0.0.1 | 后端 |
| SYLAR_LISTEN_ADDRESS | 127.0.0.1 | 网关、后端 |

网关头 `X-Request-Timeout-Ms` 的范围为 1..2000 ms，并受父请求剩余预算和 runtime 上限约束。静态端点仍可通过网关第二个命令行参数传入；启用发现后主目录使用发现视图。

## 验证与边界

| 构建 | 本机结果 | 条件 |
| --- | --- | --- |
| 核心 | 133/133 | 扩展关闭 |
| 核心 + gRPC | 152/152 | ZooKeeper 关闭、基准关闭 |
| 全部扩展 + gRPC + 基准 | 159/159 | Redis/MySQL/ZooKeeper 服务与 Java 可用 |

新增 4 项 Completion 测试、7 项治理 C++ 测试，gRPC C++ 共 18 项；进程故障矩阵覆盖两副本、摘除恢复、限额、超时、隔离和追踪；真实 ZooKeeper 测试验证会话与视图；基准另有小规模 smoke。

```bash
ctest --test-dir build-release \
  -R 'CompletionTest|GrpcTest|GrpcCluster|GrpcDiscovery|GrpcBenchmark' \
  --output-on-failure
```

本项目支持只读 unary 演示，没有写操作幂等与去重、流式 RPC、服务认证、mTLS、生产 ACL、跨机极限压测和正式可观测平台。Catalog 使用 gRPC 同步 handler，Fiber 异步适配在网关侧。新增线程包括每个 runtime 一个 CQ 线程、每个客户端一个探测线程，以及发现对象的刷新与过期线程；gRPC 自身也有内部线程。

## 追踪导出性能迭代

2026-10-03 调优增加不可变记录快照、锁外 JSON 编码及独立导出线程。网关最多两个执行/排队任务，满额 503；等待最多 500 ms，受父 deadline 和取消约束，超时 504。容量保留到实际完成/取消清理，停机先停止 I/O 使用者再结束执行器。追踪容量为 0 时不创建导出线程。

SYLAR_GRPC_MAX_POLLERS 是后端实验参数（1..32）；未设置时沿用 gRPC 默认值，本机版本为 2。扩大保留量的方案未采用为默认，不能通过减少线程创建次数推导吞吐量收益。

收益、内存与线程代价、失败方案及原始证据见[调优报告](performance/rpc_tuning.md)。最终回归 159/159，gRPC-only 152/152。
