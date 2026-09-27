# 日志性能测试

场景参考 [spdlog 的 bench.cpp](https://github.com/gabime/spdlog/blob/v1.x/bench/bench.cpp)（MIT 许可）。本项目提供两个独立程序：`sylar_log_bench` 测当前同步 Logger，`spdlog_log_bench` 测真正的 spdlog。后者使用 spdlog 自带的 fmt；spdlog 源码放在本项目目录外。

## 构建与运行

先准备 [yaml-cpp 依赖](configuration.md#依赖与构建)，再在本项目根目录执行：

```sh
git clone https://github.com/gabime/spdlog.git ../spdlog-upstream
git -C ../spdlog-upstream checkout 5b6378033725c89d30a694473e90a806d3bb5780
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release \
  -DSYLAR_BUILD_BENCHMARKS=ON -DBUILD_TESTING=OFF \
  -DSYLAR_SPDLOG_SOURCE_DIR="$PWD/../spdlog-upstream"
cmake --build build-bench -j
./build-bench/bench/sylar_log_bench 250000 4
./build-bench/bench/spdlog_log_bench 250000 4
```

`SYLAR_SPDLOG_SOURCE_DIR` 是可选的；不设置时仅构建本项目的测试。位置参数为每场景的消息总数、线程数和可选的日志目录；默认分别是 250000、4 和 `/tmp` 下的唯一目录。重复运行时使用新目录，因为两份程序的文件 Sink 都以追加模式打开文件。普通构建默认不包含 benchmark 目标。

## 场景和口径

| 场景 | 内容 |
| --- | --- |
| file/single | 当前线程写入带锁文件 Sink |
| file/1-thread | 新建一个线程写入同一个 Sink |
| file/N-thread | N 个线程并发写入同一个 Sink |
| level-off/single | 当前线程调用关闭级别的 Logger |
| level-off/N-thread | N 个线程调用关闭级别的 Logger |

两份程序使用相同的 `message {index}` 消息、日期/名称/级别/消息格式、消息总数和线程分配。每个写文件场景生成一个独立文件。计时包含日志调用；线程场景还包含创建与 join。结束后显式 flush，其耗时另列。写文件结果包括格式化、锁和缓冲写入，不包括 `fsync`。两库的文件 Sink 实现不同，因此结果衡量各自完整的同步写文件路径。level-off 场景短至微秒级，易受编译器优化和调度影响，不宜据此判断过滤快路径优劣。

## 本机对比

2026-09-27，WSL Ubuntu，GCC 13.1，Release，spdlog 1.17.0（提交 `5b6378033725`），每场景 250000 条，4 线程。交错运行两套程序各三轮，下表是吞吐中位数：

| 场景 | 当前实现（条/秒） | spdlog（条/秒） | spdlog / 当前实现 |
| --- | ---: | ---: | ---: |
| file/single | 1,139,148 | 5,866,279 | 5.15× |
| file/1-thread | 1,193,415 | 5,358,393 | 4.49× |
| file/N-thread | 323,282 | 2,209,964 | 6.84× |
| level-off/single | 902,761,729 | 142,854,776 | 0.16× |
| level-off/N-thread | 621,963,264 | 231,546,014 | 0.37× |

每轮的三个写文件输出均核对为各 250000 行。这些数字只代表本机测试；文件系统、编译选项和负载会影响结果。
