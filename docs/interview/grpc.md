# gRPC 微服务升级面试准备

本材料对应实现 `0b957aa`。先实际运行 [RPC 教程](../grpc.md)，再练下面的讲述。你需要能画出调用链、解释所有权，并在现场修改超时参数验证行为。

## 简历表述

完成下述阅读和演示后，可根据自己实际参与的部分表述：

> 在 C++23 有栈协程服务器中接入可选 gRPC 模块，构建 HTTP 网关和双副本商品目录服务；通过 CompletionQueue 与 eventfd 将异步 RPC 接入 epoll，实现 owner 线程恢复、请求预算与取消传播、在途限额和限时排空，并以多进程故障测试验证副本退出、重连、超时与过载恢复。

只写能当场解释和复现的部分。原有 TCP echo 约 31 万请求每秒的结果属于旧测试链路，不能写成 gRPC 或微服务吞吐量。商品数据仍是只读 fixture；“微服务化演示”比“完整生产微服务平台”准确。

## 一分钟介绍

“我在 sylar 模块化思路的基础上，用现代 C++ 做了一个有栈协程网络框架。此次升级解决网关访问独立后端时的阻塞和资源积压问题。我用 Protobuf 定义目录查询契约，让 HTTP 网关通过复用的 gRPC channel 调用两个独立副本。

重点是异步完成与协程生命周期：一个 CQ 线程收完成结果，通过 eventfd 唤醒 epoll，Fiber 始终回到原线程；请求超时以后，RPC 对象仍保留到完成队列回收。调用带统一 deadline 和取消，网关对在途 RPC 直接限额拒绝。我用真实进程验证了服务死亡、重启、慢调用和停机。当前路由是静态轮询，没有服务发现、自动摘除或应用重试。”

## 五个工程问题与解决方法

| 问题 | 当前解法 | 源码或证据 |
| --- | --- | --- |
| 同步 Stub 调用阻塞 I/O 线程 | PrepareAsync/Finish 注册完成，Fiber 等 eventfd 后让出 | GrpcRuntime::unary；SlowRpcYieldsAndResumesOnOwnerThread |
| CQ 线程不能直接恢复有栈 Fiber | 只发 fd 通知，owner 的 epoll 恢复 Fiber | 四 worker 测试核对恢复线程 |
| 调用方超时后 reply 仍被写入 | 调用状态与 CQ tag 共享所有权，早返回不读 reply | DeadlineReleasesFiberAndDrainsCallState、CallStateOutlivesStoppedIOManager |
| 慢后端使请求无限积压 | 全局在途限额，名额到 CQ 完成才释放 | CapacityRejectsWithoutQueuingAndRecovers；进程矩阵 |
| 多副本故障和停机难以复现 | 自动启动真实进程，杀副本、重启、注入延迟及在途停机 | GrpcCluster.GatewayAndFailures |

这些是本项目的工程取舍，CQ、eventfd 和限额是成熟技术，不能宣称原创算法。面试特色来自与自己的运行时接入、清楚的约束和可复现证据。

## 重点追问

### 为什么引入 gRPC

跨进程调用需要明确的协议、错误状态和超时机制，Protobuf 提供类型化契约。选择成熟传输可以把本阶段重点放在与 Fiber 的接入和资源治理。成本是依赖、gRPC 内部线程和两套运行时之间的通知开销。

### 为什么每个调用使用 eventfd

它能直接复用现有 epoll wait、单调时钟超时和取消路径，CQ 不需要访问 IOManager 指针。代价是每次调用创建、注册和关闭 fd。可优化方向是 per-worker 完成队列配一个 eventfd，但必须处理批量通知、停止后的投递和 fd 复用；先做测量再改。

### 为什么限额占用到 CQ 回收

如果 Fiber 超时就立刻归还名额，下游仍未完成的调用会留在 gRPC 中，新请求继续进来，限额失去作用。因此统计代表 RPC 实际在途状态，而不是仍在等待的 Fiber 数。

### 为什么取消不一定立即结束业务

TryCancel 发起协作取消。目录服务检查 IsCancelled 后结束人工延迟。未来业务如果在不可取消的第三方函数里阻塞，仅客户端返回并不能终止它，需要该适配层的 timeout 或取消接口。

### 如何处理跨机器 deadline

调用方用自己的单调时钟计算剩余预算，再给 gRPC 设置期限。服务端没有读取调用方的 steady_clock 时间点。当前两跳没有手工转发多级 budget，未来服务之间调用需要继续传递剩余时间，并测试多跳耗时。

### 故障副本会自动摘除吗

当前不会。静态轮询选到死副本的请求失败，健康副本仍工作。重启后的连接恢复由 gRPC channel 负责。新增发现模块可以发布不可变端点快照；health checking、摘除、恢复探测和避免同时重试仍要分别设计。

### 为什么不直接重试

示例显式没有应用重试，保留原始故障用于验证。读请求可以在总 deadline 内做有限重试，但会放大流量；写请求必须先解决幂等和去重。服务发现、负载均衡、熔断、重试是不同职责。

### 网关和后端都异步吗

网关 RPC 等待是异步接入 Fiber。目录服务使用 gRPC 同步 handler，设置线程资源上限和业务并发准入。人工 delay 的 sleep 在后端 gRPC 工作线程执行，没有阻塞网关 I/O 线程。不能把它描述为端到端全部使用本框架协程。

### 如何证明高性能

当前证明了等待时不阻塞 owner，以及在过载和取消下的生命周期约束。尚无 RPC 吞吐量、P99 和分配次数的基准，不能用功能测试推出“比 gRPC 原生更快”。后续需要分开测直连 RPC、网关转发和故障负载，固定消息、连接、并发与机器环境。

## 五分钟现场演示

1. 在三个终端启动两个 Catalog 和网关，重复请求展示 instance_id 轮询。
2. 给请求加入 X-Request-Id，展示 HTTP 响应和后端回传字段一致。
3. 查询不存在的商品，解释业务 NOT_FOUND 映射到 404。
4. 停掉一个副本，展示部分 503 和健康副本 200；重启后展示恢复，并说明没有自动摘除。
5. 启动 400 ms 延迟服务，用 30 ms 头演示 504；访问 /health 和 /metrics，解释本地超时与 CQ 最终计数差别。
6. 运行 GrpcTest 与 GrpcCluster，展示取消、容量恢复、线程归属和停机的验证。

## 阅读与练习

先读 `proto/catalog.proto`，再读 `CatalogClient::lookup`、`GrpcRuntime::unary`、CQ 线程和 `shutdown`。最后读网关路由和真实服务测试。逐步画出 call 状态的三个所有者，指出哪个时刻可以安全释放 reply。

建议亲手完成三个小修改：把在途限额从 64 改为 1 并解释拒绝行为；增加一个只读 RPC 和测试；将双副本路由扩展为端点快照并设计移除期间的调用生命周期。最后一项尚未实现，练习时不要当成当前能力。

官方资料：[异步 C++ API](https://grpc.io/docs/languages/cpp/async/)、[deadline](https://grpc.io/docs/guides/deadlines/)、[协作取消](https://grpc.io/docs/guides/cancellation/)。实现事实以本仓库代码和测试为准。
