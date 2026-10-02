# HTTP 请求级并发准入

`RequestLimiter` 在 HTTP 解析完成后、Servlet 执行前获取 RAII permit。超出上限时返回 503、`Retry-After: 1` 并关闭连接。TCP 连接硬上限仍独立生效；WebSocket 升级不使用 HTTP permit。permit 持有到响应写入结束、循环作用域退出或异常展开，因此覆盖 Servlet 执行、响应构造和发送；慢客户端写入也会占用在途名额并影响延迟反馈。

`SYLAR_MAX_INFLIGHT_REQUESTS` 默认 1024，上限 1000000。`SYLAR_ADAPTIVE_CONCURRENCY=1` 启用实验性的延迟反馈控制：每 64 个完成请求构成一个窗口；平均延迟相对最低窗口增长 25% 且负载高时限额缩小 20%，饱和而未触发退避时缓慢增加；限额保持在初始值的四分之一到初始值之间。默认固定模式。`/metrics` 提供在途数、限额、拒绝数、完成数。该控制器未在多种真实负载下验证稳定性。

复现命令：`python3 bench/request_limit_benchmark.py --server ./build-core/sylar/http_server --workers 4 --clients 64 --waves 20 --work-ms 20 --limit 16`。`SYLAR_DEMO_WORK_MS` 只在设置时启用 `/work` 演示路由，最大 1000 毫秒。

2026-10-02 WSL 回环单次示例：固定模式接受 320、拒绝 960、543.3 接受请求/秒、p99 49.83 ms；自适应模式接受 335、拒绝 945、571.2 接受请求/秒、p99 45.34 ms。压测端与服务端同机，且只有一次运行；结果仅验证脚本和失败语义，不能据此宣称自适应更快。进一步评估需重复轮次、混合路由、真实下游延迟、CPU 和 VmHWM。
