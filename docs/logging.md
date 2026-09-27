# 同步日志模块

当前实现采用 Logger → Sink → Formatter 分工。写入在调用日志的线程完成。

## 使用示例

    #include <iostream>
    #include <memory>
    #include "log/FileSink.hpp"
    #include "log/Logger.hpp"
    #include "log/OstreamSink.hpp"

    auto console = std::make_shared<sylar::log::OstreamSink>(std::cout);
    auto file = std::make_shared<sylar::log::FileSink>("logs/server.log");
    file->setLevel(sylar::log::LogLevel::WARN);
    file->setPattern("%Y-%m-%d %H:%M:%S [%l] %v");

    sylar::log::Logger logger("server", {console, file});
    logger.setLevel(sylar::log::LogLevel::INFO);
    logger.flushOn(sylar::log::LogLevel::ERROR);

    logger.info("started");
    SYLAR_LOG_INFO(logger, "accepted connection {}", 42);
    logger.flush();

格式化宏在级别关闭时不会求值参数。直接调用 info() 等方法用于已生成的文本。FATAL 只是日志级别，不会终止进程。

## 格式模板

默认格式是 %Y-%m-%d %H:%M:%S [%n] [%l] %v。

| 标记 | 内容 |
| --- | --- |
| %Y %m %d | 年、月、日 |
| %H %M %S | 时、分、秒 |
| %n | Logger 名称 |
| %l | 日志级别 |
| %v | 消息 |
| %t | 线程 ID |
| %s | 源文件名 |
| %# | 源代码行号 |
| %% | 百分号 |

模板在设置时解析；无效标记抛出 std::invalid_argument。每个 Sink 可设置自己的级别和模板；Logger::setPattern 会设置它当前持有的所有 Sink。

## 行为与限制

- Sink 列表在 Logger 构造时确定。级别、Formatter 可并发更新；单个 Sink 的格式化和写入由同一把锁保护。
- FileSink 以追加模式写入，自动创建父目录。外部轮转文件后调用 reopen()；普通日志不逐条 flush，可手动 flush() 或设置 flushOn()。
- 写入或 flush 失败会抛出异常。Logger 会尝试其他 Sink，再向调用方抛出第一个错误。
- 当前没有异步队列、自动文件轮转或全局 Logger 注册表。

## 验证

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
    cmake --build build -j
    ctest --test-dir build --output-on-failure
