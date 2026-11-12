"""业务测试使用独立的配置、缓存与下载目录，不接触真实资料。"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="aino-controller-") as directory:
    root = Path(directory)
    temporary = root / "temp"
    temporary.mkdir()
    environment = dict(os.environ, APPDATA=directory, XDG_CONFIG_HOME=directory,
                       TMPDIR=str(temporary), TEMP=str(temporary), TMP=str(temporary))
    subprocess.run([sys.argv[1]], env=environment, check=True, timeout=20)
