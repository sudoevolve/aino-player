## aino player v0.2.7

### 音源与发现

- 发现与搜索新增酷狗、酷我、咪咕目录，保留网易云、QQ 和 B 站；酷狗增加推荐歌单，修复歌单曲目接口与歌曲封面信息。
- 修复在线下载音源后未加载的问题。应用内移除音源只移除使用记录，保留原始 JS 文件；主动导入或重新获取可恢复。
- 完善落雪音源接口兼容：Promise 初始化、异步 HTTP、定时器、二进制响应和编码；获取失败时继续尝试其他可用音源。
- 后台静默检查 QQ、网易云样本，不播放声音；优先显示可用音源。样本结果与单首歌曲失败分别记录，避免状态反复变红。
- 修复 QQ 热搜旧接口间歇失败，增加备用入口；热搜先于歌单显示，刷新失败时保留已有结果。

### 交互与架构

- 长提示自动换行；音源和热搜错误提供可滚动、可复制的完整详情，Linux 也支持复制诊断。
- 平台搜索、热搜、歌单协议统一到内置 JS，支持同名外部脚本覆盖；移除重复的 C++ 接口与 YouTube 残留。
- 全局 provider 锁只负责查找与交换实例，网络请求不再占用全局锁；同一脚本的运行仍串行，避免并发进入 QuickJS。

### 安装与升级

- **Windows x64**：推荐 `aino-player-v0.2.7-setup-windows-x64.exe`；另提供 `aino-player-v0.2.7-windows-x64.exe` 便携版。
- **Ubuntu 22.04+ / Debian 12+ amd64**：下载 `aino-player_0.2.7_amd64.deb`，执行 `sudo apt install ./aino-player_0.2.7_amd64.deb`，可直接覆盖升级。
- 已添加项目签名 APT 源：`sudo apt update && sudo apt install aino-player`。首次添加方法见 [README](https://github.com/sudoevolve/aino-player#readme)。
- 更新保留配置、收藏、歌单与下载文件。附件提供 SHA-256 校验、FFmpeg 对应源码、构建信息与许可证。

### 使用边界

Linux 使用 X11 / XWayland。本版未修改已确认正常的 Windows 输入法逻辑。

第三方音源受服务可用性、会员与地区限制；静默样本成功不保证每首歌都可播放。依赖 AES、RSA、zlib 或 multipart 的音源脚本仍可能不兼容。新增平台的歌单读取目前最多 200 首，尚不包含这些平台的完整账号和歌手目录功能。
