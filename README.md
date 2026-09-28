# sylar

- [同步日志](docs/logging.md)
- [日志基准测试](docs/benchmark.md)
- [配置系统](docs/configuration.md)

- [开发计划与当前范围](docs/roadmap.md)
- [TCP echo 初始基准](docs/echo_benchmark.md)

## TCP echo 示例

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/sylar/echo_server 9000
```

当前 I/O 调度为单线程，socket 使用非阻塞模式；读写等待和超时由 IOManager 显式管理。
