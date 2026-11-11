"""把鼠标移到窗口客户区的某个逻辑坐标（只移动，不点击），用于观察 hover 效果。

用法：python tools/hover_at.py "aino player" 996 703
"""
import ctypes
from ctypes import wintypes
import sys
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)

WM_MOUSEMOVE = 0x0200


def hover(title, lx, ly, logical_w=1180.0):
    hwnd = user32.FindWindowW(None, title)
    if not hwnd:
        raise SystemExit(f"没找到窗口：{title}")
    rect = wintypes.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rect))
    scale = rect.right / logical_w
    x = int(lx * scale)
    y = int(ly * scale)
    lparam = (y << 16) | (x & 0xFFFF)
    user32.PostMessageW(hwnd, WM_MOUSEMOVE, 0, lparam)
    print(f"hover logical ({lx},{ly}) -> client ({x},{y})")


if __name__ == "__main__":
    hover(sys.argv[1], float(sys.argv[2]), float(sys.argv[3]))
    time.sleep(0.2)
