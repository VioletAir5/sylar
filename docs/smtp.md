# SMTP 客户端

SmtpClient::send 使用 IOManager 的非阻塞 Socket/Stream 完成服务器问候、EHLO、MAIL FROM、一个或多个 RCPT TO、DATA 与 QUIT。它解析多行回复，校验应答码，并对 DATA 的行首句点做转义。SmtpMessage::data 应包含完整邮件头与正文。

示例：

    ./build/sylar/smtp_demo 127.0.0.1 2525 sender@example.test to@example.test message.txt
    ./build/sylar/smtp_demo mail.example.test 465 sender@example.test to@example.test message.txt --tls ca.pem
    ./build/sylar/smtp_demo mail.example.test 587 sender@example.test to@example.test message.txt --starttls ca.pem

--tls 使用隐式 TLS；--starttls 先检查 EHLO 扩展、请求升级，再在 TLS 通道重新 EHLO。两种 TLS 模式都验证证书与主机名；服务端未公告 STARTTLS 时会拒绝发送。明文端口仅适用于可信本地服务或测试。当前没有 SMTP AUTH、MIME 生成或重试队列，因此还不是面向公网邮件服务的完整投递器。

集成测试使用本地假服务器，覆盖多行 EHLO、拒绝收件人、DATA 转义、隐式 TLS、STARTTLS 成功和未公告扩展时拒绝，没有发送真实邮件。协议依据：[RFC 5321](https://www.rfc-editor.org/rfc/rfc5321.html)。