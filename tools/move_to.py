"""把鼠标移到窗口客户区的逻辑坐标上（只移动，不点击）。

用途：验证 hover 才显示的控件（列表行的「＋」「×」按钮）。
用法：python tools/move_to.py "aino player" 1100 130
"""
import ctypes
from ctypes import wintypes
import sys
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
try:
    ctypes.WinDLL("shcore").SetProcessDpiAwareness(2)
except Exception:
    try:
        user32.SetProcessDPIAware()
    except Exception:
        pass

WM_MOUSEMOVE = 0x0200


def move(title, lx, ly, logical_w=1180.0):
    hwnd = user32.FindWindowW(None, title)
    if not hwnd:
        raise SystemExit(f"没找到窗口：{title}")
    user32.SetForegroundWindow(hwnd)
    rect = wintypes.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rect))
    scale = rect.right / logical_w
    x = int(lx * scale)
    y = int(ly * scale)
    lparam = (y << 16) | (x & 0xFFFF)
    # 连续两次：aino 的 hover 依赖上一帧的指针位置，单发一次有时不生效
    for _ in range(2):
        user32.PostMessageW(hwnd, WM_MOUSEMOVE, 0, lparam)
        time.sleep(0.12)
    print(f"moved logical ({lx},{ly}) scale={scale:.2f} client=({x},{y})")


if __name__ == "__main__":
    move(sys.argv[1], float(sys.argv[2]), float(sys.argv[3]))
