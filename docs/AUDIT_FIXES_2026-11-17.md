# 工程审查整改与验证记录

日期：2026-11-17。对应 [原始审查](CODEBASE_AUDIT_2026-11-17.md)，基线提交 `6f8f86d134ba56d079c0a0e0ec52536fef64fa12`。

本记录区分“源码已整改”“运行验收已通过”和“设计建议待实施”。本轮完成确定缺陷的代码整改及发布门槛补充，但没有运行回归或真实音源播放，不能据此保证全部平台、脚本、账号和媒体格式均正常。

## 1. 范围和兼容性决策

- 优先保证现有音乐、音源和登录功能可用；没有更换音频引擎、解码器、平台枚举、LX API 或 UI/IME 实现，没有升级产品版本。
- 原生网易云取流把账号 Cookie 留在官方 HTTPS API；CDN 音频请求只携带普通 User-Agent/Referer，允许无凭据的 HTTP 音频和重定向，不强改 CDN 协议。
- 原生 HTTP 请求检查头部语法；携带 Cookie/Authorization 的宿主请求不允许明文或降级。脚本桥保留既有自定义 HTTP/重定向契约，仍拒绝换行、NUL 和非法头名称，不把该兼容分支描述为凭据隔离沙箱。
- 撤下本轮曾尝试、但未经真实音源验证的进程总内存/HTTP 硬配额及 Provider 权限收紧。沿用 Provider 256 MiB、LX 512 MiB 的每运行时限制、LX 每脚本最多 16 项并行 HTTP 和既有 LX 文件上限。没有引入新的进程总量或 Provider 文件数量限制。
- Provider 覆盖目录继续作为本地可信代码；外部 Provider 的 `ctx.cookie` 契约保持不变。权限管理须另行设计，不通过内容哈希静默取消用户修改脚本的登录访问。LX 不注入宿主账号 Cookie。
- 旧下载文件和任务保留，不自动删除、重下载、改名或改写已有音频。历史映射不能证明真实平台，迁移为来源未验证；可手动从本地文件播放，但不再自动冒充当前平台的下载副本。
- 标签写入只修改可安全识别的 MPEG 音频及经典 MP4。其他格式、碎片化 MP4、异常偏移或不支持的元数据结构会明确返回内嵌失败并保留原文件；不把内嵌失败等同于音源或下载失败。

## 2. 确定问题整改清单

以下“已实现”均指工作区源码/配置已改；运行验收统一留待获准发布时执行。

