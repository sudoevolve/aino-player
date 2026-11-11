"""抓取指定标题的窗口，存成 BMP（无第三方依赖，只用 ctypes + GDI）。

用法：python tools/grab_window.py "aino player" out.bmp
"""
import ctypes
from ctypes import wintypes
import sys

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)

try:
    ctypes.WinDLL("shcore").SetProcessDpiAwareness(2)  # PER_MONITOR_DPI_AWARE
except Exception:
    try:
        user32.SetProcessDPIAware()
    except Exception:
        pass

PW_RENDERFULLCONTENT = 0x00000002


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD),
        ("biWidth", wintypes.LONG),
        ("biHeight", wintypes.LONG),
        ("biPlanes", wintypes.WORD),
        ("biBitCount", wintypes.WORD),
        ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD),
        ("biXPelsPerMeter", wintypes.LONG),
        ("biYPelsPerMeter", wintypes.LONG),
        ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wintypes.DWORD * 3)]


def grab(title, path):
    hwnd = user32.FindWindowW(None, title)
    if not hwnd:
        raise SystemExit(f"没找到窗口：{title}")
    rect = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    w, h = rect.right - rect.left, rect.bottom - rect.top
    hdc = user32.GetWindowDC(hwnd)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mem, bmp)
    ok = user32.PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT)
    if not ok:
        gdi32.BitBlt(mem, 0, 0, w, h, hdc, 0, 0, 0x00CC0020)

    bi = BITMAPINFO()
    bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = w
    bi.bmiHeader.biHeight = -h  # 负 = 自上而下
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    bi.bmiHeader.biCompression = 0
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0)

    header = bytearray()
    header += b"BM"
    header += (14 + 40 + len(buf)).to_bytes(4, "little")
    header += (0).to_bytes(4, "little")
    header += (54).to_bytes(4, "little")
    header += (40).to_bytes(4, "little")
    header += w.to_bytes(4, "little", signed=True)
    header += (-h).to_bytes(4, "little", signed=True)
    header += (1).to_bytes(2, "little")
    header += (32).to_bytes(2, "little")
    header += (0).to_bytes(4, "little")
    header += (len(buf)).to_bytes(4, "little")
    header += (2835).to_bytes(4, "little") * 2
    header += (0).to_bytes(4, "little") * 2
    with open(path, "wb") as f:
        f.write(header)
        f.write(buf.raw)

    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(hwnd, hdc)
    print(f"saved {path} {w}x{h}")


if __name__ == "__main__":
    grab(sys.argv[1] if len(sys.argv) > 1 else "aino player",
         sys.argv[2] if len(sys.argv) > 2 else "shot.bmp")
