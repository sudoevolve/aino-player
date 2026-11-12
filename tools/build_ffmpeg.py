"""Build the small, local file/pipe FFmpeg helper, or embed it for CMake."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

VERSION = "8.1.3"
ARCHIVE_SHA256 = "7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3"
ROOT = Path(__file__).resolve().parents[1]


def embed(binary, header):
    build_info = subprocess.check_output([str(binary), "-version"], text=True, encoding="utf-8", errors="replace")
    if not build_info.startswith(f"ffmpeg version {VERSION} ") or any(flag in build_info for flag in ("--enable-gpl", "--enable-nonfree")):
        raise RuntimeError(f"Expected LGPL FFmpeg {VERSION}, without GPL or nonfree components")
    options = build_info.replace("'", "").replace('"', "")
    if not all(flag in options for flag in ("--disable-everything", "--disable-network", "--disable-autodetect", "--enable-small", "--enable-protocol=file,pipe", "--enable-muxer=wav,pcm_s16le")):
        raise RuntimeError("Expected the minimal local file/pipe FFmpeg build")
    data = binary.read_bytes()
    if not data or len(data) > 12 * 1024 * 1024:
        raise RuntimeError("Expected a minimal FFmpeg helper smaller than 12 MiB")
    digest = hashlib.sha256(data).hexdigest()
    lines = ["#pragma once", "#include <cstddef>", "namespace audio::embedded {",
             f'inline constexpr const char* version = "{VERSION}";',
             f'inline constexpr const char* digest = "{digest}";',
             f'inline constexpr std::size_t size = {len(data)};']
    notice = (f"aino player embeds an unmodified-source FFmpeg {VERSION} executable.\n"
              "FFmpeg is licensed separately under LGPL 2.1 or later.\n"
              f"Source: https://ffmpeg.org/releases/ffmpeg-{VERSION}.tar.xz\n"
              f"Source SHA-256: {ARCHIVE_SHA256}\n"
              "Exact build recipe: tools/build_ffmpeg.py in the corresponding aino-player source tag.\n"
              "A replacement executable can be selected with AINO_FFMPEG.\n\n" + build_info + "\n" +
              (ROOT / "vendor/LICENSE.FFmpeg.txt").read_text(encoding="utf-8"))
    lines += ['inline constexpr const char* notice = R"AinoFFmpeg(' + notice + ')AinoFFmpeg";', "}"]
    header.parent.mkdir(parents=True, exist_ok=True)
    header.write_text("\n".join(lines) + "\n", encoding="ascii")
    print(f"Embedded FFmpeg {VERSION}: {len(data):,} bytes, SHA-256 {digest}", flush=True)


def build(output, jobs):
    work = ROOT / "build" / "codec-build"
    work.mkdir(parents=True, exist_ok=True)
    archive = work / f"ffmpeg-{VERSION}.tar.xz"
    if not archive.exists() or hashlib.sha256(archive.read_bytes()).hexdigest() != ARCHIVE_SHA256:
        partial = archive.with_suffix(".download")
        with urllib.request.urlopen(f"https://ffmpeg.org/releases/ffmpeg-{VERSION}.tar.xz", timeout=60) as response, partial.open("wb") as target:
            shutil.copyfileobj(response, target)
        if hashlib.sha256(partial.read_bytes()).hexdigest() != ARCHIVE_SHA256:
            raise RuntimeError("FFmpeg source checksum mismatch")
        partial.replace(archive)
    source = work / f"ffmpeg-{VERSION}"
    if not source.exists():
        with tarfile.open(archive) as package:
            package.extractall(work, filter="data")
    directory = work / "native"
    directory.mkdir(exist_ok=True)
    windows = os.name == "nt"
    bash = shutil.which("bash")
    if windows:
        candidates = [Path("C:/msys64/usr/bin/bash.exe"), Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Git/bin/bash.exe"]
        bash = next((str(path) for path in candidates if path.exists()), bash)
    make = shutil.which("make") or shutil.which("mingw32-make")
    if windows:
        # CI 可能同时安装多套 make；优先与 Bash 同一 MSYS2 的工具。
        sibling_make = Path(bash).with_name("make.exe") if bash else None
        if sibling_make and sibling_make.exists():
            make = str(sibling_make)
        else:
            make = shutil.which("mingw32-make") or make
    if not bash or not make or not shutil.which("gcc"):
        raise RuntimeError("Build needs bash, GNU make and GCC (Windows: MSYS2 UCRT64 or MinGW-w64 + Git Bash)")
    environment = dict(os.environ)
    if windows:
        # mingw32-make may launch mkdir/sed directly instead of going through Bash.
        environment["PATH"] = os.pathsep.join((str(Path(bash).parent), str(Path(bash).parent.parent / "usr/bin"), environment.get("PATH", "")))
    flags = ["--disable-everything", "--disable-autodetect", "--disable-network", "--disable-doc", "--disable-debug",
             "--disable-programs", "--enable-ffmpeg", "--disable-avdevice", "--disable-swscale", "--disable-shared",
             "--enable-static", "--enable-small", "--disable-x86asm", "--cc=gcc", "--host-cc=gcc",
             "--enable-decoder=aac,alac,opus,vorbis,flac,mp3,mp3float,pcm_s16le", "--enable-demuxer=aac,mov,ogg,matroska,mp3,wav,flac",
             "--enable-muxer=wav,pcm_s16le", "--enable-encoder=pcm_s16le", "--enable-parser=aac,aac_latm,opus,vorbis",
             "--enable-protocol=file,pipe", "--enable-filter=aresample,aformat,anull",
             "--extra-cflags=-Os -ffunction-sections -fdata-sections",
             "--extra-ldflags=" + ("-static " if windows else "") + "-Wl,--gc-sections"]
    if windows:
        flags += ["--target-os=mingw32", "--arch=x86_64", "--disable-pthreads", "--enable-w32threads"]
    # configure preserves the given source path; relative paths also work in native mingw32-make.
    configure = "../ffmpeg-" + VERSION + "/configure"
    compiler = shutil.which("gcc")
    fingerprint = json.dumps({"source": ARCHIVE_SHA256, "flags": flags, "compiler": compiler,
        "compiler_version": subprocess.check_output([compiler, "--version"], env=environment).decode("utf-8", "replace"),
        "bash": str(bash), "make": make}, sort_keys=True)
    stamp = directory / "aino-config.json"
    reconfigure = not (stamp.exists() and stamp.read_text(encoding="utf-8") == fingerprint and
                       (directory / "ffbuild/config.mak").exists() and (directory / "config.h").exists())
    if reconfigure:
        print(f"Configuring FFmpeg {VERSION} in {directory}", flush=True)
        subprocess.run([bash, configure, *flags], cwd=directory, check=True, env=environment)
    else:
        print("Reusing FFmpeg configuration; checking incremental build", flush=True)
    if reconfigure and windows:
        # native GNU make 也可能命名为 make.exe，不能只按文件名识别。
        source_windows = source.resolve().as_posix()
        source_msys = "/" + source_windows[0].lower() + source_windows[2:]
        for name in ("makefile", "ffbuild/config.mak"):
            path = directory / name
            text = path.read_text(encoding="utf-8").replace(source_msys, "../ffmpeg-" + VERSION)
            path.write_text(text, encoding="utf-8", newline="\n")
    if reconfigure:
        stamp.write_text(fingerprint, encoding="utf-8")
    helper = "ffmpeg.exe" if windows else "ffmpeg"
    make_args = [make, f"-j{jobs}", helper]
    if windows:
        # Native GNU make invokes SHELL without quoting a path containing spaces.
        import ctypes
        shell_path = str(Path(bash).with_name("sh.exe"))
        buffer = ctypes.create_unicode_buffer(32768)
        if ctypes.windll.kernel32.GetShortPathNameW(shell_path, buffer, len(buffer)):
            shell_path = buffer.value
        make_args += ["SHELL=" + shell_path.replace("\\", "/")]
    subprocess.run(make_args, cwd=directory, check=True, env=environment)
    built = directory / helper
    build_info = subprocess.check_output([str(built), "-version"], text=True, encoding="utf-8", errors="replace")
    print(build_info, flush=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    # 保留未变化产物的时间戳，避免触发 CMake 重新编译音频和链接播放器。
    if not output.exists() or built.read_bytes() != output.read_bytes():
        shutil.copy2(built, output)
    shutil.copy2(source / "COPYING.LGPLv2.1", output.parent / "LICENSE.FFmpeg.txt")
    (output.parent / "FFmpeg-BUILD.txt").write_text(
        f"FFmpeg source: https://ffmpeg.org/releases/ffmpeg-{VERSION}.tar.xz\nSHA-256: {ARCHIVE_SHA256}\n\n" + build_info,
        encoding="utf-8")
    print(f"Minimal FFmpeg ready: {output} ({output.stat().st_size:,} bytes)", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/codec" / ("ffmpeg.exe" if os.name == "nt" else "ffmpeg"))
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 4))
    parser.add_argument("--embed", type=Path)
    parser.add_argument("--header", type=Path)
    args = parser.parse_args()
    if args.embed:
        if not args.header:
            parser.error("--embed requires --header")
        embed(args.embed.resolve(), args.header.resolve())
    else:
        build(args.output.resolve(), max(1, args.jobs))
