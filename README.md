# 你可以在issues提出任何需求，这是一个vibe coding项目，核心是跨平台超低占用的多源播放器

# aino player

用 [aino](https://github.com/sudoevolve/aino) 做的桌面播放器：网易云 / QQ 音乐 / 酷我 / B 站并发检索与聚合，含实验性的 YouTube Music 适配，
播放使用单头文件 miniaudio，支持 Windows 和 Linux（X11）。默认打开「发现」，恢复上次队列；带关键词启动则直接搜索。

## 功能

- **搜索**：按开启的音源并发搜索，结果独立于歌单和播放队列，同一首自动合并成一行；药丸切源，hover 出「＋」加队列、「↓」下载
- **发现**：三家热搜（点词直接搜）+ 热门歌单（网易云含飙升 / 新歌 / 原创榜）
- **正在播放**：独立队列（点播 / 移除 / 清空 / 列表循环），跨重启保留；搜索和浏览歌单不会覆盖队列
- **歌单**：本地、账号与发现页歌单均打开独立详情页，含返回、曲目列表和播放全部；点击歌曲后才建立播放队列，不改在线搜索词；「同步到此」写回账号
- **歌词**：三源歌词，居中跟随、hover 高亮、点行跳转
- **音质**：标准 / 高 / 无损；下载可内嵌封面与歌词（ID3v2.3 / MP4 ilst）
- **本地收藏**：歌曲行、队列和播放条都能收藏；播放条的收藏按钮位于歌词按钮左侧，已收藏时高亮
- **本地歌单**：「歌单 → 本地」可导入音乐文件夹、把播放队列保存为命名歌单，也能保存发现页和账号中的歌单
- **保存其他在线歌单**：在本地页粘贴网易云或 QQ 的公开歌单长链接，保存曲目快照并可手动刷新；刷新失败保留原快照
- **官网登录**：默认 Chrome、Edge 或 Chromium 打开独立的外部官网窗口，由官网处理扫码、密码和验证码；登录态自动回传，无需扩展或内嵌 WebView
- **GitHub 更新**：启动时在后台检查最新正式版；「设置 → 版本更新」可手动检查、打开发布说明及下载对应平台的 exe / deb
- 深色模式；音量使用统一矢量图标，区分静音、低音量和高音量

本地收藏和保存歌单不要求登录播放器账号，但平台可能要求登录才能读取某些在线歌单。在线歌单保存曲目元数据和歌单引用，播放仍需联网；文件夹歌单只保存本地音频路径，可以离线播放。公开链接示例：

```text
https://music.163.com/#/playlist?id=123
https://y.qq.com/n/ryqq/playlist/123
```

## 导入本地音乐

打开「歌单 → 本地」，在第三行选择音乐文件夹（Windows）或粘贴路径（Windows / Linux），点击「导入文件夹」。默认包含子文件夹，可以关闭。支持 MP3、FLAC、WAV、M4A、AAC、Ogg、Opus、WebM；AAC/M4A（含 ALAC）、Ogg/Opus 和 WebM 由内置精简 FFmpeg 解码，无需用户安装系统 FFmpeg。

导入后点击歌单进入独立详情，点击一首或「播放全部」开始播放。文件名 `歌手 - 标题` 会拆为歌手和标题；其他文件使用文件名，专辑取父目录名，尚不解析完整 ID3/Vorbis 标签。原生解码格式会在后台扫描时读取时长。与音频同名的 UTF-8 `.lrc` 自动作为本地歌词（支持 BOM），不会向在线歌词接口提交本地路径。

文件夹歌单的「更新」按钮重新扫描；重复导入同一个规范路径会更新原歌单，扫描失败保留旧列表。移动或删除文件后需重新扫描；失效文件会明确提示，不会自动换成同名在线音乐。删除歌单、清除资料库和清理缓存只移除播放器记录或生成的缓存，均不删除原音乐；转码产物保存在播放器临时缓存目录。

## 新音源范围

「音源」与搜索页可开启酷我、B 站、YouTube；旧配置的开关保持不变，新音源默认关闭。搜索结果、收藏、命名快照和队列可保存这些来源的曲目 ID，临时播放 URL 不持久化。

- 酷我：优先使用 HTTPS `convert_url3` JSON 取流，回退旧的纯文本接口；不把请求的 320k 当成真实音质，受限曲目仍可能只有试听。
- B 站：WBI 签名搜索公开的视频；结果行的「分 P」打开独立列表，可逐 P 播放、加入队列、收藏、下载，也可保存整份分 P 快照并刷新。取流使用所选 cid，不同 P 的状态独立；普通结果直接点播仍使用默认 P。下载带站点 Referer，付费、地区或登录限制仍按平台返回错误。
- YouTube Music：**实验性适配**，解析 Music 搜索的歌曲列表与双行卡片，优先从官网读取客户端配置；取流只接受明文音轨地址。需要签名或平台验证时，可使用用户已安装、位于 PATH 的 `yt-dlp` 兜底，工具运行有超时，不自动安装，不要求浏览器扩展，也不读取日常浏览器 Cookie。网络、地区、反机器人验证及 PO Token 要求仍可能阻止播放；不承诺所有 YouTube 音乐可播。

本机已实测 B 站搜索、音轨下载和完整解码，酷我搜索与取流正常但样本为试听；YouTube Music 在当前出口仅返回账号/播客，没有可播歌曲，因此未完成 YouTube 的真实播放验证。离线回归覆盖搜索结构、音轨选择、签名拒绝和无权限状态。新音源仅提供搜索与取流，在线歌单链接导入仍支持网易云和 QQ。

## 下载与安装

正式版在 [GitHub Releases](https://github.com/sudoevolve/aino-player/releases)。Windows x64 下载 `.exe` 直接运行；Linux amd64 下载 `.deb` 并使用软件安装器，或执行：

```bash
sudo apt install ./aino-player_0.1.0_amd64.deb
```

Linux 包面向 Ubuntu 22.04+ / Debian 12+；Wayland 桌面需要 XWayland。更新会保留用户配置和本地资料库。Windows 替换 exe 前先关闭旧程序。启动检查更新不阻塞播放，每次启动检查一次，网络失败可在设置重试，不需要 GitHub 令牌。

## 构建

先编译精简解码器，再构建播放器。解码器源码固定为 FFmpeg 8.1.3，从官网获取并校验 SHA-256；构建时需要 Python 3.10.12+、GCC、GNU make 和 Bash。Windows 可用 MSYS2 UCRT64，或 MinGW-w64 + Git Bash。只在构建时下载源码，播放器运行时不会下载解码组件。

```bash
# 首次构建解码器；生成 build/codec/ffmpeg.exe（Windows）或 ffmpeg（Linux）
python tools/build_ffmpeg.py

# MinGW-w64（GCC 12+，UCRT；需要 CMake 和 Ninja）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# MSVC：使用本机已安装的 Visual Studio 生成器
cmake -S . -B build -A x64
cmake --build build --config Release

# Ubuntu / Debian（Linux 使用 X11，Wayland 桌面需 XWayland）
sudo apt-get install build-essential python3 cmake libx11-dev libcurl4-openssl-dev libfreetype6 libfontconfig1 fonts-noto-cjk xdg-utils
python3 tools/build_ffmpeg.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# 需要回归时显式开启；普通构建默认只编译播放器
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release --parallel 2
# Linux 可加装 xvfb 运行窗口、DPI 与字体缓存检查
ctest --test-dir build -C Release --output-on-failure
```

Windows 需要 Windows SDK 或 mingw-w64 头文件。产物为 `build/aino_player.exe`（MSVC 为 `build/Release/aino_player.exe`），Linux 为 `build/aino_player`。Windows 静态链接 C/C++ 运行库；Linux 使用系统 libcurl、X11 和动态加载的字体/音频库。

精简 FFmpeg 只需首次构建，之后直接构建播放器。再次运行解码器脚本会复用相同源码版本、编译选项和工具链的配置，执行 GNU make 增量检查；产物不变时保留时间戳，不触发应用重新嵌入。已有构建目录如已开启测试，可用 `-DBUILD_TESTING=OFF` 关闭，或用 `cmake --build build --config Release --target aino_player --parallel 2` 只编译程序。

## Linux 缩放与内存诊断

Linux 仍使用 X11/XWayland 后端，自动读取 XSETTINGS 的 DPI/窗口倍率及 Xft.dpi；运行中响应桌面设置变化。窗口、字体、尺寸约束和鼠标坐标统一缩放。优先级为 `AINO_SCALE`、`GDK_SCALE`、桌面设置、默认 1；前两项接受 0.5～4 的倍率。未使用 GDK_DPI_SCALE 放大整个窗口，以免把单独的字体设置重复应用。

```bash
# 桌面没有正确传递 X11 DPI 时，可手动覆盖（支持 1.25/1.5 等）
AINO_SCALE=1.5 aino_player
# 离线绘制一帧，输出倍率、物理/逻辑尺寸、字体缓存及进程 RSS 后退出
aino_player --diagnostics
```

本轮将 FreeType face 改为按字体文件和索引共享，字形位图缓存限制为 4 MiB / 16384 项；封面最多两个后台下载/解码任务、32 个待处理请求，完成与失败条目均参与缓存回收。Ubuntu 24.04 / Xvfb CI 已通过 8 项测试，覆盖动态 DPI、鼠标坐标、字体共享和内置解码。空资料库、未播放且无封面时的首帧诊断 RSS 为 31032 kB（约 30.3 MiB，倍率 1），其中匿名页 8208 kB、文件页 22824 kB；不是原反馈机器的前后对照，也不代表播放与封面加载后的占用。不能保证从反馈的 150 MB 降到 17 MB。XWayland 的混合 DPI、多显示器与合成器二次缩放仍需实机检查，本轮不宣称原生 Wayland 支持。

MP3、FLAC、WAV 由 miniaudio 直接解码。**AAC/M4A、Ogg/Opus、WebM 使用内置精简 FFmpeg**：后台解码成缓存目录中的 PCM WAV，再交给 miniaudio 播放。Windows 精简解码器约 2.97 MiB；不包含网络、视频解码或额外外部库，不依赖 Windows Media Foundation。播放器仍先缓存、后解码，不做边下边播。

解码器字节嵌入播放器可执行文件，首次需要时释放到本进程的私有临时缓存；正常退出删除，异常退出的旧文件可通过「设置 → 数据与缓存 → 清除缓存」清理，运行中的实例会保留。原音乐文件不改写；解码有 120 秒上限，失败的临时输出会清理。Linux 临时目录需允许执行程序。

「设置」底部显示解码器版本、体积和许可。命令行 `--codec-info` 查看内置信息，`--ffmpeg-license` 输出完整许可、源码校验值和编译选项；`AINO_FFMPEG` 可指定替代解码器路径。默认构建必须带解码器，缺失时 CMake 明确报错；仅开发时可显式使用 `-DAINO_BUNDLE_FFMPEG=OFF`，此时才依赖系统 FFmpeg。外部精简产物可用 `-DAINO_FFMPEG_EXECUTABLE=/absolute/path/to/ffmpeg` 指定。

内置解码回归使用仓库内的静音样本，不安装系统 FFmpeg，并清空子进程 PATH。无设备 CI 运行解码检查；真实设备可执行：

```bash
python tests/audio_formats_test.py build/portable/Release/player_tests.exe --bundled
# Linux 将二进制路径替换为 build/player_tests；没有音频设备时加 --decode-only
```

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

`CMakeLists.txt` 中的项目版本是版本号来源。推送对应 `v*` 标签后，`release.yml` 分别在 Windows 和 Ubuntu 编译精简 FFmpeg、嵌入播放器并运行无系统解码器的回归；Linux 生成 deb 后验证安装与 Xvfb 启动。两端成功后发布 exe、deb、FFmpeg 官方原始源码包、构建信息、许可及 `SHA256SUMS.txt`。例如 `0.1.0` 对应标签 `v0.1.0`，标签与程序版本不符或缺少内置解码器会中止发布。已有 v0.1.0 仍是旧版，需新版本发布后才包含上述新功能。

本地 Debian 打包：

```bash
python3 tools/build_ffmpeg.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cpack --config build/CPackConfig.cmake -G DEB -B dist
```
