# 重点追问与回答骨架

回答建议按“触发条件 → 项目处理路径 → 取舍 → 验证”展开。下面是骨架，练习时要脱离文档在源码中找到对应位置。

## 第一组：请求链路，必须掌握

### 1. 一条连接怎样从 accept 到 HTTP 响应？

acceptor 调用 accept4，得到 O_NONBLOCK/CLOEXEC fd；先检查连接上限，随后由连接租约负责 fd 和活动计数。回调通过 IOManagerPool 投递到一个 worker，eventfd 唤醒其事件循环，Scheduler 创建并运行连接 Fiber。SocketStream 完成非阻塞读写，HttpSession 分帧，Servlet 生成响应，然后继续 keep-alive 或结束租约。

追问：按连接轮转，长连接留在原 worker；同一个 worker 内也可能有多个同时挂起的请求。看 [TcpServer](../../sylar/src/net/TcpServer.cpp)和[HttpServer](../../sylar/src/http/HttpServer.cpp)。

### 2. Fiber 保存了什么？为什么选有栈实现？

Boost.Context 保存恢复调用现场所需的底层上下文并切换独立栈；局部变量、嵌套函数的活动调用现场保留在该栈中。这样 SocketStream 内部能挂起，上层仍写顺序调用。Fiber 自身还维护状态、回调、异常和调度归属。

当前默认固定栈大小是 128 KiB。连接容量要同时考虑栈分配、fd、缓冲区和内核资源；不直接把默认栈大小与 RSS 或最大连接数画等号。看 [Fiber](../../sylar/src/fiber/Fiber.cpp)。

### 3. 项目用了 C++20/23 标准协程吗？

构建标准为 C++23，当前 Fiber 使用 Boost.Context，没有实现 co_await/promise_type 的标准协程运行时。标准协程与有栈调用现场是不同设计；切换技术会影响接口、调用方式和资源生命周期。

项目可讲的现代 C++ 用法包括 RAII、智能指针、span、optional、chrono、charconv 与 stop_source/stop_token。说出它们解决的具体问题，避免只列语法名称。

### 4. READY 和 HOLD 有什么差别？

执行中主动 YieldToReady 表示仍可运行，Scheduler 把 Fiber 放回 ready 队列。YieldToHold 表示等待外部条件，Scheduler 持有它直到 wake/cancel。TERM 正常完成；EXCEPT 保存异常并在调度边界报告。HOLD 没有通知源就会一直等待。

协作式调度没有强制抢占。长期 CPU 循环不 yield，会妨碍同线程其他连接。看 [Scheduler](../../sylar/src/fiber/Scheduler.cpp)。

### 5. recv 返回 EAGAIN 后发生什么？

SocketStream 注册该 fd 的读等待与 WaitState，按需要注册 Deadline 定时器和 stop callback，再挂起当前 Fiber。epoll 就绪后 finish 移除相应兴趣、记录结果并 wake Fiber；Fiber 恢复后再次 recv。EINTR 重试，recv 为 0 是 EOF，短读是合法进展。

epoll 就绪不会替应用收齐 HTTP 请求。看 [SocketStream](../../sylar/src/net/SocketStream.cpp)和[IOManager](../../sylar/src/fiber/IOManager.cpp)。

### 6. epoll 用 LT 还是 ET？为什么不直接说 ET 更快？

当前兴趣中没有 EPOLLET，使用水平触发。一次等待完成后框架移除相应兴趣，下次遇到 EAGAIN 再注册。不是内核 EPOLLONESHOT 模式。

改成 ET 要重新审视持续读写直到 EAGAIN、公平性和缓冲策略，并做相同负载对照；现有项目没有 ET/ LT 性能结论。不能把“采用 epoll”描述成“采用 ET”。

### 7. 为什么需要 eventfd？入队时有锁，eventfd 是多余的吗？

队列与互斥锁保护回调数据；eventfd 负责叫醒阻塞在 epoll_wait 的线程。IOManager 把待处理队列移到局部变量后释放锁，再创建调度任务。通知会合并，因此唤醒次数不等于任务数。

看 [post 与 drainPosts](../../sylar/src/fiber/IOManager.cpp)，结合 PostsAcrossWorkersAndWakesIdleEpoll 测试。

### 8. Fiber 为什么不迁移？没有锁吗？

本项目把 Scheduler、fd 等待和 TimerManager 固定在一个 owner thread。迁移 Fiber 需要同时处理这些状态与线程局部数据。连接建立时分派，现有连接不做 work stealing。

这减少线程内状态的共享，但 post 队列、日志 Sink、配置、路由以及共享预算仍有锁或原子同步。不能声称整个框架无锁。

## 第二组：生命周期与容量，是项目特色

