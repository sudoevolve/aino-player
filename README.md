# 你可以在issues提出任何需求，这是一个vibe coding项目，核心是跨平台超低占用的多源播放器

# aino player

<img src="assets/aino.svg" width="80" height="80" alt="Aino Player 图标">

用 [aino](https://github.com/sudoevolve/aino) 做的桌面播放器：网易云 / QQ 音乐 / 酷我 / B 站并发检索与聚合，含实验性的 YouTube Music 适配，
播放使用单头文件 miniaudio，支持 Windows 和 Linux（X11）。默认打开「发现」，恢复上次队列；带关键词启动则直接搜索。

## 功能

- **搜索**：按开启的音源并发搜索，先完成的音源分批显示，支持「加载更多」及音源页单独重试；结果独立于歌单和播放队列，同一首自动合并成一行；药丸切源，hover 出「＋」加队列、「↓」下载
- **发现**：三家热搜（点词直接搜）+ 热门歌单（网易云含飙升 / 新歌 / 原创榜）
- **播放队列**：底栏右侧打开「待播清单」，与歌词共用右侧栏，顶部切换；左侧不再单列正在播放页。队列保留全部曲目、备用音源、当前歌曲与位置，重启后手动续播；下一首后台预加载，流式播放接近结束才预加载，前台点播优先；支持拖动行首系统菜单图标调整队列顺序；底栏播放模式图标切换顺序、随机、列表循环、单曲循环，模式自动保存
- **歌曲右键菜单**：搜索、歌单详情和队列里的歌曲均可右键，选择下一首播放、添加到播放列表、收藏或下载；下一首播放保留当前曲目，优先于随机和单曲循环
- **睡眠定时器**：底栏选择 15 / 30 / 60 / 90 分钟后停止，或播完当前歌曲；紧凑菜单显示状态和剩余时间，禁用不可用的取消项。计时不因暂停而暂停，重新启动不恢复定时器
- **歌单**：本地、账号与发现页歌单均打开独立详情页，含返回、曲目列表和播放全部；点击歌曲后才建立播放队列，不改在线搜索词；「同步到此」写回账号
- **系统媒体控制**：Windows SMTC、Linux MPRIS，支持桌面媒体面板、媒体键、播放/暂停、切歌和定位
- **后台播放**：「设置 → 通用」可开启托盘与关闭到托盘；隐藏时停止绘制业务界面，继续播放和计时。Linux 使用 StatusNotifierItem / DBusMenu，无托盘宿主时关闭仍会退出
- **下载管理**：左侧「下载管理」查看本次运行的等待、下载、失败和完成记录；支持单项/全部取消、失败重试、清理记录和打开文件位置，清理记录不会删除音乐
- **最近播放**：左侧「最近播放」保存最近 200 首，播放至 5 秒后去重记录，可直接重播、右键操作或清空；与本地资料库一起保存及备份
- **输出设备**：「设置 → 播放」刷新并选择音频输出，保存选择；切换时保留位置及暂停状态，设备失效时恢复系统默认输出
- **歌词**：三源歌词，居中跟随、hover 高亮、点行跳转；「校准」按歌曲保存提前/延后偏移，也可选择 UTF-8 本地 LRC 或恢复默认
- **音质**：标准 / 高 / 无损；下载可内嵌封面与歌词（ID3v2.3 / MP4 ilst）
- **本地收藏**：单个后台写入任务合并连续操作，保存成功后发布新资料库，写失败保留原文件并提示；歌曲行、队列和播放条都能收藏；播放条的收藏按钮位于歌词按钮左侧，已收藏时高亮
- **本地歌单**：「歌单 → 本地」可导入音乐文件夹、把播放队列保存为命名歌单，也能保存发现页和账号中的歌单
- **保存其他在线歌单**：在本地页粘贴网易云或 QQ 的公开歌单长链接，保存曲目快照并可手动刷新；刷新失败保留原快照
- **官网登录**：默认 Chrome、Edge 或 Chromium 打开独立的外部官网窗口，由官网处理扫码、密码和验证码；登录态自动回传，无需扩展或内嵌 WebView
- **GitHub 更新**：启动时在后台检查最新正式版；「设置 → 通用 → 检查更新」可手动检查、打开发布说明。Windows 优先下载安装器，校验大小和 SHA-256 后正常退出、覆盖升级并重新打开；Linux 下载 deb
- **关于**：「设置 → 关于」包含版本、作者 sudoevolve、Apache-2.0 协议、项目地址及第三方许可；使用黑白灰的几何 A / 播放箭头图标
- 深色模式；音量使用统一矢量图标，区分静音、低音量和高音量
- 设置按通用、账号、播放、下载、数据和关于分类；底栏采用播放控制、居中曲目信息与进度、音量及工具三个区域
- 底栏播放控制使用系统图标接口：Windows 使用系统 Segoe Fluent Icons / MDL2 Assets；播放模式弹层显示四种模式及当前勾选项。Linux 读取桌面系统图标主题，按主题继承回退到 Adwaita / hicolor，不再依赖字体是否含有播放符号

本地收藏和保存歌单不要求登录播放器账号，但平台可能要求登录才能读取某些在线歌单。在线歌单保存曲目元数据和歌单引用，播放仍需联网；文件夹歌单只保存本地音频路径，可以离线播放。公开链接示例：

```text
https://music.163.com/#/playlist?id=123
https://y.qq.com/n/ryqq/playlist/123
```

## 导入本地音乐

打开「歌单 → 本地」，在第三行选择音乐文件夹或粘贴路径（Windows / Linux），点击「导入文件夹」。默认包含子文件夹，可以关闭。支持 MP3、FLAC、WAV、M4A、AAC、Ogg、Opus、WebM；AAC/M4A（含 ALAC）、Ogg/Opus 和 WebM 由内置精简 FFmpeg 解码，无需用户安装系统 FFmpeg。

Windows 文件夹选择器在独立 STA 任务打开，Linux 通过桌面文件门户打开；门户不可用时可粘贴路径。关闭应用会取消选择器，重复点击不会叠加窗口。导入后点击歌单进入独立详情，点击一首或「播放全部」开始播放。「管理」默认折叠，展开后可歌单内搜索、按歌名/歌手/专辑/时长排序、重命名、多选移除以及导出；播放按当前筛选与排序建立队列。扫描优先读取 ID3v2、FLAC/Vorbis、Ogg/Opus、MP4 ilst、WAV INFO 中的标题、歌手和专辑，并提取支持的内嵌封面；无标签时回退文件名 `歌手 - 标题` 和父目录。读取有容量上限，损坏或不支持的字段会忽略；并非支持所有标签变体。原生解码格式会在后台扫描时读取时长。与音频同名的 UTF-8 `.lrc` 自动作为本地歌词（支持 BOM），不会向在线歌词接口提交本地路径。

文件夹歌单的「更新」按钮重新扫描；重复导入同一个规范路径会更新原歌单，扫描失败保留旧列表。移动或删除文件后需重新扫描；失效文件会明确提示，不会自动换成同名在线音乐。删除歌单、清除资料库和清理缓存只移除播放器记录或生成的缓存，均不删除原音乐；转码产物保存在播放器临时缓存目录。

## 资料库备份和任务取消

「设置 → 数据」可填写 JSON 文件完整路径，导出全部收藏和歌单，或合并恢复已有备份；单歌单可在详情页「管理」中导出。导出不覆盖已有文件，先保存临时文件再排他发布；备份不包含 Cookie、音乐文件和封面原始图片。恢复合并收藏，同 key 歌单由备份快照更新；损坏备份不会替换当前资料库。

下载最多同时执行两个任务，最多排队 32 个；再次点击下载或右键「取消下载」可取消，重试使用独立取消令牌，旧任务不会改写新任务进度。文件进入最终发布阶段后会完成保存，取消只跳过后续标签处理，已保存的音乐会保留；保存不持有界面读取的状态锁。切歌、关闭窗口会取消相应 HTTP 请求和解码子进程；已接受的本地资料库修改仍会保存后退出。

## 新音源范围

「音源」与搜索页可开启酷我、B 站、YouTube；旧配置的开关保持不变，新音源默认关闭。搜索结果、收藏、命名快照和队列可保存这些来源的曲目 ID，临时播放 URL 不持久化。

- 酷我：优先使用 HTTPS `convert_url3` JSON 取流，回退旧的纯文本接口；不把请求的 320k 当成真实音质，受限曲目仍可能只有试听。
- B 站：WBI 签名搜索公开的视频；结果行的「分 P」打开独立列表，可逐 P 播放、加入队列、收藏、下载，也可保存整份分 P 快照并刷新。取流使用所选 cid，不同 P 的状态独立；普通结果直接点播仍使用默认 P。下载带站点 Referer，付费、地区或登录限制仍按平台返回错误。
- YouTube Music：**实验性适配**，包含官网外部浏览器登录、账号歌单、歌单链接导入、创建歌单与队列同步，以及搜索、播放和下载接入。取流优先接受直接音轨地址，必要时尝试独立的官网浏览器会话或用户已安装的 `yt-dlp`；不自动安装工具或扩展，不读取日常浏览器 Cookie。网络、地区、反机器人验证及 PO Token 要求仍可能阻止播放；不承诺所有 YouTube 音乐可播。

本机已实测 B 站搜索、音轨下载和完整解码，酷我搜索与取流正常但样本为试听。YouTube Music 的登录、真实播放、下载与账号歌单写入尚未完成实网验证，本次发布继续暂停 YouTube 网络测试；离线回归覆盖搜索结构、音轨选择、签名拒绝和无权限状态。网易云和 QQ 歌单导入沿用既有实现，YouTube 歌单接入仍属实验功能。

## 下载与安装

正式版在 [GitHub Releases](https://github.com/sudoevolve/aino-player/releases)。本次版本 v0.2.3 包含 Windows 搜索输入法及滑条反馈修复、流式播放、任务取消、渐进搜索、本地歌单管理和 Linux 资源开销优化；继续提供安装器、便携版和 deb。

Windows x64 产物同时提供 `*-setup-windows-x64.exe` 安装器和 `*-windows-x64.exe` 便携版。安装器按当前用户安装到 `%LOCALAPPDATA%\Programs\Aino Player`，不需管理员权限，提供开始菜单、可选桌面快捷方式和卸载入口。后续从设置更新会通过安装器覆盖升级；首次从便携版迁移也使用该固定目录，原便携 exe 可自行移除。Linux amd64 下载 `.deb` 并使用软件安装器，或执行：

```bash
sudo apt install ./aino-player_0.2.3_amd64.deb
```

Linux 包面向 Ubuntu 22.04+ / Debian 12+；Wayland 桌面需要 XWayland。更新和卸载保留用户配置、本地资料库及下载音乐；数据清理由播放器设置单独处理。启动检查更新不阻塞播放，每次启动检查一次，网络失败可在设置重试，不需要 GitHub 令牌。旧版更新器会在浏览器中下载安装包，新版才支持应用内下载并启动安装器。

如果 Ubuntu 双击新版 deb 后只显示灰色“已安装”，无需先卸载旧版；可在下载目录执行 `sudo apt install --no-remove ./aino-player_0.2.3_amd64.deb` 直接覆盖安装。应用文件升级不会删除用户目录中的配置和歌单。

本地待发布的 Linux 更新改动：设置中的“下载并安装”会校验 GitHub 附件大小与 SHA-256，再通过系统 pkexec 授权调用 apt-get 覆盖安装；成功后手动重启播放器。需要 apt-get、pkexec 和正在运行的桌面授权代理。授权取消不会安装，安装失败保留错误信息以供排查；详细输出写入配置目录的 `update-install.log`，不强杀安装中的 dpkg。deb 维护者补为 `sudoevolve <sudoevolve@gmail.com>`，AppStream 作者显示 `sudoevolve@gmail.com`，并补充项目、许可和版本资料。此改动尚未包含在 v0.2.3 安装包中。

Windows exe 内含精简 FFmpeg，体积主要来自音频解码器和静态运行库；这是独立运行和支持 AAC/M4A 的代价。安装器采用 LZMA2 压缩，减少下载大小，不增加常驻更新进程或运行时依赖。

## 构建

先编译精简解码器，再构建播放器。解码器源码固定为 FFmpeg 8.1.3，从官网获取并校验 SHA-256；构建时需要 Python 3.10.12+、GCC、GNU make 和 Bash。Windows 可用 MSYS2 UCRT64，或 MinGW-w64 + Git Bash。只在构建时下载源码，播放器运行时不会下载解码组件。

```bash
# 首次构建解码器；生成 build/codec/ffmpeg.exe（Windows）或 ffmpeg（Linux）
python tools/build_ffmpeg.py

# Windows 播放器：MSVC + Windows SDK（含 C++/WinRT）；解码器仍用 GCC 构建
cmake -S . -B build -A x64
cmake --build build --config Release

# Ubuntu / Debian（Linux 使用 X11，Wayland 桌面需 XWayland）
sudo apt-get install build-essential python3 cmake pkg-config libdbus-1-dev libx11-dev libcurl4-openssl-dev libfreetype6 libfontconfig1 libgdk-pixbuf-2.0-0 librsvg2-common adwaita-icon-theme fonts-noto-cjk xdg-utils xdg-desktop-portal
python3 tools/build_ffmpeg.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# 需要回归时显式开启；普通构建默认只编译播放器
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release --parallel 2
# Linux 可加装 xvfb 运行窗口、DPI 与字体缓存检查
ctest --test-dir build -C Release --output-on-failure
```

Windows 桌面媒体接入使用 Windows SDK 自带 C++/WinRT，需要 MSVC；仅桌面接入编译单元启用 C++20，其余为 C++17。产物为 `build/aino_player.exe`（MSVC 为 `build/Release/aino_player.exe`），Linux 为 `build/aino_player`。Windows 静态链接 C/C++ 运行库；Linux 使用系统 libcurl、X11、libdbus-1 和动态加载的字体/音频库；系统媒体与托盘不加载 GTK/Qt。文件选择器使用系统 xdg-desktop-portal，未安装时仍可输入路径。

Windows 安装器使用构建时的 Inno Setup 6，不作为播放器运行依赖：

```powershell
python tools/build_installer.py --exe build/Release/aino_player.exe --output build/installer
```

脚本优先使用本机 ISCC；缺失时下载固定的官方编译器并校验 SHA-256，准备到 `build/tools/inno`。安装器版本来自 CMake，资源及压缩安装器由发布工作流一并打包。图标源文件为 `assets/aino.svg`；正常构建直接使用已生成资源，只有重生成时才需 Pillow 并运行 `python tools/build_icon.py`。

精简 FFmpeg 只需首次构建，之后直接构建播放器。再次运行解码器脚本会复用相同源码版本、编译选项和工具链的配置，执行 GNU make 增量检查；产物不变时保留时间戳，不触发应用重新嵌入。已有构建目录如已开启测试，可用 `-DBUILD_TESTING=OFF` 关闭，或用 `cmake --build build --config Release --target aino_player --parallel 2` 只编译程序。

## Linux 缩放与内存诊断

Linux 仍使用 X11/XWayland 后端，自动读取 XSETTINGS 的 DPI/窗口倍率及 Xft.dpi；运行中响应桌面设置变化。窗口、字体、尺寸约束和鼠标坐标统一缩放。优先级为 `AINO_SCALE`、`GDK_SCALE`、桌面设置、默认 1；前两项接受 0.5～4 的倍率。未使用 GDK_DPI_SCALE 放大整个窗口，以免把单独的字体设置重复应用。

```bash
# 桌面没有正确传递 X11 DPI 时，可手动覆盖（支持 1.25/1.5 等）
AINO_SCALE=1.5 aino_player
# 离线绘制一帧，输出倍率、尺寸、字体缓存、RSS、PSS 及 Private 页后退出
aino_player --diagnostics
```

此前已将 FreeType face 改为按字体文件和索引共享，字形位图缓存限制为 4 MiB / 16384 项；封面最多两个后台下载/解码任务、32 个待处理请求，完成与失败条目均参与缓存回收。此前 Ubuntu 24.04 / Xvfb CI 通过 8 项测试，空资料库、未播放且无封面时的首帧诊断 RSS 为 31032 kB（约 30.3 MiB，倍率 1），其中匿名页 8208 kB、文件页 22824 kB。这不是反馈机器的前后对照，也不代表播放与封面加载后的占用。XWayland 的混合 DPI、多显示器与合成器二次缩放仍需实机检查，不宣称原生 Wayland 支持。

针对相同歌词画面的后续修复保留布局、字号、字体选择、歌词动画与 60 fps 策略：查 Fontconfig 字符覆盖集合后再打开字体，避免缺字加载整条回退链；队列和搜索结果按版本原地保留，避免逐帧复制；本地歌单绘制只读引用。Linux JPEG/PNG 封面优先用 GdkPixbuf 在解码阶段请求目标尺寸，JPEG 可降采样，PNG 仍可能分配完整中间图；失败回退 stb，保持原有格式和尺寸上限。图标同样使用系统 GdkPixbuf/SVG loader，不初始化 GTK。

这批修复仅做编译、静态检查与代码审查，尚未在反馈者 Ubuntu 的相同播放场景复测，不能承诺降至 Windows 的 20 MB。`--diagnostics` 的 Private/PSS/RSS 仅为首帧数据；比较播放内存需在相同画面、窗口大小和曲目状态下读取运行进程，不能用首帧数据代替。

后续本地待发布优化：封面由最多 160 项、按 URL 顺序清理，改为最多 160 项且缓存像素总量不超过 12 MiB，按最近访问时间回收；不改变封面尺寸或歌词画面。预算仅约束缓存持有的 RGBA 像素，不包括解码中间图、外部仍持有的图片和系统库。主线程在 tick 中回收已完成后台线程，不再只等下一次创建任务才 join。尚未在用户反馈的 Ubuntu 场景测量，不能将这些上限等同于整个进程内存或声称已从 86 MB 降至 30 MB。

随后按用户明确要求运行了独立的 GitHub Actions 持续播放内存诊断：Ubuntu 22.04/Xvfb、相同歌词与音频，分别对比 v0.2.3 和内存优化版的 100%/200% 缩放。200% 下封面压力 RSS 峰值为 84.29 → 74.52 MiB；无封面 AAC 播放基本不变，为 61.28 → 61.49 MiB。该次未执行整套回归或发布，未改变画面，也不代表用户 Wayland 桌面已验证。[完整方法、数据与局限](docs/Linux-memory-profile.md)。

进一步的图标隔离和字体冷资源回收已完成专项实测：100% AAC 主进程 RSS 49.08 → 34.96 MiB（含 FFmpeg 的进程树 54.84 → 40.85）；200% 为 59.56 → 45.59 MiB。图标和中文字重画布逐字节一致。空缓存首帧多约 0.12～0.16 秒，短时进程树启动峰值升高；详见上述报告。已接入 aino 底层修复 `f8943c2`，待后续发布；不代表真实 Wayland 桌面已验证。

进一步使用相同画布、歌词、音频及实时软件设备进行跨平台归因：100% AAC 播放主进程 Linux 49.20 / Windows 32.72 MiB，计入 FFmpeg 与控制台宿主后为 55.10 / 49.49 MiB。Linux 系统图标加载单项对照净增加约 10.39 MiB；字体文件映射约 9.2 MiB，像素缓冲则是双方共同成本。Windows 私有提交不能与 Linux RSS 混比。完整原始结果和失败尺寸校验的处理见上述报告；没有修改产品画面、执行整套回归或发布。

本地 MP3、FLAC、WAV 由 miniaudio 直接解码。在线点播优先通过播放器的 HTTP 层向内置 FFmpeg 管道送数据，输出 48 kHz 双声道 PCM，经固定三秒环形缓冲交给 miniaudio；不落整首压缩文件或 PCM WAV。缓冲不足时暂时输出静音且不推进曲目位置，实际失败会提示并恢复完整文件播放。本地 AAC/M4A、Ogg/Opus、WebM 同样优先通过 FFmpeg 管道解码。

网络拖动进度会重新连接并从头解码丢弃至目标，保留暂停状态，等待时显示缓冲；不承诺 HTTP Range 快速随机定位。尾部 moov 等不能通过单向管道读取的容器会退回整首缓存和 WAV 解码；起播后断流也尝试完整文件恢复。尚未实现无缝切歌。内置 FFmpeg 只启用本地文件和管道协议，不包含网络、视频解码或额外外部库；无需 Windows Media Foundation。

上述流式播放、管理与输入法修复纳入 v0.2.3，Windows 搜索中文输入已由用户实机确认恢复正常。用户明确要求发布后，由发布流程统一执行 Windows/Linux 编译和回归，通过后打包发布；不将 CI 或 Xvfb 检查视为反馈者 Ubuntu 播放场景的实机验证。YouTube 实网测试继续暂停。
解码器字节嵌入播放器可执行文件，首次需要时释放到本进程的私有临时缓存；正常退出删除，异常退出的旧文件可通过「设置 → 数据与缓存 → 清除缓存」清理，运行中的实例会保留。原音乐文件不改写；完整文件回退解码有 120 秒上限，取消会终止子进程，失败的临时输出会清理。Linux 临时目录需允许执行程序。

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

Windows IME 按用户确认 gallery 正常的 aino `11f72ae7bc19e229551445b16306e22994018007` 恢复字段、消息处理、定位和组合窗处理，撤回远端实验；保留模态绘制重入保护和其他平台修复。播放器入口在图标和音频初始化之前建立 UI STA，封面解码配对释放 COM 引用，退出时先释放音频引擎；用户已实机确认播放器搜索中文输入正常。音量和进度条在松手帧显示刚提交的值，并补绘下一帧；浮层遮挡或失去捕获时取消拖动，不提交旧位置。

网易云已登录时，标准音质也使用账号播放地址接口；缺少码率元数据不会丢弃有效地址。接口失败保留实际拒绝码和免费外链失败原因，不再统一推断为会员问题或要求已登录用户填 Cookie。没有播放地址时尚未进入流式解码，不能将这种失败等同于 FFmpeg 故障。

已同步上游 aino 的 `d14cad3` 字体内存、桌面 DPI 和输入捕获修复；本地保留专辑位图绘制、系统消息字体选择、窗口激活与 X11 跨线程唤醒扩展。控件因刷新或切页消失、窗口失焦或鼠标捕获丢失后会释放按下状态，避免主界面无法点击而歌词仍可交互。

## 已知限制

- Windows 已完成实际窗口与音频播放验证；Linux 已通过 CI 编译、离线业务回归与 Xvfb 窗口启动检查，真实音频设备及 Linux 手机扫码登录仍待实机验证
- 流式播放仍受网络与容器限制；不能走单向管道的音频退回整首缓存，网络 seek 需要重新解码到目标，尚无无缝切歌
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

`CMakeLists.txt` 中的项目版本是版本号来源。仅在明确要求发布新版本时运行回归，普通提交不触发 CI。推送对应 `v*` 标签后，`release.yml` 分别在 Windows 和 Ubuntu 编译精简 FFmpeg、嵌入播放器并运行无系统解码器的回归；Linux 生成 deb 后验证安装与 Xvfb 启动。两端成功后发布 exe、deb、FFmpeg 官方原始源码包、构建信息、许可及 `SHA256SUMS.txt`。当前 `0.2.3` 对应标签 `v0.2.3`，标签与程序版本不符或缺少内置解码器会中止发布；不会覆盖旧版标签或安装包。

本地 Debian 打包：

```bash
python3 tools/build_ffmpeg.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cpack --config build/CPackConfig.cmake -G DEB -B dist
```

## 流式播放与定位

在线音频优先通过有界 PCM 缓冲边解码边播放。远端正确支持 HTTP Range 时，播放器以随机路径的本机 HTTP 地址提供经过范围校验的数据，FFmpeg 按容器索引读取和定位；远端 HTTPS、Cookie 和请求头仍由播放器处理，不交给 FFmpeg。每连接数据块上限 256 KiB，最多四个连接；PCM 缓冲仍为三秒。定位速度取决于容器索引和远端响应，不支持 Range 或解码失败时保留顺序管道/完整文件回退。

Linux 使用 X11 或 Wayland 下的 XWayland。桌面媒体控制需要会话 D-Bus；托盘需要 StatusNotifier 宿主，纯 GNOME 未启用托盘支持时不会隐藏窗口。源码编译通过不等于真实耳机、热插拔、桌面托盘和网络定位均已实机验证；发布时再统一回归。
