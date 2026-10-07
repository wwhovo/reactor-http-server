"""Small integration checks for the unchanged Echo and HTTP examples."""

import http.client
import pathlib
import socket
import subprocess
import sys
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
            return response.status, response.read()
        finally:
            conn.close()

    status, body = request("GET", "/index.html")
    assert status == 200 and b"<html>" in body, (status, body)
    status, body = request("GET", "/hello?name=alice")
    assert status == 200 and b"name: alice" in body, (status, body)
    status, body = request("POST", "/login", "x=1")
    assert status == 200 and body.endswith(b"x=1"), (status, body)
    status, body = request("GET", "/not-found")
    assert status == 404, (status, body)


def main():
    kind, executable = sys.argv[1:]
    project = pathlib.Path(__file__).resolve().parents[1]
    cwd = project / "source" / "http" if kind == "http" else project
    check = check_http if kind == "http" else check_echo
    with subprocess.Popen(
        [executable], cwd=cwd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
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


if __name__ == "__main__":
    main()
