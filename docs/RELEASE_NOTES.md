## 未发布：精简音频解码器、分 P 与 Linux 修复

- Windows / Linux 构建默认嵌入精简 FFmpeg，无需另装系统 FFmpeg 即可播放 AAC/M4A、ALAC、Ogg/Opus、WebM；Windows 解码器约 2.97 MiB。
- 保留 miniaudio 播放链路；原文件不改写，正常退出删除释放的解码器，缓存清理可移除已退出进程的残留。
- 设置显示内置解码器信息，增加 `--codec-info` 和 `--ffmpeg-license`；支持 `AINO_FFMPEG` 替换。
- 发布前自动验证在无系统 FFmpeg 环境下解码，并附带 FFmpeg 源码、构建信息与许可。
- B 站结果增加独立的分 P 列表，逐 P 播放、收藏、入队、下载；可保存全部分 P 快照。使用所选 cid 取流，避免所有 P 都播放第一 P。
- Linux 字体 face 跨字号共享，限制字形位图缓存；封面下载/解码最多并发两个，退出丢弃积压请求，失败封面也参与容量回收。
- X11/XWayland 读取并监听桌面 DPI，统一窗口、文字与输入缩放；支持 AINO_SCALE 覆盖和 --diagnostics。Ubuntu 24.04 / Xvfb 已通过 8 项测试，空资料库首帧 RSS 约 30.3 MiB；不代表用户机器的前后对比，真实 Wayland 仍需实机验证。
- 普通构建默认不编译测试程序；测试和发布工作流仍显式开启回归。精简 FFmpeg 配置复用，避免重复 configure 和无变化产物触发播放器重编译。

以下为已有版本说明；已有 v0.1.0 安装包未更新。

## aino player v0.1.0

- 默认进入发现页，恢复播放队列；支持本地收藏、命名歌单和其他在线歌单快照。
- Windows / Linux 使用 miniaudio 播放 MP3、FLAC、WAV；AAC/m4a 通过系统 FFmpeg 转码。
- 系统默认 Chromium 浏览器直接打开网易云或 QQ 音乐官网，使用官网的完整登录流程，自动回传登录态；无需扩展或内嵌 WebView。
- 设置支持清除登录信息、本地歌单、队列和缓存，保留其他设置和下载音乐。
- 启动时自动检查本 GitHub 仓库的最新正式版，设置 → 版本更新支持手动检查与下载。
- 修复控件在刷新、切页、失焦或失去鼠标捕获后残留按下状态，导致界面无法点击而歌词仍可交互的问题。
- 应用保留 `main.cpp`、`business.hpp`、`ui.hpp` 三文件组织。

### 安装

- **Windows x64**：下载 `.exe` 直接运行；升级时关闭旧程序并替换 exe，配置与收藏保留。
- **Ubuntu 22.04+ / Debian 12+ amd64**：下载 `.deb`，使用软件安装器或 `sudo apt install ./aino-player_0.1.0_amd64.deb`。桌面启动器名称为 aino player；Wayland 桌面需要 XWayland。
- AAC/m4a 需安装 `ffmpeg`；Linux 中文字体建议安装 `fonts-noto-cjk`。
- 自动登录回传要求默认浏览器为支持的 Chrome / Edge / Chromium，并允许本机调试连接；Firefox 暂不支持。

`SHA256SUMS.txt` 提供两个安装包的 SHA-256 校验值。
