<p align="center">
  <img src="assets/aino.svg" width="80" alt="Aino 图标" />
</p>
<h1 align="center">Aino Player</h1>
<p align="center">极简界面，多源音乐。Windows · Linux</p>
<p align="center">
  <a href="https://sudoevolve.github.io/aino-player/">项目主页</a> ·
  <a href="https://github.com/sudoevolve/aino-player/releases/latest">下载最新版</a> ·
  <a href="docs/RELEASE_NOTES.md">更新内容</a> ·
  <a href="https://github.com/sudoevolve/aino-player/issues">反馈问题</a>
</p>

![Aino Player 浅色界面](docs/screenshots/player-light.png)
![Aino Player 深色歌词界面](docs/screenshots/player-dark.png)

*界面展示使用示例曲目，不代表平台推荐内容。*

## 能做什么

- **发现与搜索**：网易云、QQ、酷我、酷狗、咪咕；B 站支持搜索与分 P；五个平台均可搜索在线歌单，热门歌单支持继续加载。
- **两条取流渠道**：第三方音源脚本（`sources/*.js`，免登录）与自带接口（`providers/*.js` + 内置，可登录），在「音源」页分开管理。
- **第三方音源**：兼容部分 LX 脚本 API（内嵌 QuickJS），网易云 / QQ / 酷我 / 酷狗 / 咪咕分别显示状态，可单独检测脚本；启动静默测速一轮，智能切换优先选同平台可用且快的源，关闭后可拖动排序、固定使用首个源。
- **脚本可更新**：平台搜索与发现统一在 `providers/*.js`，默认内嵌；第三方取流脚本可导入、在线获取或重新加载。详见[支持范围](providers/README.md)。
- **播放与浏览**：内置精简 FFmpeg，支持 AAC / M4A；歌手、专辑与 B 站分 P，队列、循环、定时关闭和同步歌词、中文译文和译文开关。
- **悬浮歌词**：单双行、字号、宽度、透明度、置顶、译文和锁定穿透；支持自定义已唱／未唱／背景颜色，按行时间推进填色。可拖入 Windows 任务栏或 Ubuntu Xorg 顶栏停靠，拖出恢复悬浮。在「设置 → 通用 → 悬浮歌词」开启和配置。
- **自己的资料库**：导入音乐文件夹、本地收藏、保存在线歌单，支持筛选、排序及备份恢复；歌曲单击选择、Ctrl 增减、Shift 连选，双击或点击封面播放；支持批量收藏、加入队列、下载与移除。
- **桌面播放**：系统托盘、媒体键、系统媒体控制和音频输出设备选择，Windows / Linux 均可用。
- **登录与升级**：外部官网登录，无需扩展；应用内查看 GitHub 更新说明、下载并安装新版本。

## 下载与安装

在 [GitHub Releases](https://github.com/sudoevolve/aino-player/releases/latest) 下载：

| 系统                             | 推荐下载                                         |
| -------------------------------- | ------------------------------------------------ |
| Windows x64                      | `setup-windows-x64.exe` 安装器；也提供便携 EXE |
| Ubuntu 22.04+ / Debian 12+ amd64 | `.deb` 安装包                                  |
| Fedora 44+ x86_64（DNF）         | `.rpm` 安装包或 [RPM 软件源](docs/RPM.md) |
| Arch x86_64                      | `.pkg.tar.zst` 安装包或 [Arch 软件源](docs/ARCH.md) |

Linux 一键安装（amd64 / x86_64）：

```bash
# Ubuntu 22.04+ / Debian 12+
curl -fsSL https://sudoevolve.github.io/aino-player/install.sh | bash

# Fedora 44+（DNF）
curl -fsSL https://sudoevolve.github.io/aino-player/install-rpm.sh | bash

# Arch Linux / 衍生发行版
curl -fsSL https://sudoevolve.github.io/aino-player/install-arch.sh | bash
```

自动添加签名软件源（[APT](docs/APT.md) / [RPM](docs/RPM.md) / [Arch](docs/ARCH.md)）并安装；之后交给系统包管理器更新。Arch 安装会执行完整的 `pacman -Syu`，避免局部升级。升级保留配置、收藏和歌单。

## 使用提示

- 音源导入后保存在用户配置目录，重启自动后台加载；“移除”保留 JS，重新导入可恢复。
- 本地音乐：在「歌单 → 本地」点击「打开音乐」，或按 `Ctrl+O` 多选文件；也可拖入文件 / 文件夹。自动播放第一首并加入队列，重复文件不会重复添加，队列可保存为本地歌单。
- 默认播放器：在「设置 → 通用 → 本地播放」设置文件关联；Windows 还需在弹出的系统设置中确认。支持系统双击 / 打开方式，已有播放器运行时会优先交给现有窗口。
- 官网登录自动回传支持 Chrome / Edge / Chromium；平台会员和地区限制照常生效。
- Linux 使用 X11 / XWayland，建议安装 `fonts-noto-cjk`；应用内安装更新需要 `pkexec`，托盘需要桌面提供支持。
- 设置 → 数据可查看存储位置、清理缓存和登录信息、导出或恢复资料库。

## 开发与许可

[构建指南](docs/BUILDING.md) · [架构与验证记录](docs/ADR-001-portable-player.md) · [Linux 内存实测](docs/Linux-memory-profile.md) · [第三方依赖](vendor/README.md)

Apache-2.0 · 作者 [sudoevolve](https://github.com/sudoevolve) · [aino UI 库](https://github.com/sudoevolve/aino)
