"""网络测试进程仅监听 loopback，日志保存在临时文件中。"""
import os
import re
import socket
import subprocess
import tempfile
import time


class RunningServer:
    def __init__(self, executable, args=(), env=None):
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen([executable, *args], stdout=self.log, stderr=self.log, env=env)
        deadline = time.monotonic() + 20 # 给调度较慢的测试环境保留启动宽限。
        while time.monotonic() < deadline:
            text = os.pread(self.log.fileno(), 65536, 0).decode(errors="replace")
            if self.process.poll() is not None:
                self.log.close()
                raise RuntimeError(f"server startup failed: {text}")
            match = re.search(r"LISTENING [^\s:]+:(\d+)", text)
            if match:
                self.port = int(match[1])
                try:
                    with socket.create_connection(("127.0.0.1", self.port), timeout=0.2):
                        return
                except OSError:
                    pass
            time.sleep(0.02)
        self.process.kill()
        self.process.wait()
        self.log.close()
        raise TimeoutError("server startup timed out")

    def stop(self, send_signal=True, timeout=6):
        if send_signal and self.process.poll() is None:
            self.process.terminate()
        try:
            self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise AssertionError("graceful shutdown timed out")
        text = os.pread(self.log.fileno(), 1000000, 0).decode(errors="replace")
        self.log.close()
        assert self.process.returncode == 0 and "STOPPED" in text, f"exit={self.process.returncode}\n{text}"
        return text
