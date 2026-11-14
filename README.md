<p align="center">
  <img src="assets/aino.svg" width="80" alt="Aino 图标" />
</p>
<h1 align="center">Aino Player</h1>
<p align="center">极简界面，多源音乐。Windows · Linux</p>
<p align="center">
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
- **第三方音源**：兼容部分 LX 脚本 API（内嵌 QuickJS），接入网易云 / QQ / 酷我 / 酷狗 / 咪咕取流；后台静默检测 QQ、网易云样本并排序，失败继续换源。
- **脚本可更新**：平台搜索与发现统一在 `providers/*.js`，默认内嵌；第三方取流脚本可导入、在线获取或重新加载。详见[支持范围](providers/README.md)。
- **播放与浏览**：内置精简 FFmpeg，支持 AAC / M4A；歌手、专辑与 B 站分 P，队列、循环、定时关闭和同步歌词、中文译文和译文开关。
- **自己的资料库**：导入音乐文件夹、本地收藏、保存在线歌单，支持筛选、排序及备份恢复；歌曲单击选择、Ctrl 增减、Shift 连选，双击或点击封面播放；支持批量收藏、加入队列、下载与移除。
- **桌面播放**：系统托盘、媒体键、系统媒体控制和音频输出设备选择，Windows / Linux 均可用。
- **登录与升级**：外部官网登录，无需扩展；应用内查看 GitHub 更新说明、下载并安装新版本。

## 下载与安装

在 [GitHub Releases](https://github.com/sudoevolve/aino-player/releases/latest) 下载：

| 系统                             | 推荐下载                                         |
| -------------------------------- | ------------------------------------------------ |
| Windows x64                      | `setup-windows-x64.exe` 安装器；也提供便携 EXE |
| Ubuntu 22.04+ / Debian 12+ amd64 | `.deb` 安装包                                  |

Linux 一键安装（Ubuntu 22.04+ / Debian 12+，amd64）：

```bash
curl -fsSL https://sudoevolve.github.io/aino-player/install.sh | bash
```

自动添加签名 [APT 软件源](docs/APT.md) 并安装；之后可用 `sudo apt install aino-player` 和系统更新器。也可下载 deb 直接安装。升级保留配置、收藏和歌单。

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
