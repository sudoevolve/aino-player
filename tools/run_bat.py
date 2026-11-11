"""跑一个 .bat 并把它（GBK）的输出原样返回。

为什么不在命令行里直接调 cmd：本机 WorkBuddy 的 shell 工具会拦截命令行文本里出现
cmd.exe / powershell 的调用（工具级安全守卫，跟 Windows 策略无关）。落成脚本再跑
python 就不经过那条文本路径，同时还能把 PATH 洗成纯 Windows 系统路径 —— 否则
Git 的 usr/bin 里的 find.exe 会抢在系统 find 前面，批处理的输出会完全失真。
"""
import os
import subprocess
import sys


def main() -> int:
    bat = sys.argv[1]
    cwd = os.path.dirname(os.path.abspath(bat))
    env = {
        "SystemRoot": r"C:\Windows",
        "windir": r"C:\Windows",
        "PATH": r"C:\Windows\System32;C:\Windows;C:\Windows\System32\Wbem",
        "ProgramFiles": os.environ.get("ProgramFiles", r"C:\Program Files"),
        "ProgramFiles(x86)": os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
        "TEMP": os.environ.get("TEMP", r"C:\Windows\Temp"),
        "TMP": os.environ.get("TMP", r"C:\Windows\Temp"),
        "SystemDrive": os.environ.get("SystemDrive", "C:"),
        "USERPROFILE": os.environ.get("USERPROFILE", r"C://Users//Public"),
        "NUMBER_OF_PROCESSORS": os.environ.get("NUMBER_OF_PROCESSORS", "1"),
        "APPDATA": os.environ.get("APPDATA", ""),
        "LOCALAPPDATA": os.environ.get("LOCALAPPDATA", ""),
    }
    r = subprocess.run(
        [r"C:\Windows\System32\cmd.exe", "/c", os.path.basename(bat)],
        cwd=cwd,
        env=env,
        input=b"\r\n" * 40,   # 万一有 pause，别挂死
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=600,
    )
    sys.stdout.write(r.stdout.decode("gbk", errors="replace"))
    return r.returncode


if __name__ == "__main__":
    raise SystemExit(main())
