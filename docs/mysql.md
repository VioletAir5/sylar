# MySQL 适配

MySqlClient 使用 MySQL 8 C API 管理连接，支持 Unix socket 和 TCP，提供普通查询以及预编译的 executePrepared/queryPrepared。预编译参数按长度传输，支持二进制数据；查询结果使用 optional<string> 区分 SQL NULL。普通 execute/query 应仅用于固定 SQL，业务数据使用预编译参数。

示例在独立线程中连接临时 MySQL：

    ./build/sylar/mysql_demo /path/to/mysql.sock

集成测试初始化临时数据目录，启动仅用 Unix socket 的 MySQL 8 实例，验证预编译写入、读取与异常路径，然后关闭并清理实例。它不会使用本机现有数据库目录。

MySQL C API 调用是同步的；在 IOManager 中应通过 BlockingExecutor 执行。当前没有连接池、事务 RAII、自动重连或 TLS 连接配置。构建需要 mysqlclient 开发包；运行集成测试还需要 mysqld 与 mysqladmin。预编译语句绑定方式参考 [MySQL 8 C API 文档](https://dev.mysql.com/doc/c-api/8.0/en/mysql-stmt-bind-param.html)。