| ID | 状态 | 改动位置与结果 |
| --- | --- | --- |
| H-01 | 已实现，脚本信任例外见上文 | `providers/netease.js::search` 使用 HTTPS；`net::safe_request_headers` 校验原生凭据传输，WinHTTP/libcurl 限制带宿主凭据请求的协议降级；`music::resolve_netease` 分开 API 与 CDN 请求头 |
| H-02 | 已实现 | `tag::shift_chunk_offsets` 只平移指向原 moov 结尾及其后的 stco/co64 值；先确认旧偏移属于 mdat，检查有符号增量及 32/64 位溢出；前置 mdat 保持原偏移 |
| H-03 | 已实现 | `tag::embed` 按内容白名单分流；ID3 后再检查 MPEG 帧头，排除 ADTS、Ogg、WAV 等误入 MP3；未知容器不写入 |
| H-04 | 已实现 | `tag::read_atom` 验证普通/扩展/零长度 atom，循环严格前进；chunk 表限定当前盒边界，递归限制深度；流式扫描和拷贝支持取消 |
| M-01 | 已实现 | `tag::update_moov` 显式构造四字节 meta FullBox 和完整 hdlr/ilst 层级，补充缺失 handler；只替换请求更新的 covr/歌词，不反复追加重复项 |
| M-02 | 已实现 | `tag::embed_mp3` 保留非目标 ID3v2.2/2.3/2.4 帧，处理支持的 unsynchronisation/扩展头；校验 footer；只在提供对应内容时替换封面/歌词；不支持时不改文件 |
| M-03 | 已实现 | `tag::write_replace_stream` 独占创建临时文件，分别检查 writer、flush、close、取消、替换；异常清理只删除本次拥有的临时文件，失败保留原文件 |
| M-04 | 已实现 | `js::parse` 严格模式检查完整文档、数字语法、转义、控制字符、UTF-8 与 surrogate；失败清空部分结果；平台响应显式 lenient 入口保留 |
| M-05 | 已实现 | `js::Value::num/integer/int_value/milliseconds` 检查有限性和目标范围；资料库、发现页、账号/目录字段迁移安全窄化，秒转毫秒避免溢出 |
| M-06 | 已实现 | `lyr::parse_stamp` 检查输入区间、分钟和秒范围及乘加溢出；点号/冒号统一解释十进制小数；超出毫秒精度的位只校验不累乘 |
| M-07 | 已实现身份修复；详细质量指纹留待扩展 | `DownloadedFile::exact`、`DlItem::source_key` 与 `provenance_version=2` 记录当前变体身份；查找只用 chosen 变体，登记/重定位不再扩散到全部平台；保留 downloads.json 主版本 1 |
| M-08 | 已实现 | `cover::Store::get/drain/load` 回收提交和逐项异常状态、worker 计数及图片字节；通知器同步；Windows COM、Linux GObject/stb 用局部 RAII |
| M-09 | 已实现 | `config::load/save` 检查完整、有限的音量数值并限定 0～100，非法值保留合法默认/先前值；质量、快捷键、覆盖窗等数值改为有范围转换 |
| M-10 | 已实现发布门槛及部分新增用例，待运行验收 | CMake 显式校验四种 release profile 的必需测试，生成 manifest；缺 Python/浏览器/Xvfb 不再静默缩减集合；新增离线安全样本及控制器输入/来源用例；完整故障注入仍需补充 |
| L-01 | 已实现 | `music::detail::merge_into` 对已确认同曲的同平台更高码率变体就地替换，不再追加重复平台项；未改变同曲判定规则 |
| L-02 | 已实现入口 | 自有应用目标启用 `/W4` 或 `-Wall -Wextra -Wpedantic`，显式 `AINO_STATIC_ANALYSIS` 开关；保留上游代码，不全仓格式化、不启用全局 Werror；已有告警仍待分类清理 |
| L-03 | 已实现 | `docs/BUILDING.md` 对齐 Windows/deb/rpm/Arch 四任务、测试职责、软件源签名和日常不回归的规则 |

## 3. 条件性风险处理

| ID | 本轮处理 | 仍未完成或未证明的部分 |
| --- | --- | --- |
| R-01 登录清理阻塞 | `login::begin_work` 在 running 清除前拒绝新任务；done 可提前交付凭据，UI 不同步 join 尚在清理浏览器的任务；线程创建失败恢复状态 | 未运行真实浏览器生命周期、超时或退出交错验证 |
| R-02 Provider/本地凭据 | 明确 Provider 为可信代码边界，修默认 HTTPS 请求和 CDN Cookie 转发，保留原登录访问以避免音源兼容变化 | Provider 最小权限、用户显式授权、系统凭据库存储尚未实施；本地 Cookie 仍为明文，不声称已加密 |
| R-03 脚本总资源 | 保持原有每脚本限制及现有 runtime_bytes/collect_idle 机制，不重复引入运行时框架 | 未量化全进程峰值；新的全局预算、懒加载和淘汰策略暂缓，不能把条件性风险标为已消除 |
| R-04 断电持久性 | `platform::flush_file` 提交 stdio 和 OS 写缓存；关键配置/资料库/下载映射/音源统计检查 flush 与 close；Linux rename 后同步目录并报告降级 | 未做断电或文件系统故障注入；目录 fsync 失败时 rename 已生效，报告持久性不足，不能伪称安全回滚 |
| R-05 供应链 | Actions 固定提交、容器固定摘要、runner 固定 OS 系列；Inno 默认校验官方固定包和缓存二进制，显式开发覆盖才用系统 ISCC | 托管 runner、系统包仓库仍变化；没有 Windows Authenticode 证书；未执行供应链/CVE 扫描或本轮打包 |

