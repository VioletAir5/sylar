# Redis 适配

RedisClient 使用 hiredis 管理连接，command 接受 string_view 参数列表，按长度传输，因此支持二进制数据。返回 RedisValue，区分状态、字符串、整数、空值和数组；Redis 错误回复会抛异常。示例：

    ./build/sylar/redis_demo 127.0.0.1 6379

集成测试启动临时、仅绑定本机且不持久化的 Redis 实例，验证各类应答后关闭实例，不依赖常驻服务。

hiredis 接口是同步阻塞的。应在 BlockingExecutor 中创建并使用连接，避免阻塞 IOManager 线程；一个连接上的命令应串行调用。本模块尚无连接池、TLS、认证配置或自动重连。构建需要 hiredis 开发包，集成测试需要 redis-server。