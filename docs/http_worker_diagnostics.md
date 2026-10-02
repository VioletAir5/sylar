# HTTP 工作线程诊断：为什么 4 worker 可能更慢

## 问题

[历史 HTTP 基准](http_benchmark.md)中，指定的小响应 keep-alive 负载下，4 worker 比 1 worker 慢。不能直接把这个结果归因于锁、epoll 或调度器；客户端和服务端同机，会共享 CPU。

## 新增诊断入口

设置 `SYLAR_IO_DIAGNOSTICS=1` 后，每个 IOManager 记录 post 数、待投递队列、fd 等待注册、`epoll_ctl` 调用、`epoll_wait` 调用/等待纳秒/返回事件及 eventfd 唤醒。`GET /metrics` 用 `worker="0"` 等标签输出各工作线程计数。禁用时保留零值快照，不在热路径做计数与计时；指标查询通过线程安全的原子快照，不暂停工作线程。

可复现命令：

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DSYLAR_BUILD_BENCHMARKS=ON
cmake --build build-release -j --target http_server sylar_http_bench
python3 bench/http_benchmark.py --server ./build-release/sylar/http_server --native-client ./build-release/bench/sylar_http_bench --workers 1 --clients 100 --requests 5000 --diagnostics
# 将 --workers 改为 4，交替运行多轮。
```

也可启动服务时设置环境变量，再抓取 `/metrics`；该端点本身会产生少量连接和 I/O 计数。`epoll_wait_nanoseconds_total` 包含空闲等待时间，不是 CPU 时间；`epoll_ready_events_total` 包含 eventfd 事件。`epoll_ctl_calls_total` 是尝试次数，不等同于成功注册数。

## 2026-10-02 的测量

环境：同一台 WSL2 主机，Release 构建，原生 epoll 客户端与服务端同机，100 条 keep-alive 连接，每条 5000 次请求；无固定 CPU 亲和性。交替运行 1/4 worker 各 3 轮，吞吐分别为：

| worker | 3 轮吞吐 req/s | 中位数 |
| ---: | --- | ---: |
| 1 | 88,897；92,823；89,908 | 89,908 |
| 4 | 69,215；70,892；88,217 | 70,892 |

4 worker 中位吞吐在这组负载下约低 21%，但第三轮接近 1 worker，说明环境或负载变化影响明显。一次启用诊断的 10 万请求测量中，1 worker 有约 1664 次 `epoll_wait`，4 worker 合计约 74,483 次；两者均约 20 万次 `epoll_ctl`。这是**批处理差异的线索**，尚不能证明它是吞吐差异的根因。

一次 4-worker `perf record -F 99 -g -e cycles:u` 采得 733 个样本。用户态热点包括 `HttpSession::readRequest` 约 7.33%、`HttpResponse::serialize` 约 7.01%、`parseHead` 约 6.13%、`malloc` 约 4.81% 和红黑树删除重平衡约 4.56%。这组样本没有内核态周期，也没有 1-worker 对照火焰图；不能据此宣称已经找出根因。更完整的实验应固定 CPU、预热、分离压测端与服务端并比较内核调度/系统调用开销。

## 后续决策

目前保留轮转分派与现有 IOManager 结构，不基于单次数据做侵入性优化。下一次优化应针对一处可重复的热点给出相同负载下的前后对照，尤其检查 HTTP 每次读等待的定时器注册/取消与四线程事件批处理。诊断数据只证明“发生了什么”，优化结论需要独立实验。
