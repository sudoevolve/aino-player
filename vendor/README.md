# 第三方依赖

依赖随源码保存，构建时不需要联网拉取。

| 文件 | 版本 | 上游 | 许可 | 用途 |
| --- | --- | --- | --- | --- |
| `miniaudio/miniaudio.h` | 0.11.21 | https://github.com/mackron/miniaudio | Public Domain / MIT-0 二选一，完整条款位于头文件末尾 | Windows / Linux 播放、MP3 / FLAC / WAV 解码 |
| `stb/stb_image.h` | 2.30 | https://github.com/nothings/stb | Public Domain / MIT 二选一，完整条款位于头文件末尾 | Linux 专辑封面 JPEG / PNG 解码 |

miniaudio 和 stb 头文件从本机已有上游副本引入。登录直接使用外部浏览器中的官网，不再嵌入二维码脚本或自行生成扫码页面。

引入时 SHA-256：

```text
miniaudio.h C0DD363F340CC30444DDCE6E7514A1C24C456D137516CDBF49A528EDECD33840
stb_image.h 1F8C1B6B408F26E3B20CBFBBD4758AFB3DC9B837FF1E17C258928F406148A87C
```

AAC/m4a 转码使用用户系统中的 FFmpeg 可执行程序，本仓库不捆绑 FFmpeg。Linux 的 libcurl、X11、fontconfig 和 FreeType 由系统提供，各自保留上游许可。
