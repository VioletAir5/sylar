# 面试用：开发中遇到的 9 个问题

配套的简历、项目介绍、26 个追问与演示步骤见 [面试准备入口](interview/README.md)。

这些问题基于仓库中的设计、测试和测量。讲述时区分“已解决”“目前的设计约束”和“待解决”，不要把单机结果说成生产集群结论。

## 1. 有栈 Fiber 能否在另一个工作线程恢复？

**问题：** [Boost.Context](https://www.boost.org/library/latest/context/) 本身允许迁移 Fiber，但本项目的 fd 等待、定时器、Scheduler 和线程局部状态都归属某个 IOManager。若只移动 Fiber 而不迁移这些状态，恢复位置与事件归属会不一致。

**解决：** 每个 IOManager 独占 Scheduler、epoll 和 TimerManager；Fiber 固定由创建它的工作线程恢复。跨线程工作通过 `IOManagerPool::post` 投递回调，用 eventfd 唤醒目标线程。详见 [架构](architecture.md) 与 `tests/test_iomanager_pool.cpp`。

**面试追问：** “不能迁移”是本项目的设计约束，不是 Boost.Context 的通用限制；负载均衡发生在**新连接分派**时。

## 2. 工作线程阻塞在 epoll_wait 时，别的线程如何立即投递任务？

**问题：** 只把回调放入队列，目标线程可能一直睡在 epoll 中。

**解决：** 每个 IOManager 注册 eventfd；`post()` 入队并写 eventfd，事件循环被唤醒后执行待处理回调。关闭期间还要禁止或取消新投递，避免任务悬空。代码见 `sylar/src/fiber/IOManager.cpp`。

**验证：** 跨线程投递与停机测试；解释 eventfd 比忙轮询更适合空闲场景。

## 3. 30 秒超时为何会拖慢高频 echo？

**问题：** 最初每次 I/O 等待都创建、马上取消一个定时器。`perf` 中红黑树删除占用户态采样约 12.88%。

**解决：** echo 每条连接维护一个空闲期限，读写进展只更新时间戳；期限到达才检查是否续期。优化后同一热点符号在一次采样中约 0.08%。10 轮、每轮 10 万请求的同机测试，中位吞吐从约 301,516 到 314,212 req/s，约提升 4.2%。证据和限制见 [echo 基准](echo_benchmark.md)。

**面试追问：** 这是针对 echo 示例的优化，不代表所有 `SocketStream` 调用已经消除了定时器成本。

## 4. 同步数据库客户端会不会卡住整个 I/O 线程？

**问题：** SQLite/MySQL/Redis/ZooKeeper 的同步调用若直接放在 I/O Fiber 中执行，线程不会因为 Fiber 语法而自动让出，其他连接也会受阻。

**解决：** `BlockingExecutor` 将同步函数交给独立线程池；调用 Fiber 等待完成通知。SQLite 再通过固定大小连接池限制并发连接数。详见 [阻塞任务池](blocking_executor.md) 与 [SQLite](sqlite.md)。

**取舍：** 任务池关闭会等待已运行的函数；不能强制中断正在执行的第三方阻塞调用，需要设置其自身超时。

## 5. HTTP keep-alive、chunked 与 1xx 为什么容易出错？

**问题：** 流式 TCP 读可能一次拿到多个请求，错误的消息边界会使后续请求错位；客户端也不能把 100/103 当最终响应，HEAD/204/304 不应读取消息体。

**解决：** `HttpSession` 留存未消费字节，按 Content-Length 或 chunked 分帧，拒绝重复/冲突头；客户端跳过有限个临时响应，再读最终响应。最近补了 ASCII token、控制字节与 trailer 限制。见 `tests/test_http.cpp` 和 `tests/test_http_server.cpp`。

**面试追问：** 这是受限的 HTTP/1.1 实现，不要宣称覆盖完整协议、HTTP/2 或生产代理场景。

## 6. 为什么“4 线程比 1 线程慢”和“4 线程明显更快”都曾出现？

**问题：** Python 或单进程 C++ 客户端与服务端同机时，压测端可能先成为瓶颈；连接数、请求大小和是否并行驱动都会改变结果。

**解决：** 用原生 epoll 客户端、多个压测进程、固定请求数和多轮中位数记录条件，再用 perf 定位热点。echo 在指定 4 客户端负载下 4 工作线程表现较好；HTTP 小响应测试中 4 工作线程仍低于 1 线程，**原因尚未确认**。见 [echo 基准](echo_benchmark.md) 与 [HTTP 基准](http_benchmark.md)。

**面试表述：** 只报告负载和环境，不把同机数字当作框架上限；HTTP 的扩展性问题是后续 profiling 任务。

## 7. SIGTERM 与 fd 生命周期如何安全收尾？

**问题：** 直接停止 IOManager 会取消正在处理的 HTTP 请求；监听 fd 关闭时，epoll 中可能仍有等待。若先释放对象再取消等待，可能访问失效状态。

**解决：** HTTP 示例通过 signalfd 接收 `SIGUSR1`，先让 readiness 返回 503、后续响应关闭 keep-alive；收到 `SIGTERM` 才停止监听，限时等待活动连接归零，再停止 worker。连接 fd 和活动计数由 RAII 租约一起管理。见[连接容量与排空](admission_and_drain.md)及 `tests/test_http_drain_app.py`。

**限制：** 超过期限的 I/O 仍会被取消，WebSocket 不会迁移到新副本。

## 8. 多副本部署时，怎样证明它们真的独立？

**问题：** 单进程多线程只提高一台机器内的并发，无法证明副本隔离或节点故障后的可用性。

**解决：** HTTP 示例增加 `/health`、`/ready`、`/instance`；本机集成测试起两个进程，确认不同 ID，并在一个退出后检查另一进程。Kubernetes 清单定义 3 副本、Service、探针、滚动更新和尽力跨节点分散。见 [多副本部署](distributed_deployment.md)。

**限制：** 本机测试只覆盖进程级隔离；跨副本 WebSocket 广播另由 [Redis Streams 双进程演示](redis_stream_cluster.md)验证；真实节点故障与共享存储一致性尚未验收。

## 9. 连接满额时为什么不能让 Kubernetes 反复重启 Pod？

**问题：** 每连接一个 Fiber 和 fd 若不设限，会在高峰下持续耗尽资源。设置上限后，新连接被关闭；HTTP liveness 探针若也走同一监听端口，会把“已满但存活”误判为故障，形成重启风暴。

**解决：** acceptor 在创建 Fiber 前按活动连接数限流，满额关闭 fd 并累加拒绝计数；`/metrics` 暴露容量。readiness 仍走业务端口，以便满额时退出新流量；部署清单的 liveness 用进程存活检查，不因业务容量满额触发重启。见[连接容量与排空](admission_and_drain.md)。

**取舍：** 进程存活检查无法发现事件循环卡死。完整生产方案应设置独立管理端口或看门狗；当前清单没有证明真实集群中的摘流时延。
