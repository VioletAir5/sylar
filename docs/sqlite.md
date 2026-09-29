# SQLite3 适配

`SqliteDatabase` 管理连接，`SqliteStatement` 管理预编译语句并提供整数、浮点、文本、BLOB 与 NULL 的绑定和读取。两者使用 RAII，语句持有连接，避免数据库包装对象提前销毁。调用 `SqliteTransaction::commit()` 提交；未提交时析构自动回滚。预编译接口只接受单条 SQL 语句，业务数据应通过参数绑定传入。

```bash
./build/sylar/sqlite_demo
./build/sylar/sqlite_demo ./metrics.db
```

第二个命令重复执行时，`requests` 计数会继续增加。测试还覆盖内存数据库、类型化数据、异常 SQL、回滚与跨进程持久化。

SQLite3 调用本身是同步的。高并发请求中可用 [BlockingExecutor](blocking_executor.md) 把查询移到专用线程，并在 Fiber 中等待结果；仍需控制同一连接上的事务并发。本模块尚未提供连接池。构建需要 SQLite3 开发包，例如 Ubuntu 的 `libsqlite3-dev`。

## 轻量实体映射

`SqliteRepository<Entity, Fields...>` 把一个含 `int64_t id` 的实体映射到单表。构造时显式列出列名和成员，`createTable()` 建表，`save()` 按主键插入或更新，`find()` 查询，`erase()` 删除：

```cpp
#include "db/SqliteOrm.hpp"

struct User {
    std::int64_t id = 0;
    std::string name;
    std::optional<std::int64_t> age;
};

auto db = sylar::SqliteDatabase::open("users.db");
sylar::SqliteRepository<User, std::string, std::optional<std::int64_t>> users(
    db, "users", &User::id,
    sylar::sqliteColumn("name", &User::name),
    sylar::sqliteColumn("age", &User::age));
users.createTable();
users.save({1, "Alice", std::nullopt});
auto user = users.find(1);
```

字段支持整数、浮点、字符串、BLOB（`std::vector<std::byte>`）及这些类型的 `std::optional`；整数转换会检查范围。表名和列名仅接受 ASCII 字母、数字及下划线，首字符不能是数字；重复列名按 ASCII 大小写不敏感规则拒绝。数据值始终使用预编译语句绑定。映射对象引用数据库连接，必须在连接销毁前销毁；连接及映射对象不提供并发共享保证。此接口专注单表 CRUD，不提供关系映射、迁移、查询构造器或连接池。
