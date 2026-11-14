"""Build the per-user Windows installer; the pinned compiler is a build-only tool."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
import urllib.request
from check_file_associations import check as check_file_associations

ROOT = Path(__file__).resolve().parents[1]
COMPILER_VERSION = "6.7.4"
COMPILER_SHA256 = "a549929319f9120716677c685535ca4671e4b937b5fdc87e2585f32d8eb30e8c"


def compiler():
    candidates = [shutil.which("ISCC"), ROOT / "build/tools/inno/ISCC.exe",
                  Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Inno Setup 6/ISCC.exe"]
    found = next((Path(p) for p in candidates if p and Path(p).is_file()), None)
    if found:
        return found
    directory = ROOT / "build/tools/inno"
    package = ROOT / "build/tools" / f"innosetup-{COMPILER_VERSION}.exe"
    package.parent.mkdir(parents=True, exist_ok=True)
    if not package.exists() or hashlib.sha256(package.read_bytes()).hexdigest() != COMPILER_SHA256:
        url = f"https://github.com/jrsoftware/issrc/releases/download/is-{COMPILER_VERSION.replace('.', '_')}/innosetup-{COMPILER_VERSION}.exe"
        partial = package.with_suffix(".download")
        print(f"Downloading official Inno Setup {COMPILER_VERSION} compiler...", flush=True)
        deadline = time.monotonic() + 120
        with urllib.request.urlopen(url, timeout=20) as response, partial.open("wb") as output:
            total = 0
            while chunk := response.read(16 * 1024):
                total += len(chunk)
                if total > 32 * 1024 * 1024 or time.monotonic() > deadline:
                    raise RuntimeError("Inno Setup download exceeded size or time limit; retry or provide ISCC in PATH")
                output.write(chunk)
        if hashlib.sha256(partial.read_bytes()).hexdigest() != COMPILER_SHA256:
            raise RuntimeError("Inno Setup compiler SHA-256 mismatch")
        partial.replace(package)
    print(f"Preparing verified Inno Setup {COMPILER_VERSION} compiler in {directory}", flush=True)
    subprocess.run([str(package), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CURRENTUSER", "/NOICONS",
                    "/TASKS=", f"/DIR={directory}"], check=True, timeout=120)
    result = directory / "ISCC.exe"
    if not result.is_file():
        raise RuntimeError("Inno Setup compiler was not installed")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "build/installer")
    args = parser.parse_args()
    check_file_associations()
    if os.name != "nt":
        parser.error("Windows installer compilation requires Windows")
    version = re.search(r"project\(aino_player VERSION ([0-9]+\.[0-9]+\.[0-9]+)", (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")).group(1)
    if not args.exe.is_file():
        parser.error("Build the player before packaging")
    args.output.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(compiler()), f"/DAppVersion={version}", f"/DPlayerExe={args.exe.resolve()}",
                    f"/DOutputPath={args.output.resolve()}", str(ROOT / "packaging/windows.iss")], check=True)
    package = args.output / f"aino-player-v{version}-setup-windows-x64.exe"
    if not package.is_file():
        raise RuntimeError("Installer output missing")
    print(f"Installer: {package} ({package.stat().st_size:,} bytes)")
