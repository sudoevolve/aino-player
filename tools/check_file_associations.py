"""Static packaging check: scanner, desktop entry and Windows associations agree."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def check():
    source = (ROOT / "src/platform.hpp").read_text(encoding="utf-8")

    def values(name):
        array = re.search(rf"\b{name}\[\]\s*\{{([^}}]+)\}}", source)
        if not array:
            raise ValueError(f"Missing {name} in platform.hpp")
        return set(re.findall(r'"([^"\n]+)"', array[1]))

    installer = (ROOT / "packaging/windows.iss").read_text(encoding="utf-8")
    extensions = set(re.findall(r'ValueName: "(\.[a-z0-9]+)"; ValueData: "AinoPlayer.Audio"', installer))
    open_with = set(re.findall(r'Software\\Classes\\(\.[a-z0-9]+)\\OpenWithProgids', installer))
    desktop = (ROOT / "packaging/aino-player.desktop").read_text(encoding="utf-8")
    mimes = set(re.search(r"^MimeType=(.+)$", desktop, re.M)[1].strip().strip(";").split(";"))
    if extensions != values("audio_extensions") or open_with != extensions:
        raise ValueError("Windows installer associations differ from platform::audio_extensions")
    if mimes != values("audio_mimes"):
        raise ValueError("Desktop MIME list differs from platform::audio_mimes")


if __name__ == "__main__":
    check()
    print("File association metadata is consistent")
