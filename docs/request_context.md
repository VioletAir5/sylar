# 请求级 Deadline 与取消

## 解决的问题

原有 `SocketStream` 和 `TlsStream` 每次等待各自拥有 30 秒超时。若一条请求被分成多次读写，整个请求可能持续超过 30 秒。`BlockingExecutor` 的调用 Fiber 若不再需要结果，已排队的同步工作仍可能继续消耗 worker。现在用一个请求上下文给这些步骤传递同一截止时间与协作取消信号。

## 接口与传播

- `RequestContext::withTimeout(duration)` 以单调时钟生成总 Deadline；`remaining()` 每次读都会递减，`cancel()` 可由其他线程调用。
- `IOManager::waitReadable/WaitWritable(fd, context)` 和带单次 timeout 的重载同时遵守总 Deadline 与取消。取消回调通过 `post()` 投递到 fd 所属 IOManager 线程，不能从外部线程直接修改 epoll。回调只有在登记的等待状态仍相同的情况下才取消，以免 fd 复用时误伤新等待。
- `Stream::setRequestContext` 将上下文交给 `SocketStream` 或 `TlsStream`；HTTP 服务器为每个普通 HTTP 请求建立 30 秒总 Deadline，并把同一对象放在 `HttpRequest::context`。WebSocket 升级后清除该请求上下文，继续使用已有的单次 I/O 超时。
- Servlet 如调用同步数据库或其他阻塞库，可使用 `BlockingExecutor::await(io, *request.context, operation)`。排队期间若过期或取消，调用 Fiber 立即返回错误，worker 跳过尚未开始的函数。

```cpp
routes->addExact("/item", std::make_shared<sylar::FunctionServlet>(
    [&](const sylar::HttpRequest& request) {
        auto value = blocking.await(
            *sylar::IOManager::GetThis(),
            *request.context,
            [&] { return databaseQuery(); }
        );
        sylar::HttpResponse response;
        response.body = value;
        return response;
    }
));
```

## 失败语义与边界

- Deadline 耗尽返回 `ETIMEDOUT`，显式取消返回 `ECANCELED`。HTTP 服务器在请求读取或写入超时时关闭该连接；目前不保证返回 408/504。
- 请求处理函数若正在执行**不可中断的同步第三方调用**，上下文不能杀掉该线程；必须给数据库/客户端自身设置超时并在任务逻辑里协作检查。运行中的函数完成前仍占据 blocking worker。
- 服务器不能在 Servlet 正同步阻塞时立即发现对端断连；下一次 socket I/O 才能观察到。需要主动断连传播时，应加入独立的连接监视机制并验证同一 fd 上并发读的协议安全性。
- 取消与 I/O 就绪同时发生时先处理的事件决定结果；状态检查保证旧取消回调不影响随后复用同一 fd 的等待。

## 验证

`tests/test_iomanager.cpp` 覆盖 Deadline、显式取消和 epoll 登记清理；`tests/test_stream.cpp` 覆盖请求总 Deadline 限制 socket 读取；`tests/test_blocking_executor.cpp` 覆盖到期/取消的排队函数不执行；`tests/test_http_server.cpp` 验证 Servlet 能得到请求上下文。完整 Release 回归按 README 的 CTest 命令运行。
