# 构建与发布

[返回首页](../README.md)


先编译精简解码器，再构建播放器。解码器源码固定为 FFmpeg 8.1.3，从官网获取并校验 SHA-256；构建时需要 Python 3.10.12+、GCC、GNU make 和 Bash。Windows 可用 MSYS2 UCRT64，或 MinGW-w64 + Git Bash。只在构建时下载源码，播放器运行时不会下载解码组件。

```bash
# 首次构建解码器；生成 build/codec/ffmpeg.exe（Windows）或 ffmpeg（Linux）
python tools/build_ffmpeg.py

# Windows 播放器：MSVC + Windows SDK（含 C++/WinRT）；解码器仍用 GCC 构建
cmake -S . -B build -A x64
cmake --build build --config Release

# Ubuntu / Debian（默认 X11；Wayland 窗口仍使用 X11 桌面集成）
sudo apt-get install build-essential python3 cmake pkg-config libdbus-1-dev libx11-dev libcurl4-openssl-dev libfreetype6 libfontconfig1 libgdk-pixbuf-2.0-0 librsvg2-common adwaita-icon-theme fonts-noto-cjk xdg-utils xdg-desktop-portal
python3 tools/build_ffmpeg.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# 原生 Wayland 窗口（运行时需 Wayland compositor）
cmake -S . -B build-wayland -DCMAKE_BUILD_TYPE=Release -DAINO_LINUX_WAYLAND=ON
cmake --build build-wayland --target aino_player --parallel

# 仅在用户明确要求发布新版本时开启并运行回归；普通构建默认只编译播放器
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

脚本默认使用固定版本的官方编译器：校验下载包 SHA-256 后安装到 `build/tools/inno`，并检查缓存编译器的版本标识和二进制摘要。只有显式设置 `AINO_ALLOW_SYSTEM_ISCC=1` 才使用本机已有 ISCC；该覆盖用于开发，不能据此声称使用了固定工具链。安装器版本来自 CMake，资源及压缩安装器由发布工作流一并打包。图标源文件为 `assets/aino.svg`；正常构建直接使用已生成资源，只有重生成时才需 Pillow 并运行 `python tools/build_icon.py`。

仅更新本地 Windows 安装时，可配置 `-DAINO_LOCAL_REVISION=1 -DAINO_BUILD_TESTS=OFF`。正式项目版本保持不变，程序显示、Windows 文件属性及安装器增加第四段（例如 `0.5.1.1`）；更新检查仍比较三段正式版本。安装器从待打包 EXE 读取并核对版本。准备正式发布前用 `-DAINO_LOCAL_REVISION=0` 恢复默认；该值只存于本机构建缓存，不创建标签或发布。

精简 FFmpeg 只需首次构建，之后直接构建播放器。再次运行解码器脚本会复用相同源码版本、编译选项和工具链的配置，执行 GNU make 增量检查；产物不变时保留时间戳，不触发应用重新嵌入。已有构建目录如已开启测试，可用 `-DAINO_BUILD_TESTS=OFF` 关闭，或用 `cmake --build build --config Release --target aino_player --parallel 2` 只编译程序。

主程序沿用 main、UI 和业务的少文件布局：`main.cpp` 定义 `AINO_PLAYER_INTERFACE`，只编译业务声明；业务实现由现有 `cover_decode.cpp` 在 `AINO_PLAYER_BUSINESS_UNIT` 构建宏下承载，后者同时保留 Linux 封面解码职责。业务实现只编译一份，UI 通过业务动作与 ViewModel 访问，不直接访问业务全局变量。测试目标保留原来的头文件实现方式。

`AINO_PLAYER_PCH=ON` 默认缓存 aino 公共声明，可用 `-DAINO_PLAYER_PCH=OFF` 禁用；业务单元跳过该 PCH。aino 0.5.4 的公共实现不再全部为 inline，因此 PCH 不定义 `AINO_IMPLEMENTATION`，由 `main.cpp` 提供唯一实现，避免 PCH 对象与主程序重复定义。实现部分仍需随主单元编译，不能沿用旧版 PCH 的性能预期。

内置 UI 库同步上游标签 `v0.5.4`（提交 `74d86d49b8c8848d5a8b3b9c75f917a23795d247`），三方差异中保留播放器扩展：封面位图、系统字体、菜单选中／禁用及箭头控制、完整提示、主题色选择、窗口激活和关闭回调。剪贴板写入迁移到新版可检查接口；Windows 原生 IME、候选定位和文字提交保留用户确认版本，未确认的定时器实验未进入发布候选。合并或模拟输入都不能代替实机确认。

Windows 消息泵另有播放器修补：`Window::poll` 在原生消息派发后再次消费 wake 请求，保证托盘／媒体回调提交的业务事件在本轮可触发帧回调，不修改输入法分支。

自有代码使用 `.editorconfig` 和 `.clang-format` 约定，只整理正在修改的范围。上游 `aino.hpp`、miniaudio、vendor、资源和构建产物排除在格式化之外；纯格式修改独立提交，不对整个仓库自动格式化。`python tools/check_file_associations.py` 静态核对扫描器公共格式清单、Windows 安装器和 Linux desktop 的文件关联声明，安装器构建与下次 Linux 发布都会执行它。

日常 Linux 编译可手动运行 `desktop-build.yml`，仅在 Ubuntu 22.04 / 24.04 编译 `aino_player`，明确关闭测试，不执行发布和桌面交互。回归仍只在用户明确要求发布时运行。

自有应用目标显式启用 MSVC `/W4` 或 GCC/Clang `-Wall -Wextra -Wpedantic`，现有告警逐步清理，不对 vendor 一刀切 `-Werror`。需要静态分析时可在独立构建目录配置 `-DAINO_STATIC_ANALYSIS=ON -DAINO_BUILD_TESTS=OFF`：MSVC 使用 `/analyze`，其他编译器要求本机安装 clang-tidy。启用选项不会运行回归或安装新工具。

Windows 使用 Ninja 时须确认 `/showIncludes` 本地化前缀被正确识别；如果改了头文件却出现 `no work to do`，不能把旧产物当作本轮编译通过，应使用干净的独立目录重新构建。日常构建和发布构建建议使用不同目录，避免缓存中的测试选项相互影响。


## 发布流程

`CMakeLists.txt` 中的项目版本是版本号来源。仅在明确要求发布新版本时运行回归，普通提交不触发 CI。推送对应 `v*` 标签后，`release.yml` 执行 Windows、Ubuntu/Debian、Fedora/RPM、Arch 四项验证；四项成功后发布 Windows 安装器、deb、rpm、Arch 包、FFmpeg 官方原始源码包、构建信息、许可及 `SHA256SUMS.txt`。各任务使用嵌入解码器，Linux 包还验证安装和 Xvfb 启动。项目版本如 `0.2.9` 对应标签 `v0.2.9`，标签与程序版本不符或缺少内置解码器会中止发布；不会覆盖旧版标签或安装包。

发布配置显式设置 `AINO_RELEASE_PROFILE=windows|linux|rpm|arch`，同时要求 `AINO_BUILD_TESTS=ON` 和 `AINO_BUNDLE_FFMPEG=ON`。配置阶段生成 `release-test-manifest.txt`，必需依赖或测试缺失即失败，不再只靠 `ctest --no-tests=error` 判断集合完整。验收职责如下：

| Profile | 必须注册的验收 |
| --- | --- |
| 全部四项 | UI 生命周期、player 核心回归、包仓库元数据、嵌入解码、浏览器登录 HTTP、无窗口控制器 |
| windows、linux | 另需已安装 Chromium 浏览器及官网交接测试；可用 `AINO_TEST_BROWSER` 指定路径 |
| windows | 另需消息派发期间唤醒检查 `windows_event_wakeup`（原生窗口，不操作用户托盘） |
| linux、rpm、arch | 另需 Xvfb、Linux 窗口及 DPI/字体缓存检查 |

RPM/Arch 不声称执行了完整浏览器交接验收，该项由 Windows/Ubuntu 任务承担。Arch 的普通 `makepkg` 关闭测试；发布任务显式传入 `AINO_RELEASE_VALIDATION=1` 才开启 profile 和 `check()`。不要为了日常编译设置该变量。

`repositories.yml` 在发行完成后汇总现有产物，生成由独立 GPG 密钥签名的 APT、RPM、pacman 软件源并一起部署 Pages，不重新构建播放器或执行音频回归。这种仓库签名不等于 Windows Authenticode；本仓库没有因此获得 Windows 代码签名证书。第三方 Actions 固定提交，容器固定摘要；托管 runner 的安全更新及系统仓库包仍是变化的外部输入。

发布工作流手动执行时可选择单个平台，仅构建、验证并上传产物，不自动发布。可用于只恢复失败的平台；复用另一平台的产物前须确认同一标签、同一提交且其验证任务成功。

本地 Debian 打包：

```bash
python3 tools/build_ffmpeg.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DAINO_BUILD_TESTS=ON -DAINO_RELEASE_PROFILE=linux
cmake --build build --parallel
ctest --test-dir build --output-on-failure --no-tests=error
cpack --config build/CPackConfig.cmake -G DEB -B dist
```

上面的本地 Debian 验收同样只适用于获准发布时，并要求安装 Chrome/Chromium 和 Xvfb。`tools/probe_*.cpp` 是手动诊断工具，不属于自动发布测试，也不是无用源码；部分探针会读本机配置、访问真实音乐服务并下载文件，普通编译检查不要运行它们。
