# RPC 调优：隔离可观测性开销

日期：2026-10-03。基线 bde7669，最终实现 5f80cd3，WSL2 Ubuntu 22.04 / GCC 13.1.0 / gRPC 1.30.2 Release。最终方案针对追踪开销；同时保留失败的线程参数实验。

## 最终完整链路结果

追踪容量 4096，worker 通知，默认 gRPC 轮询设置保持不变；并发 16，每组 30000 次商品请求，旧/新二进制交替 5 轮。一个持久 HTTP 连接持续读取 /traces，每次读取后等待 20 ms。这是较重的观测压力，不是空载吞吐量测试。

| 五轮中位数 | 前 | 后 |
| --- | ---: | ---: |
| 业务请求/秒 | 7326 | 7785 |
| 业务 P99 ms | 6.994 | 4.445 |
| 业务错误 | 0 | 0 |

吞吐量中位数提高 **6.3%**，P99 降低 **36.4%**；五个配对轮次均为新版本吞吐量更高、P99 更低。10 组、300000 次计时请求，业务与导出均无错误。[最终原始数据](rpc_export_tuning_paired.json)含二进制 SHA256、每轮指标与导出数量。

## 排查过程与被否定的方案

旧后端在 1000 次请求、并发 4 的 strace 诊断下创建 485 个线程，RPC 零错误；保留上限调至 8 后降至 12。计数含启动、停止和预热。strace 改变调度，不能用其耗时作为正式性能数字。

