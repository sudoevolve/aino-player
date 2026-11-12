# aino player

用 [aino](https://github.com/sudoevolve/aino) 做的桌面播放器：网易云 / QQ 音乐 / 酷我 三源并发检索与聚合，
播放使用单头文件 miniaudio，支持 Windows 和 Linux（X11）。默认打开「发现」，恢复上次队列；带关键词启动则直接搜索。

## 功能

- **搜索**：三源并发，同一首自动合并成一行；药丸切源，hover 出「＋」加队列、「↓」下载
- **发现**：三家热搜（点词直接搜）+ 热门歌单（网易云含飙升 / 新歌 / 原创榜）
- **正在播放**：队列管理（点播 / 移除 / 清空 / 列表循环），跨重启保留
- **歌单**：网易云 / QQ 的自建歌单，点开即播，「同步到此」写回账号
- **歌词**：三源歌词，居中跟随、hover 高亮、点行跳转
- **音质**：标准 / 高 / 无损；下载可内嵌封面与歌词（ID3v2.3 / MP4 ilst）
- **本地收藏**：歌曲行、队列和播放条都能收藏；播放条的收藏按钮位于歌词按钮左侧，已收藏时高亮
- **本地歌单**：「歌单 → 本地」可把当前列表保存为命名歌单，也能保存发现页和账号中的歌单
- **保存其他在线歌单**：在本地页粘贴网易云或 QQ 的公开歌单长链接，保存曲目快照并可手动刷新；刷新失败保留原快照
- **官网登录**：默认 Chrome、Edge 或 Chromium 打开独立的外部官网窗口，由官网处理扫码、密码和验证码；登录态自动回传，无需扩展或内嵌 WebView
- **GitHub 更新**：启动时在后台检查最新正式版；「设置 → 版本更新」可手动检查、打开发布说明及下载对应平台的 exe / deb
- 深色模式；音量使用统一矢量图标，区分静音、低音量和高音量

本地收藏和保存歌单不要求登录播放器账号，但平台可能要求登录才能读取某些在线歌单。保存的是曲目元数据和歌单引用，播放仍需联网；离线音频请使用下载功能。公开链接示例：

```text
https://music.163.com/#/playlist?id=123
https://y.qq.com/n/ryqq/playlist/123
```

## 下载与安装

正式版在 [GitHub Releases](https://github.com/sudoevolve/aino-player/releases)。Windows x64 下载 `.exe` 直接运行；Linux amd64 下载 `.deb` 并使用软件安装器，或执行：

```bash
sudo apt install ./aino-player_0.1.0_amd64.deb
```

Linux 包面向 Ubuntu 22.04+ / Debian 12+；Wayland 桌面需要 XWayland。更新会保留用户配置和本地资料库。Windows 替换 exe 前先关闭旧程序。启动检查更新不阻塞播放，每次启动检查一次，网络失败可在设置重试，不需要 GitHub 令牌。

## 构建

```bash
# MinGW-w64（GCC 12+，UCRT；需要 CMake 和 Ninja）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# MSVC：使用本机已安装的 Visual Studio 生成器
cmake -S . -B build -A x64
cmake --build build --config Release

# Ubuntu / Debian（Linux 使用 X11，Wayland 桌面需 XWayland）
sudo apt-get install build-essential cmake libx11-dev libcurl4-openssl-dev libfreetype6 libfontconfig1 fonts-noto-cjk xdg-utils ffmpeg
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# 回归检查；Linux 可加装 xvfb 运行窗口冒烟测试
ctest --test-dir build -C Release --output-on-failure
```

Windows 需要 Windows SDK 或 mingw-w64 头文件。产物为 `build/aino_player.exe`（MSVC 为 `build/Release/aino_player.exe`），Linux 为 `build/aino_player`。Windows 静态链接 C/C++ 运行库；Linux 使用系统 libcurl、X11 和动态加载的字体/音频库。

MP3、FLAC、WAV 由 miniaudio 直接解码。**AAC/m4a 额外需要 `ffmpeg` 在 PATH 中**：后台转为临时 PCM WAV 后播放。未安装时会提示并尝试其他音源；不依赖原来的 Windows Media Foundation。

## 用法

```bash
aino_player.exe              # 打开发现并恢复上次队列
aino_player.exe "关键词"      # 带词启动
./aino_player                # Linux
```

配置与资料库目录：Windows 为 `%APPDATA%\aino-player\`；Linux 为 `$XDG_CONFIG_HOME/aino-player/`，未设置时使用 `~/.config/aino-player/`。`config.ini` 保存设置及登录态，`queue.txt` 保存队列，`library.json` 保存本地收藏和歌单。损坏的资料库不会被自动覆盖。
在「设置 → 数据与缓存」可打开数据目录，分别清除登录信息、本地收藏与歌单、当前列表和缓存；每项操作都有确认提示。音频缓存位于系统临时目录：Windows 为 `%TEMP%\aino-player\<进程号>\`，Linux 为临时目录下的 `aino-player-<用户ID>/<进程号>/`；封面缓存在内存中。清理会保留正在使用的缓存和已下载的音乐，退出播放器账号不会退出系统浏览器中的账号。
酷我默认关闭（受限曲目只给 11 秒试听），可在「音源」页手动开。

在「设置」点击「官网登录」，播放器会启动系统默认 Chromium 浏览器的专用临时会话，直接打开网易云 `https://music.163.com/` 或 QQ 音乐 `https://y.qq.com/`。点击官网的登录入口，使用官网提供的扫码、密码或其他验证方式。播放器仅从这个专用会话取得对应音乐网站的 Cookie，不读取日常浏览器配置或 Cookie 数据库。
登录成功后保存本机登录态、请求播放器回到前台，并关闭专用登录窗口和清除临时 profile。桌面系统可能限制抢占焦点，此时从任务栏切回播放器。取消、关闭窗口或超时均结束本次尝试；浏览器原有窗口与本地歌单不受影响。登录凭据保存在 `config.ini`，不会写入本地歌单。

目前自动回传支持默认浏览器为 Chrome、Edge、Chromium 等受识别的 Chromium 浏览器；Firefox 等其他内核暂不支持，会明确提示。浏览器策略若禁用本机调试连接，自动回传也可能不可用。备用手动 Cookie 导入入口保留在登录模块内，正常 UI 登录不经过本地扫码页。

可用真实浏览器执行离线回传回归（仅注入假 Cookie，不访问账号）：

```bash
python tests/browser_login_test.py build/portable/Release/player_tests.exe "C:/Program Files/Google/Chrome/Application/chrome.exe"
# Linux 可传 build/player_tests 和 /usr/bin/chromium
```

CMake 检测到受支持的浏览器时，会加入 `browser_official_handoff` CTest；其他测试不要求浏览器。手机确认登录已由用户反馈可用，自动化回归仍只注入假 Cookie，不使用实际账号。

## 应用代码结构

应用按三个文件组织：`src/main.cpp` 负责启动、窗口和主循环，`src/business.hpp` 负责业务动作、后台任务、状态快照和持久化，`src/ui.hpp` 负责布局、绘制与输入。UI 通过只读业务快照展示数据，通过业务动作执行播放、登录和清理，不直接操作网络、文件或播放设备。

两个实现头文件由 `main.cpp` 在单一编译单元中包含，保持现有单头文件工程的构建方式；已有音频、登录及平台适配模块继续复用。业务可单独编译运行测试，不需要加载 aino 或创建窗口。

## 字体与界面

Windows 默认字体跟随系统消息字体，减少中文回退导致的字形变化。文字仍通过 aino 的 GDI 灰度抗锯齿绘制，与 Windows 应用的 DirectWrite / ClearType 在粗细和清晰度上可能不同。Linux 按本机 fontconfig / FreeType 选择字体，建议安装 Noto CJK；没有捆绑微软字体，因此两平台不会像素一致。

对 aino 的本地改动包括专辑位图绘制、系统消息字体选择、X11 跨线程唤醒与输入捕获生命周期修复。控件因刷新或切页消失、窗口失焦或鼠标捕获丢失后会释放按下状态，避免主界面无法点击而歌词仍可交互。

## 已知限制

- Windows 已完成实际窗口与音频播放验证；Linux 已通过 CI 编译、离线业务回归与 Xvfb 窗口启动检查，真实音频设备及 Linux 手机扫码登录仍待实机验证
- 不做边下边播：先缓存音频再播放，起播时间取决于网络和格式；AAC 还需要转码
- 音质取决于账号：网易云无损要会员，酷我的直链跟资源走、档位无效
- 歌单写入接口无公开文档，失败时原样回传服务端 code
- 酷我没有公开的歌单广场接口，发现页那一栏只有热搜
- 接口是「公开但不承诺稳定」的 web 接口，平台一改就得改对应适配器
- 官网外部浏览器登录已覆盖真实 Chrome 离线 Cookie 回传、标签切换、取消、注销和临时目录清理；实际手机登录由用户确认可用
- 只接受明确的官方歌单长链接；分享短链接请先在浏览器中展开，私密或受限歌单依平台权限处理

## 许可

Apache-2.0。`src/aino.hpp` 版权归 sudoevolve，见 `LICENSE-aino`。
新增第三方依赖的版本、来源和许可证见 [vendor/README.md](vendor/README.md)。架构决策和验证边界见 [docs/ADR-001-portable-player.md](docs/ADR-001-portable-player.md)。

## 致谢

- [aino](https://github.com/sudoevolve/aino) —— 整个界面都靠它
- [Melodex-Music](https://github.com/LunaShiori/Melodex-Music) —— 多音源分层（每源一个适配器、聚合层做并发与合并）的思路参考了它，代码为 C++ 重写，未复制

## 发布流程

`CMakeLists.txt` 中的项目版本是版本号来源。推送对应 `v*` 标签后，`release.yml` 分别在 Windows 和 Ubuntu 构建并运行回归；Linux 生成 deb 后验证安装与 Xvfb 启动，两端都成功才发布 exe、deb 和 `SHA256SUMS.txt`。例如 `0.1.0` 对应标签 `v0.1.0`，标签与程序版本不符会中止发布。

本地 Debian 打包：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cpack --config build/CPackConfig.cmake -G DEB -B dist
```
