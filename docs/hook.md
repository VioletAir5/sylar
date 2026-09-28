# Hook

HookScope 在当前线程开启 POSIX 调用拦截。IOManager 管理的 Fiber 调用 read、write、readv、writev、recv、send、recvfrom、sendto、recvmsg、sendmsg、connect、accept/accept4、close 时，非阻塞 socket 遇到 EAGAIN 会挂起当前 Fiber，并由 epoll 唤醒。sleep、usleep 和 nanosleep 会使用定时器挂起 Fiber。

```cpp
io.schedule([&] {
    sylar::HookScope hooks(std::chrono::seconds(5));
    char buffer[1024];
    const auto count = ::read(nonblocking_socket, buffer, sizeof(buffer));
});
io.run();
```

Hook 是显式启用的，默认保持 POSIX 行为。启用范围是线程；交错的 Fiber 可以分别持有 HookScope，最后一个作用域结束后恢复默认行为。用于 I/O 的 socket 必须已经设为 O_NONBLOCK，框架创建的 Socket 满足这个条件。普通文件和阻塞 socket 保持原有调用行为。调用超时返回 -1，并设置 errno 为 ETIMEDOUT；取消时设置 ECANCELED。

当前覆盖最常用的读写、连接、接受连接和睡眠调用。暂不拦截 fcntl、socket 和 poll/select。生产代码优先使用 [SocketStream](../include/net/SocketStream.hpp) 明确表达 I/O 边界。
