"""真实 Chromium 的离线回传回归；仅注入假 Cookie，不连接音乐账号。"""
import base64
import hashlib
import json
import os
from pathlib import Path
import queue
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import urlsplit


class DevTools:
    def __init__(self, endpoint):
        url = urlsplit(endpoint)
        assert url.hostname == "127.0.0.1"
        self.socket = socket.create_connection((url.hostname, url.port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        self.socket.sendall((f"GET {url.path} HTTP/1.1\r\nHost: {url.netloc}\r\n"
                             f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
                             f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        header = b""
        while not header.endswith(b"\r\n\r\n"):
            header += self.read(1)
            assert len(header) < 16384
        expected = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        assert header.startswith(b"HTTP/1.1 101 ") and expected in header
        self.sequence = 0

    def read(self, length):
        result = b""
        while len(result) < length:
            chunk = self.socket.recv(length - len(result))
            if not chunk:
                raise EOFError("dedicated browser connection closed")
            result += chunk
        return result

    def send(self, data, opcode=1):
        length = len(data)
        header = bytes((0x80 | opcode, 0x80 | (length if length < 126 else 126 if length < 65536 else 127)))
        if length >= 126:
            header += struct.pack("!H" if length < 65536 else "!Q", length)
        mask = os.urandom(4)
        self.socket.sendall(header + mask + bytes(value ^ mask[index % 4] for index, value in enumerate(data)))

    def call(self, method, params=None, session=None):
        self.sequence += 1
        message = {"id": self.sequence, "method": method, "params": params or {}}
        if session:
            message["sessionId"] = session
        self.send(json.dumps(message).encode())
        while True:
            first, second = self.read(2)
            length = second & 127
            if length >= 126:
                length = struct.unpack("!H" if length == 126 else "!Q", self.read(2 if length == 126 else 8))[0]
            assert not second & 128 and length < 2 * 1024 * 1024
            payload = self.read(length)
            if first & 15 == 9:
                self.send(payload, 10)
                continue
            assert first == 0x81, "expected complete CDP text frame"
            response = json.loads(payload)
            if response.get("id") == self.sequence:
                assert "error" not in response, response
                return response["result"]

    def close(self):
        self.socket.close()


class Probe:
    def __init__(self, binary, browser, site, directory):
        self.directory = Path(directory)
        self.temporary = self.directory / "temp"
        self.temporary.mkdir()
        env = dict(os.environ, APPDATA=directory, XDG_CONFIG_HOME=directory,
                   TEMP=str(self.temporary), TMP=str(self.temporary), TMPDIR=str(self.temporary), AINO_LOGIN_TRACE="1")
        self.log_path = self.directory / "browser.log"
        self.log = self.log_path.open("w", encoding="utf-8")
        self.process = subprocess.Popen([binary, "--browser-probe", site, browser],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log,
                                        text=True, encoding="utf-8", env=env)
        self.lines = queue.Queue()
        def reader():
            for line in self.process.stdout:
                self.lines.put(line.strip())
            self.lines.put(None)
        self.reader = threading.Thread(target=reader, daemon=True)
        self.reader.start()
        self.cdp = None

    def line(self):
        try:
            result = self.lines.get(timeout=25)
        except queue.Empty:
            raise AssertionError("browser probe timed out:\n" + self.log_path.read_text(encoding="utf-8", errors="replace")[-8000:]) from None
        assert result is not None, f"probe exited unexpectedly: {self.process.poll()}"
        return result

    def command(self, command):
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()
        return json.loads(self.line())

    def connect(self):
        endpoint = self.line()
        assert endpoint.startswith("ws://127.0.0.1:"), endpoint
        self.cdp = DevTools(endpoint)
        profiles = list(self.temporary.glob("**/browser-auth-*"))
        assert len(profiles) == 1, profiles
        self.profile = profiles[0]
        assert self.profile.parent.name == str(self.process.pid), "owned per-process profile"
        info = self.cdp.call("Target.getTargets")
        target = next(item["targetId"] for item in info["targetInfos"] if item["type"] == "page")
        self.session = self.cdp.call("Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]
        return target

    def cookie(self, name, value, domain, path="/"):
        result = self.cdp.call("Network.setCookie", {"name": name, "value": value, "domain": domain,
                                                    "path": path, "secure": True, "httpOnly": True}, self.session)
        assert result["success"], result

    def finish(self, command="finish"):
        result = self.command(command)
        assert self.process.wait(timeout=8) == 0
        assert result["done"]
        assert not list(self.temporary.glob("**/browser-auth-*")), "ephemeral profile must be removed"
        return result

    def close(self):
        if self.process.poll() is None:
            try:
                self.finish("cancel")
            finally:
                if self.process.poll() is None:
                    self.process.kill()
                    self.process.wait(timeout=8)
        if self.cdp:
            self.cdp.close()
        self.process.stdin.close()
        self.process.stdout.close()
        self.log.close()


def run(binary, browser):
    for site in ("netease", "qq"):
        with tempfile.TemporaryDirectory(prefix="aino-browser-test-") as directory:
            probe = Probe(binary, browser, site, directory)
            try:
                target = probe.connect()
                for name, value in (("MUSIC_U", "wrong-domain"), ("uin", "o001234"), ("qm_keyst", "wrong-domain")):
                    probe.cookie(name, value, "unrelated.example")
                probe.cookie("MUSIC_U", "wrong-path", "music.163.com", "/not-the-official-root")
                time.sleep(1.4)
                state = probe.command("status")
                assert not state["done"] and not state["cookie"] and not state["return_requested"], state
                # 官网登录弹窗或跳转可能关闭原标签，回传需要重新绑定尚存页面。
                new_target = probe.cdp.call("Target.createTarget", {"url": "about:blank"})["targetId"]
                probe.cdp.call("Target.closeTarget", {"targetId": target})
                probe.session = probe.cdp.call("Target.attachToTarget", {"targetId": new_target, "flatten": True})["sessionId"]
                time.sleep(1.4)
                assert not probe.command("status")["done"], "tab changes must not end login"
                if site == "netease":
                    probe.cookie("MUSIC_U", "fixture-auth-netease", ".music.163.com")
                else:
                    probe.cookie("uin", "o001234", ".qq.com")
                    time.sleep(1.3)
                    assert not probe.command("status")["done"], "QQ uin alone is not a music session"
                    probe.cookie("qm_keyst", "fixture-auth-qq", ".qq.com")
                result = probe.finish()
                assert result["return_requested"], result
                assert "fixture-auth-" + site in result["cookie"] and "wrong-" not in result["cookie"], result
                if site == "qq":
                    assert result["uid"] == "1234", result
            finally:
                probe.close()
        print(f"{site}: real browser cookie handoff, domain/path scope, tab replacement and profile cleanup passed")

    for command in ("cancel", "forget"):
        with tempfile.TemporaryDirectory(prefix="aino-browser-test-") as directory:
            probe = Probe(binary, browser, "netease", directory)
            try:
                probe.connect()
                result = probe.finish(command)
                assert not result["cookie"] and not result["uid"] and not result["return_requested"], result
            finally:
                probe.close()
        print(f"{command}: no credentials published; dedicated browser and profile removed")


if __name__ == "__main__":
    run(sys.argv[1], sys.argv[2])
