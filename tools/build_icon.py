"""Regenerate committed app icons from the geometric SVG (Pillow, build-time only)."""
from io import BytesIO
from pathlib import Path
import xml.etree.ElementTree as ET
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]


def render(size):
    scale = size * 4 / 256
    image = Image.new("RGBA", (size * 4, size * 4))
    draw = ImageDraw.Draw(image)
    for element in ET.parse(ROOT / "assets/aino.svg").getroot():
        kind = element.tag.rsplit("}", 1)[-1]
        if kind == "rect":
            x, y, w, h, r = (float(element.get(k)) * scale for k in ("x", "y", "width", "height", "rx"))
            draw.rounded_rectangle((x, y, x + w, y + h), radius=r, fill=element.get("fill"))
        elif kind == "polygon":
            points = [tuple(float(v) * scale for v in point.split(",")) for point in element.get("points").split()]
            draw.polygon(points, fill=element.get("fill"))
    return image.resize((size, size), Image.Resampling.LANCZOS)


if __name__ == "__main__":
    render(512).save(ROOT / "assets/aino.png", optimize=True)
    render(256).save(ROOT / "assets/aino.ico", sizes=[(n, n) for n in (16, 20, 24, 32, 40, 48, 64, 128, 256)])
    buffer = BytesIO()
    render(64).save(buffer, format="PNG", optimize=True)
    data = buffer.getvalue()
    rows = [", ".join(str(b) for b in data[i:i+24]) for i in range(0, len(data), 24)]
    (ROOT / "assets/aino_icon.hpp").write_text(
        "// Generated from aino.svg by tools/build_icon.py.\n#pragma once\n"
        "namespace player::brand {\ninline constexpr unsigned char png[] = {\n" +
        ",\n".join(rows) + "\n};\n}\n", encoding="ascii")
    print("Generated PNG, multi-resolution ICO and embedded 64px PNG from assets/aino.svg")
