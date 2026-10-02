# 可重复的故障注入与恢复矩阵

故障测试以固定种子 `20261002` 运行本机代理，记录每次转发块大小。失败时输出种子与事件序列，可用 `python3 tests/test_http_fault_matrix.py ./build-core/sylar/http_server` 独立复现。测试基于隔离的回环进程，不依赖外部服务。Redis 项只在可选构建且存在 `redis-server` 时运行。

| 故障 | 预期响应 | 资源回收与恢复证据 |
| --- | --- | --- |
| Stream 每次读 1 字节、每次写 2 字节 | 完整解析 HTTP body 并写出合法响应 | `FaultStreamTest.FragmentedRequestAndHalfCloseRemainParseable` |
| HTTP body 提前 EOF | 明确的解析错误，不无限等待读取 | `FaultStreamTest.TruncationAndInjectedWriteFailFast` |
| Stream 第三次写入失败 | 异常立即结束写入 | 同上；独立后续请求仍可处理 |
| 本机代理按固定 1–7 字节分段请求，并每 3 字节延迟转发响应 | 仍返回 HTTP 200 | `HttpApp.DeterministicFaultMatrix`；固定块序列记录在测试输出 |
| 40 次客户端请求头中途 RST | 服务继续响应健康检查 | 同上；测试要求活动连接与 fd 回落。2026-10-02 一次运行的 fd 基线 11、结束 11 |
| 慢 WebSocket 对端停止读取 | 写入在截止时间内超时，预算归还，其他 Fiber 继续运行 | `OutboundBudgetTest.SlowSocketTimesOutWithoutStoppingOtherFiber` |
| 阻塞任务排队时截止时间到达 | 不执行已过期排队任务 | `BlockingExecutorTest.DeadlineSkipsQueuedOperation` |
| Redis 后端关闭 | 发布返回明确错误，无本地假成功 | `RedisCluster.BroadcastReplayAndGap` |

核心 Release 测试数量为 129，可选扩展全部开启为 133。当前故障矩阵证明了上述局部恢复行为；尚未测量故障时的 p99、长期 RSS 漂移，也不等价于跨机器高可用。进程排空的独立测试见 `HttpApp.AdmissionAndDrain`，包含限时关闭与在途请求完成。
