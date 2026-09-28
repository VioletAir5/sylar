# SQLite3 适配

`SqliteDatabase` 管理连接，`SqliteStatement` 管理预编译语句并提供整数、浮点、文本、BLOB 与 NULL 的绑定和读取。两者使用 RAII，语句持有连接，避免数据库包装对象提前销毁。调用 `SqliteTransaction::commit()` 提交；未提交时析构自动回滚。预编译接口只接受单条 SQL 语句，业务数据应通过参数绑定传入。

```bash
./build/sylar/sqlite_demo
./build/sylar/sqlite_demo ./metrics.db
```

第二个命令重复执行时，`requests` 计数会继续增加。测试还覆盖内存数据库、类型化数据、异常 SQL、回滚与跨进程持久化。

SQLite3 调用本身是同步的。高并发请求中可用 [BlockingExecutor](blocking_executor.md) 把查询移到专用线程，并在 Fiber 中等待结果；仍需控制同一连接上的事务并发。本模块尚未提供连接池或 ORM。构建需要 SQLite3 开发包，例如 Ubuntu 的 `libsqlite3-dev`。
