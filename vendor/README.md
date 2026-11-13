# 第三方依赖

基础单头依赖随源码保存。精简 FFmpeg 在构建时从官网获取固定源码并校验，运行时不下载依赖。

| 文件 | 版本 | 上游 | 许可 | 用途 |
| --- | --- | --- | --- | --- |
| `miniaudio/miniaudio.h` | 0.11.21 | https://github.com/mackron/miniaudio | Public Domain / MIT-0 二选一，完整条款位于头文件末尾 | Windows / Linux 播放、MP3 / FLAC / WAV 解码 |
| `stb/stb_image.h` | 2.30 | https://github.com/nothings/stb | Public Domain / MIT 二选一，完整条款位于头文件末尾 | Linux 专辑封面 JPEG / PNG 解码 |
| 嵌入的 FFmpeg 可执行文件 | 8.1.3 | https://ffmpeg.org/releases/ffmpeg-8.1.3.tar.xz | LGPL-2.1-or-later，见 `LICENSE.FFmpeg.txt` | AAC/ALAC、Ogg/Opus、WebM 解码至 PCM WAV |

miniaudio 和 stb 头文件从本机已有上游副本引入。登录直接使用外部浏览器中的官网，不再嵌入二维码脚本或自行生成扫码页面。

引入时 SHA-256：

```text
miniaudio.h C0DD363F340CC30444DDCE6E7514A1C24C456D137516CDBF49A528EDECD33840
stb_image.h 1F8C1B6B408F26E3B20CBFBBD4758AFB3DC9B837FF1E17C258928F406148A87C
```

FFmpeg 原始源码包 SHA-256：`7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3`。

`tools/build_ffmpeg.py` 保留完整的精简构建参数，不修改上游源码，不启用 GPL、nonfree 或外部库。只保留 file/pipe/http/tcp 协议、所需音频解码器/容器、PCM 输出；关闭外部库、设备及视频解码。HTTP/TCP 供播放器本机 Range 代理使用，FFmpeg 不承担远端 HTTPS 或账号请求。Windows 产物约 3.33 MiB，嵌入播放器，独立进程运行；`AINO_FFMPEG` 允许替换。完整许可、版本和参数也嵌入 exe，可通过 `--ffmpeg-license` 读取。发布工作流附带官方原始源码包和平台构建信息；Linux deb 安装许可与构建脚本。

`tests/fixtures/audio-formats.zip` 为本项目生成的一秒钟静音测试样本（CC0），仅供回归，不含音乐作品，也不打入播放器。样本生成命令和格式列表见 `tests/audio_formats_test.py` 的 `--ffmpeg` 参数。

Linux 的 libcurl、X11、fontconfig 和 FreeType 由系统提供，各自保留上游许可。
