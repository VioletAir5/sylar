# 阻塞任务池

`BlockingExecutor` 在独立线程执行同步函数。调用它的 Fiber 通过 `IOManager::waitReadable` 等待完成信号，等待期间同一 I/O 线程可以运行其他 Fiber。阻塞任务的返回值和异常会回到调用者。任务完成与关闭取消都有测试。

```cpp
sylar::BlockingExecutor blocking(2);
io.schedule([&] {
    auto value = blocking.await(io, [&] {
        auto query = database.prepare("SELECT COUNT(*) FROM item");
        query.step();
        return query.columnInt64(0);
    });
    // 使用 value
});
io.run();
```

`await` 只能从该 `IOManager` 调度的 Fiber 调用。任务池析构时会等待已提交的任务完成；应在 `IOManager` 和被任务引用的对象仍存活时销毁。关闭 `IOManager` 会取消正在等待的 Fiber，但不能中断已开始执行的阻塞函数。SQLite 的同一连接即使启用互斥，也应由调用方协调事务范围；高吞吐数据库访问仍需要连接池或独立连接。
