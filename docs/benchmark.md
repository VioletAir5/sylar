# 日志性能测试

场景参考 [spdlog 的 bench.cpp](https://github.com/gabime/spdlog/blob/v1.x/bench/bench.cpp)（MIT 许可），测试程序为本项目重新实现。它测试当前同步 Logger，不链接 spdlog，也不把结果当作两库之间的直接对比。

## 构建与运行

    cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release \
      -DSYLAR_BUILD_BENCHMARKS=ON -DBUILD_TESTING=OFF
    cmake --build build-bench -j
    ./build-bench/bench/sylar_log_bench 250000 4

位置参数依次是每个场景的消息总数、线程数、可选日志目录。默认分别为 250000、4 和 /tmp 下的唯一目录。普通构建默认不包含 benchmark 目标。

## 场景和口径

| 场景 | 内容 |
| --- | --- |
| file/single | 当前线程写入一个 FileSink |
| file/1-thread | 新建一个线程写入同一个 FileSink |
| file/N-thread | N 个线程并发写入同一个 FileSink |
| level-off/single | 当前线程调用关闭级别的 Logger |
| level-off/N-thread | N 个线程调用关闭级别的 Logger |

每个写文件场景生成一个独立文件。程序从第一条日志开始计时；线程场景把创建和 join 线程的时间算入日志耗时。写入结束后显式 flush，并单独报告 flush 时间。消息总数会在各线程间分配完整，不丢弃余数。

写文件结果包含格式化、互斥锁和写入 C++ 缓冲流的时间，不包含 fsync 或磁盘持久化时间。关闭级别的结果只反映过滤快路径。不同机器、文件系统和编译选项的数字不可直接比较；本项目没有 spdlog 的单线程无锁 Sink、旋转文件或每日文件 Sink，所以未移植对应场景。

## 本机基线

2026-09-27，WSL Ubuntu，GCC 13.1，Release，250000 条/场景，4 线程：

| 场景 | 日志时间 | 吞吐（条/秒） |
| --- | ---: | ---: |
| file/single | 0.181828 s | 1,374,927 |
| file/1-thread | 0.192173 s | 1,300,913 |
| file/N-thread | 0.756068 s | 330,658 |
| level-off/single | 0.000287 s | 871,341,238 |
| level-off/N-thread | 0.000327 s | 764,881,535 |

写文件的三个输出各核对为 250000 行。这些数值是一次测量的基线，不代表稳定性能承诺。
