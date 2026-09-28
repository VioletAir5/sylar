# WebSocket

`WebSocketSession` 实现 RFC 6455 服务端升级握手、客户端掩码校验、文本与二进制消息、分片重组、ping/pong 和关闭握手。默认单条消息上限为 1 MiB，可在构造时设置。文本消息会校验 UTF-8。无效帧返回协议错误关闭码 1002。

`WebSocketServer` 基于 `TcpServer` 和 I/O 工作线程池，可使用 `TlsContext` 提供 wss。示例：

```bash
./build/sylar/ws_echo_server 8081 4
./build/sylar/ws_echo_server 8444 4 --tls cert.pem key.pem
```

示例路由是 `/ws`。HTTP 和 WebSocket 目前使用独立的服务端实例与监听端口；尚未实现同端口升级、客户端 API、扩展压缩或子协议协商。协议测试同时运行 ws 与 wss，覆盖握手后紧接着发送的帧、两种扩展长度、分片、ping/pong、关闭和未掩码错误帧。设计依据：[RFC 6455](https://www.rfc-editor.org/rfc/rfc6455.html)。
