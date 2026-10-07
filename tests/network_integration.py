import socket
import struct
import sys
import time
from server_support import RunningServer


def receive_all(conn):
    parts = []
    while True:
        part = conn.recv(65536)
        if not part:
            return b"".join(parts)
        parts.append(part)


def main():
    server = RunningServer(sys.argv[1])
    try:
        payload = b"half-close\0" * 20000
        with socket.create_connection(("127.0.0.1", server.port), timeout=3) as conn:
            conn.sendall(payload)
            conn.shutdown(socket.SHUT_WR)
            assert receive_all(conn) == payload
        with socket.create_connection(("127.0.0.1", server.port), timeout=2) as conn:
            conn.sendall(b"release-twice")
            assert conn.recv(1) == b""
        conn = socket.create_connection(("127.0.0.1", server.port), timeout=2)
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        conn.sendall(b"reset")
        conn.close()
        # RST 不能杀死服务，后续连接仍然能正常回显。
        with socket.create_connection(("127.0.0.1", server.port), timeout=2) as conn:
            conn.sendall(b"still-alive")
            assert conn.recv(128) == b"still-alive"
        idle = socket.create_connection(("127.0.0.1", server.port), timeout=2)
        assert "STOPPED closed=" in server.stop()
        assert idle.recv(1) == b""
        idle.close()
    finally:
        if server.process.poll() is None:
            server.stop()
    # 同一服务重复启动/停机，每次都要正常回收工作线程。
    for _ in range(3):
        RunningServer(sys.argv[1]).stop()
    server = RunningServer(sys.argv[1])
    with socket.create_connection(("127.0.0.1", server.port), timeout=4) as conn:
        conn.sendall(b"large")
        first = conn.recv(1024)
        server.process.terminate()
        assert first + receive_all(conn) == b"z" * (8 * 1024 * 1024)
        server.stop(send_signal=False) # 已发出 SIGTERM，避免退出恢复信号掩码时重复发信号。
    server = RunningServer(sys.argv[1])
    conn = socket.socket()
    conn.settimeout(2)
    conn.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
    conn.connect(("127.0.0.1", server.port))
    conn.sendall(b"large")
    time.sleep(0.1)
    # 不读取响应，停机必须在期限后强制释放，不能无限等待 EPOLLOUT。
    server.stop()
    conn.close()


if __name__ == "__main__":
    main()
