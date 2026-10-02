# 七天训练与模拟面试

每天建议 90–120 分钟。是否准备充分用“能解释、能修改、能验证”判断，测试数量只是证据之一。

| 天 | 读代码与练习 | 当天交付 |
| --- | --- | --- |
| 1 | 阅读 Fiber 与 Scheduler；手画 INIT/EXEC/READY/HOLD/TERM/EXCEPT。用已有 nested-call、HOLD 与 unwind 测试解释每次恢复 | 录制 60 秒介绍；不看文档解释 READY 和 HOLD |
| 2 | 跟踪 SocketStream 的 recv → EAGAIN → registerWait → YieldToHold → epoll → finish → wake → recv；找出每个对象的持有者 | 画一条成功请求与一条超时请求的调用时序 |
| 3 | 阅读 RequestContext、BlockingExecutor；解释排队取消和运行中取消。写一个小实验：超时返回后，下游函数继续完成 | 明确任务捕获对象的生命周期，能指出引用捕获风险 |
| 4 | 阅读 HttpSession、HttpServer；练习拆分 TCP 字节流和 HTTP 消息边界；对照 permit 的真实作用域 | 手写一个分段 Content-Length 请求测试，跑通 200/503 示例 |
| 5 | 阅读 echo/HTTP/log benchmark；复述参数、计时区间、样本数和不确定性 | 写一页性能结果解释，包含一个没有解决的反常现象 |
| 6 | 跑慢消费者、RST 和 Redis 双进程用例；区分副本、线程、连接与消息游标 | 录制 8 分钟演示，并说明每项演示证明了什么 |
| 7 | 完成下方模拟面试；挑答不清楚的三个问题回到代码 | 一页简历项目描述、一张架构图、三段问题故事 |

建议阅读顺序：[Fiber.cpp](../../sylar/src/fiber/Fiber.cpp) → [Scheduler.cpp](../../sylar/src/fiber/Scheduler.cpp) → [SocketStream.cpp](../../sylar/src/net/SocketStream.cpp) → [IOManager.cpp](../../sylar/src/fiber/IOManager.cpp) → [TcpServer.cpp](../../sylar/src/net/TcpServer.cpp) → [HttpServer.cpp](../../sylar/src/http/HttpServer.cpp) → [BlockingExecutor.hpp](../../include/fiber/BlockingExecutor.hpp)。

每次阅读记四件事：输入与输出、对象由谁持有、什么时候挂起/恢复、异常和关闭如何收尾。避免逐行背诵。

## 30 分钟模拟面试

| 时间 | 面试官问题 | 合格表现 |
| --- | --- | --- |
| 0–3 分钟 | 介绍项目和选择这个题目的原因 | 先讲解决的问题；架构与数据没有夸大 |
| 3–8 分钟 | 从 accept 到响应写完，画执行链路 | 说清线程归属、EAGAIN、挂起、事件恢复与再次系统调用 |
| 8–13 分钟 | 就绪与超时同时出现如何处理？取消返回后数据库还在执行怎么办？ | 解释 WaitState、owner post、协作取消和任务对象生命周期 |
| 13–18 分钟 | 为什么 4 worker 的 HTTP 更慢？31 万请求/秒说明什么？ | 区分证据与猜测；给出可检验的下一轮实验 |
| 18–23 分钟 | 慢客户端、队列增长、连接容量满额如何保护服务？ | 区分连接上限、permit、输出预算与尚未有界的队列 |
| 23–27 分钟 | Redis 广播能否保证不丢？为什么不用同一个消费者组？ | 解释游标、裁剪、重连重复与当前示例容量 |
| 27–30 分钟 | 让你修改一项设计，你会选什么？ | 选一个具体缺口，说明接口、失败语义和验证方法 |

## 自测打分

每项 0–2 分：0 为答不清，1 为能说概念，2 为能结合源码与测试解释。

- 请求执行链路和 Fiber 状态。
- fd、等待状态、定时器和连接对象的生命周期。
- epoll 水平触发、短读短写和 EAGAIN。
- 总 Deadline、显式取消与阻塞任务的边界。
- 连接容量、HTTP permit 和 WebSocket 输出预算。
- 性能实验的方法与未证实的根因。
- Redis 重连游标、保留窗口和副本隔离。
- 一个现场修改与可复现测试。

12/16 且前四项没有 0 分，可开始常规项目面试练习；这是练习标尺，不是招聘结果预测。分数低时减少简历主讲范围，先讲自己能解释的链路。

## 五个动手练习

1. 在 socketpair 上先发送半个 HTTP 请求，确认解析器等待；再补齐 body，验证响应。回答“TCP 分包”和“HTTP 分帧”的差别。
2. 在等待可读的 Fiber 上施加短 Deadline，断言登记清理；同一线程的另一个 Fiber 要能执行。
3. 使用 RequestLimiter 上限 1，让一个请求挂起，第二个收到 503；指出 permit 何时释放。
4. 将 WebSocket 对端发送缓冲区压小并停止读，观察 ETIMEDOUT、预算归零与其他 Fiber 运行。
5. 保留一个失败测试的输入、输出和参数，解释修复为什么有效；没有亲手发现的故障不要讲成自己的事故。

完成练习时建议自己写改动，再通过代码审查校对。把源码关闭后重写最小版本，比背诵答案更能暴露理解缺口。
