"""以静音样本验证真实音频设备的播放、暂停、跳转和结束；需安装 FFmpeg。"""
import pathlib
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="aino-audio-") as directory:
    for ext, codec in [("wav", "pcm_s16le"), ("mp3", "libmp3lame"), ("flac", "flac"), ("m4a", "aac")]:
        file = pathlib.Path(directory) / ("中文音频." + ext)
        subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i", "anullsrc=r=44100:cl=stereo",
                        "-t", "1.0", "-c:a", codec, str(file)], check=True)
        subprocess.run([sys.argv[1], "--audio-device", str(file)], check=True, timeout=20)
        print(f"{ext}: playback/pause/seek/end passed", flush=True)
