"""Echo/HTTP end-to-end checks with isolated static-file fixtures."""

import http.client
import os
import pathlib
import signal
import socket
import subprocess
import sys
import tempfile
import time


def try_for_five_seconds(check, process):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("server exited before handling a request")
        try:
            check()
            return
        except (ConnectionError, OSError):
            time.sleep(0.05)
    raise TimeoutError("server did not accept a request within five seconds")


def check_echo():
    with socket.create_connection(("127.0.0.1", 8500), timeout=2) as conn:
        conn.settimeout(2)
        conn.sendall(b"refactor-echo")
        data = conn.recv(1024)
        assert data == b"refactor-echo", repr(data)


def check_http():
    def request(method, path, body=None):
        conn = http.client.HTTPConnection("127.0.0.1", 8085, timeout=2)
        try:
            conn.request(method, path, body=body)
            response = conn.getresponse()
            return response.status, response.read(), dict(response.getheaders())
        finally:
            conn.close()

    status, body, headers = request("GET", "/index.html")
    assert status == 200 and b"<html>" in body, (status, body)
    assert headers["Content-Type"] == "text/html", headers
    get_length = len(body)
    status, body, headers = request("HEAD", "/index.html")
    assert status == 200 and body == b"" and int(headers["Content-Length"]) == get_length
    status, body, headers = request("GET", "/empty.txt")
    assert status == 200 and body == b"" and headers["Content-Length"] == "0"
    status, body, _ = request("GET", "/hello?name=alice")
    assert status == 200 and b"name: alice" in body, (status, body)
    status, body, _ = request("POST", "/login", "x=1")
    assert status == 200 and body.endswith(b"x=1"), (status, body)
    status, body, _ = request("GET", "/not-found")
    assert status == 404, (status, body)
    for path in ("/./../outside.txt", "/%2e/%2e%2e/outside.txt",
                 "/outside-link.txt", "/sibling-link.txt"):
        status, body, _ = request("GET", path)
        assert status == 404 and b"private-marker" not in body, (path, status, body)
    status, body, _ = request("GET", "/assets/../index.html")
    assert status == 200 and b"<html>" in body
    status, body, _ = request("PUT", "/1234.txt", "must-not-write")
    assert status == 404, (status, body)
    if os.geteuid() != 0:
        status, body, _ = request("GET", "/denied.txt")
        assert status == 500 and b"private-marker" not in body, (status, body)
    else:
        print("read-denial fixture skipped: root bypasses ordinary file permissions")

    # 同一 socket 上复用请求，客户端读完一份正文后才能发下一份。
    conn = http.client.HTTPConnection("127.0.0.1", 8085, timeout=2)
    try:
        conn.request("GET", "/hello")
        response = conn.getresponse()
        assert response.status == 200
        response.read()
        first_socket = conn.sock
        conn.request("POST", "/login", body="x=1")
        response = conn.getresponse()
        assert response.status == 200 and response.read().endswith(b"x=1")
        assert conn.sock is first_socket and first_socket is not None
    finally:
        conn.close()

    # 一批输入包含 POST 和两个 GET：第二份要求关闭，第三份必须被丢弃。
    def read_response(stream):
        status = int(stream.readline().split()[1])
        headers = {}
        while True:
            line = stream.readline()
            if line == b"\r\n":
                break
            assert line, "unexpected EOF in response headers"
            key, value = line.split(b":", 1)
            headers[key.lower()] = value.strip()
        return status, headers, stream.read(int(headers[b"content-length"]))

    with socket.create_connection(("127.0.0.1", 8085), timeout=2) as sock:
        sock.sendall(b"POST /login HTTP/1.1\r\nHost: localhost\r\nContent-Length: 3\r\n\r\nx=1"
                     b"GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
                     b"GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n")
        with sock.makefile("rb") as stream:
            status, headers, body = read_response(stream)
            assert status == 200 and body.endswith(b"x=1")
            status, headers, body = read_response(stream)
            assert status == 200 and headers[b"connection"] == b"close"
            assert stream.read(1) == b"", "server must close without responding to the third request"


def prepare_http_fixtures(project, cwd):
    root = cwd / "wwwroot"
    root.mkdir()
    (root / "assets").mkdir()
    (root / "index.html").write_bytes((project / "source/http/wwwroot/index.html").read_bytes())
    (root / "empty.txt").write_bytes(b"")
    (cwd / "outside.txt").write_bytes(b"private-marker")
    sibling = cwd / "wwwroot-extra"
    sibling.mkdir()
    (sibling / "outside.txt").write_bytes(b"private-marker")
    (root / "outside-link.txt").symlink_to(cwd / "outside.txt")
    (root / "sibling-link.txt").symlink_to(sibling / "outside.txt")
    denied = root / "denied.txt"
    denied.write_bytes(b"private-marker")
    denied.chmod(0)


def main():
    kind, executable = sys.argv[1:]
    project = pathlib.Path(__file__).resolve().parents[1]
    check = check_http if kind == "http" else check_echo
    # 测试资源全部在临时目录，不修改项目原有 wwwroot。
    with tempfile.TemporaryDirectory(prefix="reactor-http-smoke-") as fixture_dir:
        cwd = pathlib.Path(fixture_dir) if kind == "http" else project
        if kind == "http":
            prepare_http_fixtures(project, cwd)
        with tempfile.TemporaryFile() as server_log, subprocess.Popen(
            [executable], cwd=cwd, stdout=server_log, stderr=server_log
        ) as process:
            try:
                try_for_five_seconds(check, process)
            finally:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                expected = 0 if kind == "http" else -signal.SIGTERM
                if process.returncode != expected:
                    server_log.seek(0)
                    raise AssertionError(f"server exit={process.returncode}: "
                                         f"{server_log.read().decode(errors='replace')}")


if __name__ == "__main__":
    main()
