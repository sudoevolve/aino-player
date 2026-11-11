"""BMP(32bpp, 自上而下/自下而上) → PNG，只用标准库（zlib + struct）。

机器上没有 PIL，但截图核对和交付都得要 PNG，于是自己写一个最小编码器。
用法：python tools/bmp2png.py in.bmp out.png
"""
import struct
import sys
import zlib


def load_bmp(path):
    d = open(path, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    if bpp != 32:
        raise SystemExit(f"只支持 32bpp，当前 {bpp}")
    top_down = h < 0
    h = abs(h)
    row = w * 4
    rows = []
    for y in range(h):
        src = y if top_down else (h - 1 - y)
        start = off + src * row
        px = d[start:start + row]
        rgba = bytearray(w * 4)
        for x in range(w):
            b, g, r, a = px[x * 4:x * 4 + 4]
            rgba[x * 4:x * 4 + 4] = bytes((r, g, b, 255 if a == 0 else a))
        rows.append(bytes(rgba))
    return w, h, rows


def write_png(path, w, h, rows):
    raw = bytearray()
    for r in rows:
        raw.append(0)  # filter type 0
        raw += r

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


if __name__ == "__main__":
    src = sys.argv[1] if len(sys.argv) > 1 else "shot.bmp"
    dst = sys.argv[2] if len(sys.argv) > 2 else "shot.png"
    w, h, rows = load_bmp(src)
    write_png(dst, w, h, rows)
    print(f"{dst} {w}x{h}")