## 4. 性能、冗余与重构

| 已落地项目 | 原理与预期收益 | 代价与验证边界 |
| --- | --- | --- |
| 流式标签改写 | 只保留有界 moov/ID3 元数据，64 KiB 分块拷贝音频，避免整首文件及插入副本同时驻留；空间由随音频长度增长改为随元数据上限增长 | 仍需同目录临时磁盘和全文件一次 IO；flush 有延迟；大文件 RSS、取消、哈希与实解码未实测 |
| 歌词定位 upper_bound | 从逐帧线性遍历改为二分，保持稳定排序下相同时间戳取末行、跳过未同步文本的语义 | 普通歌词行数少，收益可能很小；未测帧时间，不报告提升比例 |
| 元数据替换 | MP4 请求字段替换而非不断附加，避免重复写入后的文件增长 | 遇到不支持的重复/非经典结构安全拒绝，暂不做通用容器修复 |
| 公共编码/解析复用 | `library::quote`、`lx::detail::js_quote` 转到 `js::quote`；Cookie 获取共用 `net::cookie_value`；B 站 MD5 共用 LX 实现并保留调用包装 | 保留 Provider 有不同净化语义的 quote，不改变其行为；新增已知答案用例尚未执行 |
| 删除已证实冗余 | 删除未使用的宽路径整文件读取器、第二份 MD5、Windows 已不可达的 MinGW 播放器分支、关闭时重复 reset；共享快照替换已废弃的 unique() 调用 | Windows 播放器仍用 MSVC；FFmpeg 仍可用 GCC。没有删除 vendor、活动依赖、人工探针或示例能力 |

架构继续使用现有少编译单元、头文件业务、ViewModel 和后台任务模型。没有引入数据库、通用插件框架或新的运行依赖。打包元数据与 Python 工具之间的重复组织方式未顺带重构。

## 5. 本轮检查证据

- 在 `build/audit-reviewed` 的全新目录执行 Windows Release 配置和 `aino_player` 编译，`AINO_BUILD_TESTS=OFF`、profile 为空，使用已有嵌入 FFmpeg，编译和链接成功。
- 旧 `build/audit-fixes` 的增量检查曾错误显示无工作；发现本机 MSVC 本地化 include 前缀未被依赖扫描正确识别后，改用上面的全新目录。没有把旧产物当作本次通过。
- 对 `tests/player_tests.cpp` 与 `tests/controller_tests.cpp` 执行 MSVC `/Zs` 语法编译成功。新增 `tests/audit_safety.hpp` 随 player_tests 一起检查；没有启动测试二进制。
- 单独配置 Windows release profile，成功生成恰含七个必需项的 `release-test-manifest.txt`；只做 CMake 配置，没有构建或运行该目录中的测试。Python 工具通过 AST 语法检查，Arch PKGBUILD 通过 `bash -n`，外部 Action 引用均为完整提交哈希，`git diff --check` 通过。
- 对变更做静态代码复核、差异及语法检查。编译仍有上游/既有 CRT 废弃、遮蔽和转换告警，不声称 warning-free 或已执行 `/analyze`。
- 遵循 AGENTS.md，未运行 CTest、音频格式测试、浏览器 E2E、压力测试、性能测试、整套 CI 或人工音乐探针；未用真实账号验证播放。
- 未提交、推送、触发工作流、安装/替换现用播放器、改 IME 或发布版本。构建脚本与日志留在忽略的 build 目录。

静态复核另外纠正了两处整改方案自身的风险：普通 HTTPS 音频不能一律禁降级，限制仅用于携带宿主凭据的原生请求；Arch 发布环境变量要通过 `sudo -u builder env ...` 显式传入，否则 makepkg 可能丢失发布开关。Linux 编译、工作流执行和安装器制作均未在本轮验证。

## 6. 后续验收与发展方案

