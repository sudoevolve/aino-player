## aino player v0.2.0

### 更新内容

- **内置精简 FFmpeg**：Windows / Linux 无需安装系统 FFmpeg 即可解码 AAC/M4A、ALAC、Ogg/Opus、WebM；保留 miniaudio 播放链路。解码器随程序分发，运行时不下载组件，也不改写原音乐文件。
- **本地音乐与独立歌单**：「歌单 → 本地」支持导入音乐文件夹并重新扫描。在线搜索、歌单详情和正在播放队列分别展示；浏览歌单不会把歌单名称写进搜索框。
- **B 站分 P**：搜索结果可打开独立分 P 列表，逐 P 播放、收藏、入队和下载，也可保存全部分 P 为本地快照。取流使用所选 cid，修复始终播放第一 P 的问题。
- **Linux 内存**：FreeType face 跨字号共享，持久字形位图缓存限制为 4 MiB / 16384 项。封面最多两个并发任务，失败条目也参与回收。字体和 DPI 修复已同步上游 aino `d14cad3`。
- **Linux 缩放**：X11/XWayland 自动读取并监听桌面 DPI，统一窗口、文字和鼠标坐标；支持 `AINO_SCALE=1.5` 手动覆盖，`high_dpi=false` 保持 1 倍。`--diagnostics` 可输出倍率、字体缓存和 RSS。
- **编译提速**：默认只构建播放器；精简 FFmpeg 复用配置与未变化产物，减少重复 configure 和重新嵌入。仅明确发布新版本时执行回归，普通提交不自动跑 CI。
- **YouTube Music（实验性）**：加入官网外部浏览器登录、账号与链接歌单、创建和同步队列，以及搜索、播放和下载接入。本次仍未做真实 YouTube 网络验证，不保证受限曲目可用。

保留默认发现页、本地收藏、官网登录自动回传、缓存清理和 GitHub 正式版更新检查。升级会保留配置、收藏与本地歌单。

### 安装与升级

- **Windows x64**：下载 `aino-player-v0.2.0-windows-x64.exe` 直接运行；升级前关闭旧程序，然后替换 exe。
- **Ubuntu 22.04+ / Debian 12+ amd64**：下载 `aino-player_0.2.0_amd64.deb`，使用软件安装器或 `sudo apt install ./aino-player_0.2.0_amd64.deb`。建议安装 `fonts-noto-cjk` 以显示中文。
- 官网自动登录回传需要受支持的 Chrome / Edge / Chromium，Firefox 暂不支持。无需浏览器扩展或内嵌 WebView。

### 验证范围与附件

发布流程在 Windows 和 Linux 编译并运行离线业务、输入生命周期、隔离浏览器登录回传和内置音频解码回归；Linux 另检查动态 DPI、字体共享、deb 安装与 Xvfb 启动。自动化登录只使用假 Cookie，不使用真实账号。

Linux 仍为 X11/XWayland；原生 Wayland、混合 DPI 多显示器、真实 Linux 音频设备和手机扫码登录仍需实机验证。内存占用受窗口大小、音频与封面影响，不承诺固定 RSS 或降到 Windows 的 17 MB。

`SHA256SUMS.txt` 提供校验值；附件同时包含精简 FFmpeg 对应的官方源码包、两平台构建信息和许可证。