### 9. 多次读写每次 30 秒，为什么还需要总 Deadline？

每次重置单次超时可能让完整请求远超期望时长。RequestContext 保存单调时钟截止时间，每步计算剩余预算；SocketStream/TlsStream 的等待取单次上限与剩余预算的较小值。

HTTP 当前创建 30 秒总 Deadline。超时可能关闭连接，未保证返回 408/504；CPU 函数不协作时也不会被定时器强行打断。看 [RequestContext](../../include/fiber/RequestContext.hpp)。

### 10. 超时、取消和就绪同时到达，谁赢？

事件在 owner IOManager 上处理，第一个完成当前等待的路径决定结果；后续路径找不到相同等待就无效。取消从其他线程到达时，先 post 回 owner，不能直接改其 epoll 和 Scheduler。

定时器和 stop callback 捕获共享 WaitState，finishIfCurrent 比较当前 slot.state。该校验防止旧取消误伤新的等待；它不是所有 fd 复用问题的统一证明。看 [finishIfCurrent](../../sylar/src/fiber/IOManager.cpp)。

### 11. shared_ptr 能解决全部生命周期问题吗？

它能表达共享持有，例如 held Fiber、等待状态和连接租约，但不会自动保证线程安全、消除环引用或修复不受其保护的引用捕获。fd 用 RAII，谁关闭必须清楚；borrowed Socket 不应重复 close。

追问：任务超时后 callback 仍可能运行，不能继续引用已经退出作用域的栈变量；用值捕获或正确管理的共享对象。看 [BlockingExecutor](../../include/fiber/BlockingExecutor.hpp)。

### 12. stop token 能取消正在执行的数据库查询吗？

排队但尚未执行的函数可被跳过；调用 Fiber 的等待可因 Deadline/取消结束。已开始的函数仍占用 blocking worker，必须依靠第三方客户端自己的超时、取消 API 或协作检查。

BlockingExecutor 析构 join 已提交工作，队列当前未设置通用硬上限。任务结束前其依赖对象必须仍存活。不能把“调用者返回”说成“下游工作已终止”。

### 13. 连接上限和请求上限的区别？

连接上限在 acceptor 创建 Fiber 前检查，保护 fd 和连接相关资源；超限关闭新 fd。HTTP 请求上限在解析后获取 permit，超限响应 503 与 Retry-After，并关闭连接。一个空闲连接不一定占 HTTP permit。

**按当前代码，permit 一直持有到 sendResponse 完成或异常退出。** 因此慢客户端会占在途数，并影响自适应窗口耗时。HTTP 升级 WebSocket 后走单独发送预算。看 [准入代码](../../sylar/src/http/HttpServer.cpp)。

### 14. 自适应并发算法如何工作？为什么称实验性？

64 个完成请求组成窗口；延迟高于历史最低窗口约 25% 且有负载时降低限额，饱和且未退避时小步增加，限制在 min/max。目标是过载时明确拒绝并降低同时执行的工作。

当前反馈包含 Servlet 与响应写入，历史基线可能受业务变化影响，窗口计数与耗时也是近似并发采样。只有一次演示对照，未证明稳定性和通用性能收益。默认固定上限。看 [RequestLimiter](../../sylar/src/http/RequestLimiter.cpp)。

### 15. epoll 不阻塞线程，为什么还要背压？

挂起的写 Fiber 仍持有 frame、栈和连接。当前写帧前预留全进程 64 MiB 默认预算，单会话拒绝重叠发送，慢写使用 2 秒 Deadline，异常释放租约。

这只约束出站帧预算，不覆盖内核 socket 缓冲、输入 fragment、TLS 缓冲或整个进程 RSS。没有异步广播队列。看 [WebSocket](../../sylar/src/http/WebSocket.cpp)与[OutboundBudget](../../sylar/src/http/OutboundBudget.cpp)。

### 16. 排空为何要先撤 readiness，再停 listener？

SIGUSR1 先让 readiness 503，完成的 HTTP 响应关闭 keep-alive；部署清单等待摘流传播。SIGTERM 再停止监听、限时等活动连接，之后停止 worker。已有请求可获得完成机会。

超限或到期仍可能中断请求；WebSocket 不迁移，当前也没有排空时的 1001 握手。进程存活 liveness 避免容量满额触发重启，但无法检测事件循环死锁。看 [排空设计](../admission_and_drain.md)。

## 第三组：协议、数据与分布式

### 17. TCP 分段如何处理？如何避免读到下一条请求？

HttpSession 保存未消费字节，解析 head 后按 Content-Length/chunked 获取本条 body，后续字节留给下一请求。readExact/writeAll 循环处理短读短写；head/body 有边界约束，并拒绝部分冲突头。

