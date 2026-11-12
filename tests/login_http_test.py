"""离线测试本机登录边界；不会打开浏览器或连接音乐账号。"""
import http.client
import json
import subprocess
import sys
import socket
import time
from urllib.parse import urlsplit

process = subprocess.Popen([sys.argv[1], "--login-probe"], stdout=subprocess.PIPE, text=True)
try:
    url = process.stdout.readline().strip()
    portal = urlsplit(url)
    assert portal.hostname == "127.0.0.1" and portal.port and len(portal.path.split("/")[-1]) == 64

    def request(method="GET", path=None, body=None, headers=None):
        connection = http.client.HTTPConnection(portal.hostname, portal.port, timeout=4)
        connection.request(method, path or portal.path, body, headers or {})
        response = connection.getresponse()
        result = response.status, response.read().decode("utf-8"), dict(response.getheaders())
        connection.close()
        return result

    status, page, headers = request()
    assert status == 200 and "Cookie" in page and "<iframe" not in page
    assert "chrome-extension://" not in page and "browser-extension/" not in page
    assert headers["Cache-Control"] == "no-store" and headers["X-Frame-Options"] == "DENY"
    # 浏览器请求可能分多次到达；不能把一次 100ms 的等待误当成总超时。
    with socket.create_connection((portal.hostname, portal.port), timeout=4) as fragmented:
        fragmented.sendall(f"GET {portal.path} HTTP/1.1\r\n".encode())
        time.sleep(.15)
        fragmented.sendall(f"Host: {portal.netloc}\r\nConnection: close\r\n\r\n".encode())
        assert fragmented.recv(4096).startswith(b"HTTP/1.1 200")
    assert 'href="https://music.163.com/"' in page
    assert "/qrcode.js" not in page and "/qr" not in page
    assert request(path=portal.path + "/qrcode.js")[0] == 403
    state_status, state, _ = request(path=portal.path + "/status")
    assert state_status == 200 and json.loads(state)["done"] is False
    assert request(path="/session/" + "0" * 64)[0] == 403
    assert request(headers={"Host": "attacker.example"})[0] == 403
    base_headers = {"Content-Type": "application/json", "Origin": f"http://{portal.netloc}"}
    assert request("POST", portal.path + "/return", "{}", base_headers)[0] == 409
    endpoint = portal.path + "/cookie"
    assert request("POST", endpoint, "{broken", base_headers)[0] == 400
    assert request("POST", endpoint, json.dumps({"cookie": "foo=bar"}), base_headers)[0] == 400
    assert request("POST", endpoint, "{}", {**base_headers, "Origin": "https://attacker.example"})[0] == 403
    assert request("POST", endpoint, "{}", {"Content-Type": "text/plain"})[0] == 403
    assert request("POST", portal.path + "/cancel", "{}", base_headers)[0] == 200
    assert process.wait(timeout=5) == 0
    print("Login HTTP boundary checks passed")
finally:
    if process.poll() is None:
        process.terminate()
        process.wait(timeout=5)
