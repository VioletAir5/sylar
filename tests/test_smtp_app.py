#!/usr/bin/env python3
"""SMTP dialogue tests against a local fake server, plain and TLS."""

import socket
import ssl
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

CRLF = bytes((13, 10))


def run_case(executable, message_file, cert=None, key=None, reject=False,
             starttls=False, advertise=True):
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen()
    listener.settimeout(5)
    port = listener.getsockname()[1]
    result = {}

    def serve():
        try:
            connection, _ = listener.accept()
            connection.settimeout(5)
            if cert and not starttls:
                context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                context.load_cert_chain(str(cert), str(key))
                connection = context.wrap_socket(
                    connection, server_side=True
                )
            with connection:
                stream = connection.makefile("rb")
                connection.sendall(b"220 fake" + CRLF)

                def read_line():
                    line = stream.readline(4096)
                    if not line.endswith(CRLF):
                        raise RuntimeError(f"bad SMTP line: {line!r}")
                    return line

                if read_line() != b"EHLO localhost" + CRLF:
                    raise RuntimeError("missing EHLO")
                if starttls and advertise:
                    connection.sendall(
                        b"250-fake" + CRLF +
                        b"250-STARTTLS" + CRLF +
                        b"250 SIZE 100000" + CRLF
                    )
                    if read_line() != b"STARTTLS" + CRLF:
                        raise RuntimeError("missing STARTTLS")
                    connection.sendall(b"220 ready for TLS" + CRLF)
                    stream.close()
                    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                    context.load_cert_chain(str(cert), str(key))
                    connection = context.wrap_socket(
                        connection, server_side=True
                    )
                    stream = connection.makefile("rb")
                    if read_line() != b"EHLO localhost" + CRLF:
                        raise RuntimeError("missing post-TLS EHLO")
                    connection.sendall(
                        b"250-fake" + CRLF +
                        b"250 SIZE 100000" + CRLF
                    )
                else:
                    connection.sendall(
                        b"250-fake" + CRLF +
                        b"250 SIZE 100000" + CRLF
                    )
                    if starttls:
                        return
                if read_line() != (
                    b"MAIL FROM:<sender@example.test>" + CRLF
                ):
                    raise RuntimeError("wrong MAIL FROM")
                connection.sendall(b"250 sender ok" + CRLF)
                if read_line() != (
                    b"RCPT TO:<to@example.test>" + CRLF
                ):
                    raise RuntimeError("wrong RCPT TO")
                if reject:
                    connection.sendall(b"550 rejected" + CRLF)
                    return
                connection.sendall(b"250 recipient ok" + CRLF)
                if read_line() != b"DATA" + CRLF:
                    raise RuntimeError("missing DATA")
                connection.sendall(b"354 continue" + CRLF)
                body = []
                while (line := read_line()) != b"." + CRLF:
                    body.append(line)
                expected = (
                    b"Subject: test" + CRLF + CRLF
                    + b"first" + CRLF
                    + b"..leading" + CRLF
                )
                if b"".join(body) != expected:
                    raise RuntimeError(
                        f"bad SMTP data: {body!r}"
                    )
                connection.sendall(b"250 queued" + CRLF)
                if read_line() != b"QUIT" + CRLF:
                    raise RuntimeError("missing QUIT")
                connection.sendall(b"221 bye" + CRLF)
        except Exception as error:
            result["error"] = error
        finally:
            listener.close()

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    command = [
        executable, "127.0.0.1", str(port),
        "sender@example.test", "to@example.test",
        str(message_file),
    ]
    if cert:
        command += ["--starttls" if starttls else "--tls", str(cert)]
    completed = subprocess.run(
        command, capture_output=True, text=True,
        check=False, timeout=5,
    )
    thread.join(timeout=5)
    if thread.is_alive():
        raise RuntimeError("fake SMTP server did not stop")
    if "error" in result:
        raise result["error"]
    if reject or (starttls and not advertise):
        if completed.returncode == 0:
            raise RuntimeError("rejected recipient was accepted")
    elif completed.returncode != 0:
        raise RuntimeError(
            f"SMTP client failed: {completed!r}"
        )


def main(executable):
    with tempfile.TemporaryDirectory() as directory:
        base = Path(directory)
        message = base / "message.txt"
        message.write_bytes(
            b"Subject: test" + CRLF + CRLF
            + b"first" + CRLF
            + b".leading" + CRLF
        )
        run_case(executable, message)
        run_case(executable, message, reject=True)
        cert = base / "cert.pem"
        key = base / "key.pem"
        subprocess.run(
            [
                "openssl", "req", "-x509", "-newkey",
                "rsa:2048", "-nodes", "-days", "1",
                "-subj", "/CN=localhost",
                "-addext",
                "subjectAltName=IP:127.0.0.1",
                "-keyout", str(key), "-out", str(cert),
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=5, check=True,
        )
        run_case(executable, message, cert, key)
        run_case(executable, message, cert, key, starttls=True)
        run_case(executable, message, cert, key,
                 starttls=True, advertise=False)


if __name__ == "__main__":
    main(sys.argv[1])