HTTP/1.1 支持范围受限，不宣称完整生产协议栈或 HTTP/2。能现场举出一个半请求和一次读到两请求的例子。看 [HTTP 分帧](../../sylar/src/http/Http.cpp)。

### 18. 配置监听器为什么在解锁后执行？

更新值并复制通知信息后释放锁，再调用用户 callback，减少重入读取/更新导致死锁的风险。变量本身的同步不意味着多个配置键一次加载原子生效。

监听器抛异常会中断后续通知；多个并发更新的通知顺序也需要业务明确约定。看 [ConfigVar](../../include/config/Config.hpp)。

### 19. 同步日志为什么没有 spdlog 快？

本项目按 Logger → Sink → Formatter 拆分，Sink 在格式化和写入时加锁。历史指定文件场景 spdlog 吞吐约高 4.6–6.1 倍。该结果覆盖完整同步写文件路径，未单独证实分配、格式化还是锁为主因。

改进顺序：测同一负载下 CPU、分配和锁等待，再选一个热点；增加异步队列时还必须设计容量、丢弃/阻塞、flush 和停机语义。当前没有异步日志。看 [日志基准](../benchmark.md)。

### 20. Kubernetes Service 能广播 WebSocket 消息吗？

现有长连接留在其选定副本；连接入口与消息分发是两层。项目的 Redis Streams 演示由每个订阅连接用独立 ID XREAD，让多个副本各自获取同一事件。

Kubernetes 清单尚未实机运行。跨副本测试起的是本机两个进程，不能推论节点故障和真实网络行为。看 [部署文档](../distributed_deployment.md)。

### 21. 为什么不把所有副本放进同一个 Redis consumer group？

同组消费者分担事件；广播需要各个订阅都读到事件，因此本演示使用独立 XREAD 游标。生产演进可每副本一个读取器，再向本地订阅有界扇出。

当前每副本最多 8 个阻塞订阅加一个发布线程，30 秒会话到期；还没有高并发 fanout 设计。看 [Redis 示例](../../sylar/app/redis_ws_server.cpp)。

### 22. Redis Streams 能保证不丢不重吗？

客户端以最后处理 ID 补读，但在处理与保存 ID 之间断线可能重复；ID 可用于去重。裁剪、Redis 数据丢失或读取与裁剪的竞态可能造成缺口。因此这里只证明保留窗口内的补读演示，不能保证所有发布消息最终被处理，也不宣称 exactly-once。

XRANGE 最老事件检查与 XREAD 不是原子操作；异常中断和删除 Stream 等场景还需完善。MAXLEN 限制条数，单条上限 1 MiB，Redis 总内存仍可能很高。看 [RedisStreamBus](../../sylar/src/redis/RedisStreamBus.cpp)。

## 第四组：性能、验证和个人贡献

### 23. 为什么 4 worker 的 HTTP 比 1 worker 慢？

当前只证实指定小响应负载下有这个现象。可能涉及客户端驱动、CPU 争用、事件批次、定时器与系统调用成本；它们都是待验证假设。一次诊断观察到 4 worker epoll_wait 次数明显增多，只是线索。

下一实验固定负载/CPU，分开压测端，交替重复，并对比用户态与内核态采样；一次只改变一个策略。不能用 echo 的结果证明 HTTP 已解决。看 [诊断记录](../http_worker_diagnostics.md)。

### 24. 31 万 req/s 和 129 个测试说明什么？

31 万属于历史 WSL 同机 64 字节 echo 指定负载的中位吞吐，不是业务 API QPS、独立压测机极限或当前任意版本性能。129 项测试证明已覆盖的一组行为；不证明无 bug、完整协议或生产可靠性。

故障测试记录 seed、输入与资源恢复，比只展示退出码更有解释力；短时间 fd/RSS 检查不能代替长时间稳定性。看 [故障矩阵](../fault_matrix.md)。

### 25. 项目有哪些原创部分？哪些借助现有库与 AI？

据实区分 sylar 模块思路、Boost.Context 底层机制、本项目组织的接口/等待/资源边界，以及 AI 辅助产出的代码与测试。把个人贡献限定在确实做过且能复现的工作中。

被要求现场修改时，先重述触发条件与期望行为，再改一个小接口或测试。能解释和维护的代码，比罗列不熟悉模块更有说服力。

### 26. 下一步你会改什么？

优先选择一个具体问题：例如 BlockingExecutor 有界提交队列、Redis 每副本单读取器与本地有界扇出，或 HTTP 多 worker 的定位实验。说明它保护哪项资源、超限返回什么、如何停机、用哪项对照验证。

若选择任务队列上限，讨论“立即拒绝”和“等待槽位”的差别，并确保等待也受 Deadline 约束。若选择性能优化，先拿出能重复的热点再改调度器。
