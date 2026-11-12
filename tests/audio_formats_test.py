"""验证音频解码及可选真实设备播放；内置模式不允许使用系统 FFmpeg。"""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile

FORMATS = [("wav", "wav", "pcm_s16le"), ("mp3", "mp3", "libmp3lame"), ("flac", "flac", "flac"),
           ("m4a", "m4a", "aac"), ("aac", "aac", "aac"), ("alac", "m4a", "alac"),
           ("ogg", "ogg", "libvorbis"), ("opus", "opus", "libopus"), ("webm", "webm", "libopus"), ("tagged-aac", "aac", "aac")]


def run(args):
    binary = str(Path(args.binary).resolve())
    generator = str(Path(args.ffmpeg).resolve()) if args.ffmpeg else None
    with tempfile.TemporaryDirectory(prefix="aino-audio-") as directory:
        root = Path(directory)
        cache = root / "cache"
        cache.mkdir()
        temporary = root / "temp"
        temporary.mkdir()
        environment = dict(os.environ, APPDATA=directory, XDG_CONFIG_HOME=directory,
                           TEMP=str(temporary), TMP=str(temporary), TMPDIR=str(temporary))
        if args.bundled:
            environment["PATH"] = ""
            environment.pop("AINO_FFMPEG", None)
            info = subprocess.check_output([binary, "--codec-info"], text=True, encoding="utf-8", env=environment)
            assert "embedded" in info, f"Test requires embedded FFmpeg: {info}"
            print(info.strip(), flush=True)
        if not generator:
            with zipfile.ZipFile(Path(__file__).parent / "fixtures/audio-formats.zip") as archive:
                archive.extractall(root)
        for key, ext, codec in FORMATS:
            file = root / ("中文音频-" + key + "." + ext)
            if generator:
                subprocess.run([generator, "-nostdin", "-v", "error", "-f", "lavfi", "-i", "anullsrc=r=44100:cl=stereo",
                                "-t", "1.0", "-c:a", codec, str(file)], check=True)
                if key == "tagged-aac":
                    file.write_bytes(b"ID3\x04\0\0\0\0\0\0" + file.read_bytes())
            else:
                (root / (key + "." + ext)).rename(file)
            original = hashlib.sha256(file.read_bytes()).hexdigest()
            mode = "--audio-decode" if args.decode_only else "--audio-device"
            subprocess.run([binary, mode, str(file), str(cache / (key + ".pcm.wav"))],
                           check=True, timeout=30, env=environment, cwd=root)
            subprocess.run([binary, "--audio-stream", str(file)], check=True, timeout=30, env=environment, cwd=root)
            assert hashlib.sha256(file.read_bytes()).hexdigest() == original, "original music must not change"
            assert not file.with_name(file.name + ".pcm.wav").exists(), "conversion must not write beside original music"
            assert not list(temporary.glob("**/ffmpeg-*")), "embedded executable must be removed on exit"
            print(f"{key}: " + ("decode" if args.decode_only else "playback/pause/seek/end") + " passed", flush=True)
        original = hashlib.sha256(file.read_bytes()).hexdigest()
        subprocess.run([binary, "--audio-reject", str(file), str(file)], check=True, timeout=30, env=environment, cwd=root)
        assert hashlib.sha256(file.read_bytes()).hexdigest() == original, "aliased conversion output must not overwrite original music"
        if args.bundled:
            override = dict(environment, AINO_FFMPEG=str(root / "missing-decoder"))
            subprocess.run([binary, "--audio-reject", str(file), str(cache / "override.pcm.wav")],
                           check=True, timeout=30, env=override, cwd=root)
        invalid = root / "corrupt.aac"
        invalid.write_bytes(b"\xff\xf1\x50\x80\x00\x1f\xfc" * 4)
        subprocess.run([binary, "--audio-reject", str(invalid), str(cache / "corrupt.pcm.wav")],
                       check=True, timeout=30, env=environment, cwd=root)
        assert not list(cache.glob("*.part.wav")), "failed output must be removed"
        assert not list(temporary.glob("**/ffmpeg-*")), "error paths must remove embedded executable"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary")
    parser.add_argument("--bundled", action="store_true")
    parser.add_argument("--decode-only", action="store_true", help="CI without an audio device")
    parser.add_argument("--ffmpeg", help="Optional full FFmpeg to generate fixtures instead of using the checked-in silence samples")
    run(parser.parse_args())
