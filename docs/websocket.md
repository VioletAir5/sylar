# WebSocket

`WebSocketSession` 实现 RFC 6455 服务端升级握手、客户端掩码校验、文本与二进制消息、分片重组、ping/pong 和关闭握手。默认单条消息上限为 1 MiB，可在构造时设置。文本消息会校验 UTF-8。无效帧返回协议错误关闭码 1002。

`WebSocketServer` 基于 `TcpServer` 和 I/O 工作线程池，可使用 `TlsContext` 提供 wss。`WebSocketClient` 接受 ws/wss URI，验证升级应答，发送带随机掩码的帧，并支持文本、二进制、ping/pong 与关闭握手。示例：

```bash
./build/sylar/ws_echo_server 8081 4
./build/sylar/ws_echo_server 8444 4 --tls cert.pem key.pem
./build/sylar/ws_client ws://127.0.0.1:8081/ws hello
./build/sylar/ws_client wss://localhost:8444/ws hello --ca cert.pem
```

示例路由是 `/ws`。WebSocket 可以通过 `HttpServer::addWebSocket(path, handler)` 与普通 HTTP 路由共用监听端口；`http_server` 示例在 `/ws` 提供 ws/wss 回显。也可单独运行 `ws_echo_server`。尚未实现扩展压缩或子协议协商。协议测试同时运行 ws 与 wss，覆盖握手后紧接着发送的帧、两种扩展长度、分片、ping/pong、关闭和未掩码错误帧；客户端测试还覆盖错误握手应答与证书验证。设计依据：[RFC 6455](https://www.rfc-editor.org/rfc/rfc6455.html)。
