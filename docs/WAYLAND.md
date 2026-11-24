# 原生 Wayland 播放器

[返回首页](../README.md) · [构建指南](BUILDING.md)

从 v0.5.6 开始，源码与 Linux 安装包默认使用纯 Wayland，主程序及桌面模块不链接 X11，也不启动 XWayland。Windows 继续使用原有原生实现；Linux X11 可通过 `-DAINO_LINUX_WAYLAND=OFF` 独立构建。v0.5.5 及更早安装包仍使用旧的 X11 配置。

## 构建与依赖

Ubuntu / Debian 开发依赖：`libwayland-dev`、`wayland-protocols`、`libdbus-1-dev`、`libcurl4-openssl-dev`、CMake、Python 和 C++17 编译器。Fedora 使用 `wayland-devel`、`wayland-protocols-devel`；Arch 使用 `wayland`、`wayland-protocols`。主程序依旧需要字体与音频运行库，完整清单见构建指南。

```bash
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release -DAINO_LINUX_WAYLAND=ON -DAINO_BUILD_TESTS=OFF
cmake --build build-native --target aino_player --parallel 2
env -u DISPLAY build-native/aino_player
ldd build-native/aino_player
```

正式分发需先构建精简 FFmpeg。仅验证开发编译时可设置 `-DAINO_BUNDLE_FFMPEG=OFF`，AAC 等格式会使用系统 FFmpeg。Wayland 桌面必须提供有效的 `XDG_RUNTIME_DIR`、`WAYLAND_DISPLAY`；不在失败时自动回退到 X11。

## 功能与桌面协议

| 功能 | 原生实现及限制 |
| --- | --- |
| 主窗口、输入、剪贴板、缩放 | 复用 aino v0.5.4 的 Wayland 后端；输入法协议支持仍以 aino 和 compositor 为准 |
| 音频播放 | 原有 miniaudio / FFmpeg 管线，不依赖窗口系统 |
| 媒体控制、睡眠通知、单实例交接 | D-Bus MPRIS、login1 与播放器 IPC；没有 X11 窗口 ID |
| 托盘及右键菜单 | D-Bus StatusNotifierItem / DBusMenu；桌面必须提供托盘 host，例如 KDE 或 GNOME 扩展 |
| 隐藏及显示主窗口 | 取消映射及重新 configure；隐藏期间业务仍可响应后台唤醒，compositor 决定后台激活焦点 |
| 全局快捷键 | GlobalShortcuts Portal，异步申请并由桌面授权；组合键是建议值，以桌面确认结果为准。拒绝、取消或协议缺失会显示原因，MPRIS 继续可用 |
| 文件选择 | 原有 FileChooser Portal，无 X11 父窗口标识；未安装 Portal 时仍可输入路径 |
| 文件拖放 | 接入 aino 的有界 `text/uri-list` 异步管线，只接收本地文件路径 |
| 悬浮歌词 | 优先 layer-shell，支持透明、置顶、锁定穿透与拖动；不支持 layer-shell 的 GNOME、Weston 等使用普通 xdg-shell 歌词窗口，定位和堆叠由 compositor 管理 |

歌词窗口复用主窗口提供的字体绘制函数，使用独立 Wayland 连接、最多三个 ARGB 共享内存缓冲。compositor 释放缓冲前不覆盖，关闭后释放连接与映射；后台线程只探测连接可读性并唤醒 UI，协议派发及资源生命周期由 UI 线程管理。无额外渲染线程。

layer-shell 不是 Wayland 核心协议。GNOME 不支持它时，播放器不会启动 XWayland 来模拟置顶或覆盖顶栏。普通歌词窗口仍能显示及拖动，主窗口设置可关闭或解除锁定；未锁定时也可右键关闭。不同桌面的 layer-shell、全局快捷键授权和多屏缩放须分别实际确认。

## 发布验收

用户要求发布时，CMake 将纯 Wayland 主窗口、字体缓存与桌面集成检查纳入必需集合。`tools/run_wayland.py` 创建独立的 headless Weston 与 session bus，清除 `DISPLAY`，检查 ELF 依赖中没有 libX11/libxcb/libXext，再执行指定的检查程序；未注册必需检查的 release profile 会配置失败。

`wayland_desktop_tests` 检查无 XWayland 的歌词窗口、共享内存绘制、调整设置、关闭重开和 MPRIS；Ubuntu 发布任务还通过禁用 XWayland 的 headless Sway 验证 layer-shell 路径。Portal 使用独立 session bus 的假服务核对授权与绑定契约，不代替真实桌面的授权对话框。日常维护仍只编译和静态检查，不自动运行这些回归或启动窗口。

## 本轮编译与审查记录

2026-11-24 在 WSL Ubuntu 24.04 完成纯 Wayland 主程序、交互检查、控制器及新增桌面 / Portal 检查目标的编译，退出码为 0。配置时同时传入 `CMAKE_DISABLE_FIND_PACKAGE_X11=TRUE`，该构建没有调用 X11 查找。`ldd` 和 ELF 的 NEEDED 记录确认主程序链接 Wayland、D-Bus、libcurl，未链接 libX11、libxcb 或 libXext；这项检查不启动播放器。

X11 兼容主程序及交互检查、Windows MSVC Release 主程序及控制器 / 交互检查也已编译通过。Python 脚本通过 AST 检查，协议 XML 可解析，本地文档链接与 `git diff --check` 通过。日志位于忽略目录 `build/pure-wayland-*.log`。

代码审查覆盖了取消映射后的初始属性重发、隐藏期间业务唤醒、Portal 信号早于方法回包、取消授权与会话清理、歌词输出缓冲释放、窗口关闭重开及 Wayland/X11 构建分支。`wayland_window.hpp` 集中使用 aino 的内部协议接口，库升级时需要复核；`aino.hpp` 仅增加 Wayland 的可见性与呈现控制，未改输入法路径。

上述实现阶段遵守 `AGENTS.md`，只编译与静态检查，未执行回归或发版。随后用户明确要求提交并发布 v0.5.6，发布阶段开始运行完整验收；不能将之前的编译记录当作运行验收。

发布前本地检查已发现并修复 Weston 下旧 xdg 角色取消映射后无法重新 configure 的问题。恢复窗口改为重建角色，保留原 Wayland 连接和字体资源，不修改输入法路径。最终 v0.5.6 在 WSL 的十项 CTest（含 layer-shell）、独立 Sway 的窗口隐藏恢复检查，以及 Windows 八项验收均已通过；X11 兼容版也通过 Xvfb 启动检查。各发行包还由发布工作流统一验证。

真实 GNOME/KDE 桌面的授权对话框、输入法、跨应用拖放和混合 DPI 多屏仍需分别实际确认；headless compositor 和模拟 Portal 不等同于这些实机验收。
