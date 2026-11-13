# 构建与发布

[返回首页](../README.md)


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
cmake -S . -B build -DAINO_BUILD_TESTS=ON
cmake --build build --config Release --parallel 2
# Linux 可加装 xvfb 运行窗口、DPI 与字体缓存检查
ctest --test-dir build -C Release --output-on-failure --no-tests=error
```

Windows 桌面媒体接入使用 Windows SDK 自带 C++/WinRT，需要 MSVC；仅桌面接入编译单元启用 C++20，其余为 C++17。产物为 `build/aino_player.exe`（MSVC 为 `build/Release/aino_player.exe`），Linux 为 `build/aino_player`。Windows 静态链接 C/C++ 运行库；Linux 使用系统 libcurl、X11、libdbus-1 和动态加载的字体/音频库；系统媒体与托盘不加载 GTK/Qt。文件选择器使用系统 xdg-desktop-portal，未安装时仍可输入路径。

Windows 安装器使用构建时的 Inno Setup 6，不作为播放器运行依赖：

```powershell
python tools/build_installer.py --exe build/Release/aino_player.exe --output build/installer
```

脚本优先使用本机 ISCC；缺失时下载固定的官方编译器并校验 SHA-256，准备到 `build/tools/inno`。安装器版本来自 CMake，资源及压缩安装器由发布工作流一并打包。图标源文件为 `assets/aino.svg`；正常构建直接使用已生成资源，只有重生成时才需 Pillow 并运行 `python tools/build_icon.py`。

精简 FFmpeg 只需首次构建，之后直接构建播放器。再次运行解码器脚本会复用相同源码版本、编译选项和工具链的配置，执行 GNU make 增量检查；产物不变时保留时间戳，不触发应用重新嵌入。已有构建目录如已开启测试，可用 `-DAINO_BUILD_TESTS=OFF` 关闭，或用 `cmake --build build --config Release --target aino_player --parallel 2` 只编译程序。


## 发布流程

`CMakeLists.txt` 中的项目版本是版本号来源。仅在明确要求发布新版本时运行回归，普通提交不触发 CI。推送对应 `v*` 标签后，`release.yml` 分别在 Windows 和 Ubuntu 编译精简 FFmpeg、嵌入播放器并运行无系统解码器的回归；Linux 生成 deb 后验证安装与 Xvfb 启动。两端成功后发布 exe、deb、FFmpeg 官方原始源码包、构建信息、许可及 `SHA256SUMS.txt`。当前 `0.2.7` 对应标签 `v0.2.7`，标签与程序版本不符或缺少内置解码器会中止发布；不会覆盖旧版标签或安装包。

本地 Debian 打包：

```bash
python3 tools/build_ffmpeg.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DAINO_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure --no-tests=error
cpack --config build/CPackConfig.cmake -G DEB -B dist
```
