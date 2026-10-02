# Redis Streams 跨副本 WebSocket 演示

`redis_ws_server` 使用一个 Redis Stream (`sylar:room`) 连接两个独立 WebSocket 进程。每个订阅连接从自己的最后 ID 使用 `XREAD`，因此 A 和 B 副本都能收到同一条事件；这里不使用消费者组，因为组内消费者会分担事件。`XADD MAXLEN = N` 精确裁剪，默认保留最近 1000 条。

## 协议和复现

可选构建：`cmake -S . -B build-release -DSYLAR_WITH_REDIS=ON`，再构建 `redis_ws_server`。先启动本机 Redis，然后启动 `redis_ws_server 8081 6379` 和 `redis_ws_server 8082 6379`。连接各自的 `/ws`。

- 发送 `PUB hello`，收到 `ACK <stream-id>`；Redis 不可用时收到 `ERROR redis-unavailable`，本地不投递。
- 发送 `SUB 0-0` 读取当前保留窗口中的事件；收到 `EVENT <stream-id> <payload>`。重连时用最后处理的 ID，例如 `SUB 1720000000000-0`，可补读该 ID 后的事件。客户端应按 ID 去重。
- 请求的 ID 已早于当前最老事件时收到 `GAP replay-window-expired` 并关闭连接。上游需重新同步快照；框架不会猜测缺失消息。
- `SYLAR_STREAM_RETENTION` 可配置保留条数。每个副本最多 8 个同时订阅连接；第 9 个收到 `ERROR subscriber-capacity`。订阅会话 30 秒后结束，需要带最后 ID 续订。

验证：`ctest --test-dir build-release -R RedisCluster.BroadcastReplayAndGap --output-on-failure` 启动隔离的本机 Redis 与两个服务进程，测试跨副本广播、杀掉 B 后 A 继续发布、B 重启补读、裁剪缺口和 Redis 停止后的拒绝发布。

## 交付语义和容量边界

此演示是至少一次的应用级读取语义：`XREAD` 以 ID 前进，但客户端可能在收到事件后、保存 ID 前断线，因此重连可能重复。单个 Stream 内以 Redis ID 排序，没有跨房间全局顺序，也不保证客户端最终收到被裁剪的消息。Redis 是单点依赖；不可用时拒绝发布，订阅退出。

当前 hiredis 调用在 `BlockingExecutor` 中执行，最多 8 个阻塞订阅加一个发布线程。这个明确的上限让演示不占用 I/O worker，却不适合大量长期订阅；大规模部署应改为每副本一个 Redis 异步读取器和有界本地扇出队列，并复用 WebSocket 输出预算。断开的空闲订阅可能占用槽位直到 30 秒会话到期。此项目尚未做 Kubernetes 实机验收。
