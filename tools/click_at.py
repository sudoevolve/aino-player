"""在窗口客户区里按逻辑坐标点一下（自动换算 DPI 缩放）。

用法：python tools/click_at.py "aino player" 600 128
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
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
MK_LBUTTON = 0x0001


def click(title, lx, ly, logical_w=1180.0):
    hwnd = user32.FindWindowW(None, title)
    if not hwnd:
        raise SystemExit(f"没找到窗口：{title}")
    user32.SetForegroundWindow(hwnd)
    rect = wintypes.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rect))
    scale = rect.right / logical_w
    # lParam 用的是**客户区**坐标（不是屏幕坐标）。之前传屏幕坐标，
    # 结果点在"进度条"上其实落在了列表里 —— 排查了半天。
    x = int(lx * scale)
    y = int(ly * scale)
    lparam = (y << 16) | (x & 0xFFFF)
    user32.PostMessageW(hwnd, WM_MOUSEMOVE, 0, lparam)
    time.sleep(0.05)
    user32.PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lparam)
    time.sleep(0.06)
    user32.PostMessageW(hwnd, WM_LBUTTONUP, 0, lparam)
    print(f"clicked logical ({lx},{ly}) scale={scale:.2f} screen=({x},{y})")


if __name__ == "__main__":
    click(sys.argv[1], float(sys.argv[2]), float(sys.argv[3]))
