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
