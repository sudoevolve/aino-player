"""业务测试使用独立的配置、缓存与下载目录，不接触真实资料。"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import http.server
import struct
import threading
import time
import zlib


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


pixels = b"".join(b"\0" + bytes((x + y) % 256 for x in range(256 * 3)) for y in range(256))
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 256, 256, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b"")


class Covers(http.server.BaseHTTPRequestHandler):
    lock = threading.Lock()
    active = 0
    peak = 0

    def do_GET(self):
        with Covers.lock:
            Covers.active += 1
            Covers.peak = max(Covers.peak, Covers.active)
        try:
            time.sleep(.03)
            self.send_response(200)
            self.send_header("Content-Length", str(len(png)))
            self.end_headers()
            self.wfile.write(png)
        finally:
            with Covers.lock:
                Covers.active -= 1

    def log_message(self, *_):
        pass

with tempfile.TemporaryDirectory(prefix="aino-controller-") as directory:
    root = Path(directory)
    temporary = root / "temp"
    temporary.mkdir()
    environment = dict(os.environ, APPDATA=directory, XDG_CONFIG_HOME=directory,
                       TMPDIR=str(temporary), TEMP=str(temporary), TMP=str(temporary))
    subprocess.run([sys.argv[1]], env=environment, check=True, timeout=20)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Covers)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        subprocess.run([sys.argv[1], "--cover-stress", f"http://127.0.0.1:{server.server_port}"], env=environment, check=True, timeout=20)
        assert Covers.peak == 2, f"expected two cover workers, observed {Covers.peak}"
        print(f"Cover stress: peak concurrent downloads={Covers.peak}")
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