1. 获准发布时先执行 Windows/Ubuntu 必需验收，再完成 RPM/Arch 安装及其 profile；核对 manifest，补齐写失败/close 失败、线程提交异常、清缓存交错、旧下载迁移等故障注入。
2. 由用户使用实际音源验证未登录/登录、标准/高音质、智能/固定脚本、HTTP CDN、下一曲预加载和下载。初始化成功、模拟 HTTP 或编译成功都不能代替这些结果。
3. 在隔离样本上验证 MP3 各 ID3 版本/unsynchronisation、MP4 两种 moov 顺序和 stco/co64，检查无关元数据、音频 payload、解码与 seek；不直接拿唯一一份音乐文件做故障实验。
4. 先记录脚本加载/占用、锁等待、真实启动及取消耗时，再制定可回退的总预算；Provider 权限和系统密钥库存储需带迁移方案，优先级高于新增格式/API。
5. 原报告的脱敏诊断包、离线质量/容器/内容指纹、ReplayGain/专辑模式、按用户脚本需求补 LX 能力可分别立项；原生 Wayland、可访问性及输出独占为中长期独立设计。它们是产品与架构发展建议，本轮没有为“全部修复”自动启用未经确认的新行为。

验收状态：**代码整改完成到可编译、可审查；动态回归、真实音源兼容性及上表注明的后续能力尚未完成。**

## 7. 用户授权后的 v0.5.2 发布验收

用户随后明确要求发布新版本，故开始执行发布回归。第 5 节与上一行状态保留为发布授权前的历史记录，不能当作本阶段仍未运行测试。

- 修复提交：`36994b3`。正式播放器版本更新为 0.5.2，内置 UI 版本仍为 0.5.2，正式构建的本地修订号为 0。
- 本机独立目录 `build/release-v052` 完整编译通过，Windows release profile 的 CTest **7/7 通过，60.86 秒**。涵盖界面生命周期、核心回归、新增标签/解析/来源用例、软件源元数据、十种嵌入音频格式、登录 HTTP、真实 Chromium 的隔离假 Cookie 交接及无窗口控制器/封面队列。
- 文件关联元数据与 `git diff --check` 通过；EXE 报告 `aino player 0.5.2`，解码器报告内置 FFmpeg 8.1.3。没有安装或替换现用播放器。
- 本机自动回归不等于真实账号及所有 LX 音源实播；没有将输入法或任务栏歌词的编译/模拟结果写成用户实机确认。
- 四平台正式产物、安装验收和软件源同步由 v0.5.2 标签工作流完成；其最终结果以 GitHub Actions 和后续发布记录为准。

最终发布结果：版本提交 `b7dfcf0`，标签 [v0.5.2](https://github.com/sudoevolve/aino-player/releases/tag/v0.5.2)，正式发布时间为 2026-11-17 15:10:15（Asia/Shanghai），非草稿、非预发布，并已设为最新稳定版。[发布工作流 44035916203](https://github.com/sudoevolve/aino-player/actions/runs/44035916203) 的 Windows、Ubuntu、Fedora、Arch 和 publish 全部成功：Windows **7/7，48.93 秒**；Ubuntu **9/9，23.62 秒**；Fedora **8/8，17.48 秒**；Arch **8/8，17.56 秒**。Linux 包安装与窗口启动检查通过，Ubuntu 还验证由上一正式版覆盖升级。

本机下载全部 10 个发布附件，逐一核对实际文件大小和 SHA-256 与 GitHub asset digest 一致；9 项载荷另外与 `SHA256SUMS.txt` 一致，校验清单自身也与 GitHub 摘要一致。Windows 安装器 4,750,948 字节、便携 EXE 8,468,480 字节、deb 3,754,766 字节、rpm 3,201,925 字节、Arch 包 3,839,671 字节；FFmpeg 源码、两份构建信息与许可同时发布。

[软件源工作流 44036629590](https://github.com/sudoevolve/aino-player/actions/runs/44036629590) 的 APT、RPM、Arch 和 Pages 部署全部成功。重新读取公开 APT Packages、RPM primary 元数据及 Arch 数据库，确认分别包含 0.5.2、0.5.2、0.5.2-1。没有替换用户正在使用的安装，没有操作真实音乐账号或把所有第三方音源宣称为逐一实播通过；R-02/R-03 等已明确暂缓的能力仍保持原记录。