gRPC 1.30.2 同步服务默认一个 CQ、最低一个 poller、最多保留两个 poller，见[版本头文件](https://raw.githubusercontent.com/grpc/grpc/v1.30.2/include/grpcpp/server_builder_impl.h)。MAX_POLLERS 是轮询线程保留设置，不是整个服务的资源上限。[线程管理源码](https://github.com/grpc/grpc/blob/master/src/cpp/thread_manager/thread_manager.cc)解释退出与保留的关系，实际效果仍需在使用版本与负载上测量。

随后探索 2、4、8、16、32，分别对候选 32 和 8 做交替测量。部分负载吞吐量下降、P99 变差，因此**没有将它们设为默认**。SYLAR_GRPC_MAX_POLLERS 仅为可选实验参数，范围 1..32，未设置时沿用库默认；ResourceQuota 64 不变。线程少创建不等于请求更快。

perf 软件事件命令可运行，但本机 record 没有产生有效采样报告；没有声称采样证明某个函数为全链路主要热点。下面的结论来自针对性计数、竞争基准和完整链路对照。

## 修改一：traceparent 单次分配编码

合法 version 00 长度固定为 55 字节。原实现连续拼接字符串，新实现先按字段长度 reserve，再 append，输出一致。

基准链接实际 TraceContext::header，没有复制算法。分配计数为独立 10000 次调用；计时关闭计数，7 轮各 300 万次，交替运行独立链接的二进制。

| 局部指标 | 前 | 后 |
| --- | ---: | ---: |
| 每次普通 operator new 分配 | 2 | 1 |
| 每次编码 ns，中位数 | 35.32 | 19.97 |

[原始数据](trace_header_tuning.json)。此微基准没有随机 ID、记录或网络，不能推广为 RPC 加速。新增 GrpcTrace.AllocationBudget 只检查一次分配预算，不设时间阈值。仅编码优化的完整 RPC 对照差异小，未作为整体收益。

## 修改二：锁内固定快照，锁外编码

原 TraceRecorder::json 持互斥锁编码整个 ring，业务 record 必须等待。新实现保存不可变共享记录，锁内复制引用及 dropped 数值，锁外编码。ring 淘汰不会销毁正在导出的对象。

竞争基准的旧二进制已包含 header 编码改动、仍保留原导出锁，以隔离快照改动；完整 RPC 基线仍为 bde7669。容量 4096，一个持续 exporter，与按 100 µs 间隔记录 1000 条 span 的 producer 并发，交替 5 轮。

| record 延迟中位数 | 前 µs | 后 µs |
| --- | ---: | ---: |
| P50 | 1959.032 | 0.380 |
| P99 | 3338.406 | 38.109 |

[原始数据](trace_export_tuning.json)。这是 **record 等待**，不是 RPC P99。旧实现阻塞 producer，两版本完成 1000 次记录的总时长、累计导出次数不同；该实验验证临界区竞争，不是固定导出次数的 CPU 对比。

仅修改快照仍未改善完整网关 P99，因为 JSON 编码还运行在 I/O worker。这个中间结果也保留了。

## 修改三：诊断导出与 I/O 调度隔离

追踪开启时，网关创建一个独立 BlockingExecutor 线程。/traces 的 Fiber 等待结果，I/O worker 继续处理业务。导出最多两个在途任务，包含执行和排队，满额 503；等待最多 500 ms，并受父 deadline 与取消约束，超时 504。

名额保留到实际完成或排队任务取消清理，局部超时不能绕过容量。TraceExportLease 让 limiter 活到 permit 析构之后。停机先停止 I/O 使用者，再 join 导出执行器；晚到结果按现有 BlockingExecutor 的拥有状态回收。

```mermaid
flowchart LR
    D[/traces Fiber] --> A[准入: 最多两个任务]
    A --> E[独立导出线程]
    E --> S[锁内固定引用]
    S --> J[锁外 JSON 编码]
    E --> N[结果通知]
    N --> D
    B[业务 Fiber] --> R[record: 短临界区]
```

## 代价与验证边界

- 追踪开启时增加一个线程；样本网关 RSS 峰值中位数 19092→20144 KiB，约增加 1 MiB；峰值线程数 9→10、fd 35→36。
- 网关累计 CPU 秒中位数 5.32→5.88。服务端窗口含客户端启动/预热，两版本导出次数及墙钟时间不同，不能直接当作严格的每请求 CPU 效率。目标是观测压力下的业务延迟隔离。
- span 增加共享对象与控制块；导出需要 O(capacity) 引用快照，并延长被淘汰记录的生命期。网关最多两个导出任务约束快照占用。
- HTTP 响应序列化和发送仍在原链路，没有消除全部观测成本。生产部署还需配置采样与抓取频率。
- 同机 WSL、闭环、5 轮短测，未固定 CPU 亲和性或全部背景负载；绝对结果波动。表中 P99 为各轮 P99 中位数，未做统计显著性检验，不是生产保证。

## 复现

依赖和构建配置见[基准入口](../rpc_benchmark.md)。

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DSYLAR_WITH_GRPC=ON -DSYLAR_BUILD_BENCHMARKS=ON
cmake --build build-release -j
./build-release/bench/sylar_trace_bench 3000000 1
./build-release/bench/sylar_trace_export_bench

# build-before 应独立构建 bde7669；当前版本用 build-release
python3 bench/compare_rpc_builds.py \
  --before-build build-before --after-build build-release \
  --before-pollers 2 --after-pollers 2 --trace-capacity 4096 \
  --trace-export-interval-ms 20 --requests 30000 \
  --concurrency 16 --rounds 5 --modes gateway-worker \
  --output /tmp/rpc-export-comparison.json

ctest --test-dir build-release \
  -R 'GrpcTest|GrpcCluster|GrpcTrace|BlockingExecutorTest' \
  --output-on-failure
```

最终本机全扩展与基准 **159/159**，仅 gRPC **152/152**。新增并发导出/淘汰的 JSON 一致性测试和分配门槛，原超时、取消、停机、恢复与 discovery 回归通过。核心没有修改。

## 原始实验索引

| 数据 | 阶段 |
| --- | --- |
| [最终网关](rpc_export_tuning_paired.json) | 接受：编码、快照、独立有界导出 |
| [编码](trace_header_tuning.json)、[记录竞争](trace_export_tuning.json) | 接受方案的局部验证 |
| [候选32](rpc_tuning_candidate32.json) | 未设为默认：三种调用路径 |
| [参数探索](rpc_poller_sweep.json)、[候选8](rpc_tuning_candidate8.json) | 未设为默认：较小保留量 |
| [线程计数前](rpc_threads_before.json)、[后](rpc_threads_after.json) | 候选8诊断，不是最终默认 |
| [仅编码 RPC](rpc_trace_tuning_paired.json) | 局部收益不能代表整体 |
| [仅快照网关](rpc_export_snapshot_only.json) | I/O worker 仍受编码影响 |
| [补齐 lease 前](rpc_export_before_lease.json) | 中间实现，最终结果另列 |

bench/profile_rpc_threads.py 支持 --max-pollers 8 复现候选设置。strace 的耗时和可选线程数量门槛不作为跨平台 CI 性能要求。
