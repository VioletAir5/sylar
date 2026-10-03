# RPC 升级面试准备

基于 2026-10-03 本仓库实现。先读 [架构](../grpc.md)、[治理](../rpc_governance.md)和[基准](../rpc_benchmark.md)，再按照下面的问题到源码定位。材料中的功能已实现，性能数字只适用于记录的实验。

## 一分钟介绍

我在借鉴 sylar 模块划分的 C++23 有栈协程框架上增加了可选 gRPC 微服务模块，用 HTTP 网关调用独立商品目录副本。网关通过完成队列把 RPC 接回原 I/O 工作线程，等待期间 Fiber 让出，超时与取消共享请求预算。

我重点处理了跨线程生命周期、故障恢复与资源边界：调用状态持有到 CQ 完成，容量也在真正完成后释放；不同下游预留名额，慢服务不能耗尽快服务资源；只读请求有共享令牌约束的重试，端点故障后摘除并在后台探测恢复。ZooKeeper 用临时节点注册，过期发现快照会撤销 readiness，traceparent 串起跨进程父子 span。

最后保留旧通知机制作为对照。三轮同机基准中，worker 通知把直接 Fiber 客户端峰值 fd 从 31 降到 15，网关从 50 降到 34；吞吐量基本相近，不能宣称所有情况下更快。

代码由 AI 辅助实现，应据实说明自己的参与，至少能独立解释下面的接口、修改测试并复现结果。

## 七个可以深讲的问题

| 问题 | 解法与取舍 | 代码 / 验证 |
| --- | --- | --- |
| Fiber 已超时，gRPC 仍写响应 | 共享调用状态留到 CQ，超时路径不读未完成响应；TryCancel 不等于完成 | GrpcRuntime；deadline、stop、inflight 测试 |
| 每个 RPC 建 eventfd 和 epoll 等待开销 | worker Completion 队列，共用唤醒 fd、批量消费，生命周期门控 | IOManager；4 项 Completion 测试与双模式基准 |
| 慢下游占满总容量 | 显式下游保留名额，通用调用只能用未保留份额；配置在流量前完成 | configureDownstream；ReservedCapacityCannotBeConsumedByGenericCalls |
| 故障副本持续承接请求，恢复又迟迟无人知道 | 累计 UNAVAILABLE 摘除、成功清零，独立限时只读探测，连续成功恢复 | EndpointDirectory；EjectAndProbeRecoverEndpoint、进程重启 |
| 重试使故障流量翻倍，或越过原超时 | 只读 UNAVAILABLE、换端点、共享原 deadline、共享有限令牌 | CatalogClient；预算及放大测试 |
| 发现视图更新会销毁在途 stub；注册中心失联仍沿用旧地址 | 原子快照与共享 endpoint；独立过期线程清空 stale 视图；会话恢复重新注册 | ZkDiscovery；真实 ZooKeeper 增删、SIGKILL、停启测试 |
| 请求 ID 不能表达多次尝试的父子链 | trace ID + span ID + parent span ID，HTTP/RPC/server 分别记录 | Trace；父子校验、跨进程 JSON、sampled=0 测试 |

## 重点追问

**为什么 CQ 不直接 resume Fiber？**

Boost.Context 的 Fiber 属于原工作线程。CQ 只发布 done 与 Completion，owner 队列处理等待结果再调度原 Fiber。生命周期门控避免 IOManager 销毁后悬空访问。先完成后等待、重复通知和停机晚到通知都有测试。

**为什么局部容量还要“保留”？**

只有每服务上限时，未命名调用仍可能填满总容量。显式 reservation 从总容量扣除，让未保留调用不能消费这些名额。代价是某服务空闲容量暂时不能被其他服务借用；本版优先隔离，不实现动态借用。

**为什么只在 UNAVAILABLE 时摘除？**

调用者设置 1 ms deadline，也可能让健康机器超时；业务满额同样不是机器消失。只用传输不可用作为失败证据仍有局限，但可以减少误摘除。恢复探测串行、deadline 100 ms，不占业务名额；没有渐进恢复流量。

**重试预算是 20% 吗？**

不是。它是每客户端共享初始令牌，首次成功补一个、追加尝试消耗一个，约束连续故障放大。没有时间窗口百分比保证，也没有跨网关全局控制。写操作还需幂等键和去重持久化，当前业务是只读。

**ZooKeeper 是 watch 推送吗？**

不是。当前每 100 ms 周期刷新不可变视图，用独立线程在 1 秒过期时撤销；同步 SDK 调用不会阻塞 Fiber。临时节点与会话恢复已测，生产 ACL、quorum 故障和跨机网络没有验收。

**trace 能分离连接时间吗？**

当前 RPC duration 包含建连、网络、服务端等待，owner_queue_us 只测 CQ 完成到 Fiber 恢复。server_elapsed_us 是 handler 耗时，不能直接相减得到纯网络时间。JSON 导出能定位父子链，尚无 OTLP、tracestate 和 baggage。

**高并发和高性能凭什么？**

高并发设计由非阻塞 I/O、有栈 Fiber、线程归属、准入和有界资源支撑；性能要看具体实验。当前 RPC 约一万请求/秒的同机短测不能当生产上限，更不能使用 echo 的 31 万请求/秒代替 RPC。能证明的是 fd/epoll 注册减少以及故障场景的行为。

## 八分钟演示

1. 启动两个目录副本与网关，查询商品并观察 instance_id 和 request ID。
2. 杀掉一个副本，重复查询；追加只读重试转到健康副本，`/endpoints` 显示 open。重启后观察探测关闭 open。
3. 使用延迟后端和短头预算，解释 504；再通过隔离测试说明其他下游仍能成功。
4. 带固定 traceparent 查询，导出网关与后端 JSON，对照 trace ID 和父 span。
5. 运行 ZooKeeper 测试，说明注册中心失联、视图过期、ready/health 区别和会话恢复。
6. 展示原始基准数据，说明预热、三轮中位数、fd 采样、closed-loop 和同机限制。

```bash
ctest --test-dir build-release \
  -R 'CompletionTest|GrpcTest|GrpcCluster|GrpcDiscovery' \
  --output-on-failure
python3 bench/run_rpc_benchmark.py --build build-release \
  --requests 5000 --concurrency 16 --rounds 3 \
  --output /tmp/interview-rpc.json
```

实际讲述前亲手运行一次，准备指出 GrpcRuntime、EndpointDirectory、CatalogClient 各自负责什么。远端 CI、Docker 运行、服务认证、写事务和 OpenTelemetry 均不能说已经验收。
