# ZooKeeper 适配

ZkClient 使用 ZooKeeper 多线程 C 客户端，等待会话连接后提供 znode 的创建、存在检查、读取、更新、删除和子节点列表。支持短暂节点与版本条件更新，节点数据上限为 1 MiB。

示例：

    ./build/sylar/zk_demo 127.0.0.1:2181

集成测试启动独立的临时 ZooKeeper Java 服务，验证二进制数据及 znode 生命周期，再终止服务并清理临时目录。当前 WSL 的 Java cgroup 检测有异常，测试只为子进程设置 -XX:-UseContainerSupport。

ZooKeeper 同步 C API 会阻塞调用线程，应用应在 BlockingExecutor 中使用 ZkClient。当前尚未提供 watch 回调、ACL/认证封装、断线自动重试或服务发现上层 API。构建需要 libzookeeper-mt-dev；运行集成测试还需要 zookeeperd 与 Java。

构建该适配器时设置 `-DSYLAR_WITH_ZOOKEEPER=ON`；默认核心构建不包含此适配器。
