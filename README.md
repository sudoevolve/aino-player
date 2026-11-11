# aino player

用 [aino](https://github.com/sudoevolve/aino)（单头文件 C++17 立即模式 UI 库）做界面，
网易云 / QQ 音乐 / 酷我 三源并发检索与聚合，播放走 Windows 自带的 Media Foundation（IMFMediaEngine）。

一个 exe（约 950 KB），静态链接 CRT 和 WebView2 的 loader，不装运行库；
除了 WebView2 的头文件和静态库（只用于登录）没有任何第三方依赖，连 JSON 都是自己写的。

## 功能

| 页面 | 内容 |
|---|---|
| 搜索 | 三源并发检索，同一首自动合并成一行；右侧药丸切换取流源；hover 出现「＋」（加入队列）与下载 |
| 发现 | 网易云 / QQ / 酷我三家的**热搜**（点词直接搜）+ **热门歌单**（点开直接载入列表，网易云含飙升 / 新歌 / 原创三个官方榜） |
| 正在播放 | 队列：点播、移除、清空、列表循环开关；队列跨重启保留 |
| 歌单 | 网易云 / QQ 账号的自建歌单；点开即播；「同步到此」把当前列表写回歌单 |
| 音源 | 每源开关、登录 Cookie（可选）、上次检索的耗时与条数 |
| 设置 | 两个账号登录、深色模式、默认音质（标准 / 高 / 无损）、默认音量、下载目录、内嵌封面 / 内嵌歌词 |

其他：Apple Music 式底栏、右侧歌词抽屉（三源歌词，居中、hover 高亮、点行跳转）、
深色模式、下载完整音频（m4a / mp3，扩展名按真实容器定）、内嵌 ID3v2.3 / ilst 标签。

## 结构

| 层 | 实现 | 文件 |
|---|---|---|
| 界面 | aino：六页侧栏、虚拟滚动列表、紧凑底栏、歌词抽屉、深色模式 | `src/main.cpp`、`src/aino.hpp` |
| 发现页 | 三源热搜 + 热门歌单 + 官方榜 | `src/discover.hpp` |
| 多音源 | 三源并发 + 同曲合并 + 相关性排序 + 跨源换源 | `src/sources.hpp` |
| 歌词 | 三源歌词 + LRC 解析 + 逐行跟随 / 点击跳转 | `src/lyrics.hpp` |
| 账号 | WebView2 内嵌登录（QQ / 网易云共用一套流程） | `src/login.h`、`src/login.cpp` |
| 歌单 | 网易云歌单读写、QQ 歌单读写 | `src/netease_api.hpp`、`src/qqapi.hpp` |
| 播放 | Media Foundation `IMFMediaEngine` | `src/audio.hpp` |
| 封面 | WIC 解码 + 内存缓存 + 自加的 `draw_bitmap` | `src/cover.hpp` |
| 标签 | ID3v2.3（APIC / USLT）与 MP4 `ilst`（covr / ©lyr，含 stco 平移） | `src/tag.hpp` |
| 基础 | 手写 JSON 解析（能吃酷我单引号 JSON）、WinHTTP 封装 | `src/json.hpp`、`src/net.hpp` |

## 多音源聚合的几个设计

- **并发扇出**：每个源一个线程，单源 HTTP 超时 8s、聚合层 9s 软超时；
  慢的源不拖垮整次搜索，失败降级成 `SourceReport`（界面上那行红字）。
- **跨源同曲合并**：指纹 = 归一化标题 | 首位歌手 | 3 秒时长桶；
  合并时再校验「时长差 ≤ 4s 且歌手集合有交集」。标题归一化会去括号后缀、去标点、折叠空白。
- **相关性排序**：标题全等 100 / 前缀 70 / 包含 45，加"多源命中"与"有可播源"加权，
  翻唱 / 伴奏 / 现场降权；同分时长者优先。
- **跨源换源**：当前源取流失败，先在同曲的其它源里换，再按「歌名 + 主歌手」去没命中过的源补齐；
  补到的结果会写回列表（药丸上出现，下次直接命中）。
- **音质真相**：播完落地后用 mp3 帧头算真实码率与采样率，不拿平台标称糊弄。

### 一个实测踩到的坑：酷我的"试听片段"

酷我对受限曲目**不报错**：HTTP 200、`audio/mpeg`、没有任何标记，只是文件只有 181KB
（11 秒）。只看状态码会以为取流成功，播到第 11 秒就没声了。

处理：取流后先用 `Range: bytes=0-1` 探体积，和「时长 × 128kbps」的预期值比，
小于 60% 判为片段 → 换下一个源；三个源都只有片段时，放最长的那段并在音质栏写明
「试听片段」。这也是**默认关闭酷我**的原因（默认只开网易云 + QQ，可在「音源」页手动开）。

## 三源歌词

| 源 | 接口 | 坑 |
|---|---|---|
| 网易云 | `/api/song/lyric?id=&lv=-1&tv=-1` | 匿名可用 |
| QQ | `fcg_query_lyric_new.fcg?songmid=&format=json&nobase64=1` | Referer 必须是 `y.qq.com/portal/player.html`；`nobase64=0` 是 base64 |
| 酷我 | `m.kuwo.cn/newh5/singles/songinfoandlrc?musicId=&httpsStatus=1` | **必须 https 且带 `httpsStatus=1`**，走 http 回 301「音乐查询失败」 |

取词顺序：当前选中源 → 其它源 → 网易云按名反查。面板底部写明「歌词来自 X」。

反查有个坑：搜「花海 周杰伦」前两条是 70 秒的翻唱（「治愈版」「抒情版」），
如果只按归一化标题比，翻唱和原曲同分、先到的赢。判据是**原始标题完全相等 +60**、
归一化相等只给 +8、时长差超过 15 秒直接排除。

歌词抽屉：当前行高亮、居中、hover 有底色，点任意一行跳到那句，上下边缘用十条 alpha 渐变柔化。

## 登录与歌单

登录用**内嵌 WebView2 窗口**（`src/login.cpp`），不去读浏览器的 cookie：

- Chrome 127+ / Edge 用 App-Bound Encryption，cookie 值用应用绑定的密钥加密，
  外部进程拿不到主密钥，读出来也是乱码；
- WebView2 有自己的 user data 目录（QQ 与网易云各一套），扫一次码之后 cookie 就存在那里，
  **下次启动直接是登录态**。

成功判据：QQ 看到 `qm_keyst`、网易云看到 `MUSIC_U`。拿到 cookie 自动关窗并拉歌单。

接口上踩到的几处：

- **QQ 歌单列表** `fcg_user_created_diss`：字段是 `diss_name / diss_cover / song_cnt / dirid`，
  老教程写的 `dissname / logo / songnum / dissid` 也兼容着读。
- **QQ 歌单详情** `music.srfDissInfo.aiDissInfo` 的参数分两种：自建歌单 `disstid=0` + `dirid=<编号>`，
  广场歌单（热门 / 别人分享）反过来 `disstid=<编号>` + 不传 `dirid`。传错一律 `-100008`。
- **QQ 取流** `vkey.GetVkeyServer.CgiGetVkey`：`comm` 里必须同时带 `uin` / `qq` / `authst`
  （`authst` 取 cookie 的 `qm_keyst`），只带 Cookie 头会回 200 但 `purl` 全空；
  文件名拼法是 `<档位前缀><songmid><media_mid>.<ext>`，`media_mid ≠ songmid` 时要先查一次歌曲详情。
- **网易云热搜** 是 `/api/search/hot?type=1111`（`hot/detail` 已经 404）；
  **QQ 热搜** 是 `gethotkey.fcg`；**酷我热搜** 是 `hotword.kuwo.cn` 返回的纯文本。
- **酷我没有公开的歌单广场接口**：web 端要 `kw_token` + `csrf` 双校验，匿名一律
  `The request is illegal!`，所以发现页酷我那一栏只有热搜。

## 封面从哪来

| 来源 | 方式 | 请求数 |
|---|---|---|
| QQ | 搜索结果里就有 `album.pmid`，拼 `y.gtimg.cn/music/photo_new/T002R300x300M000{pmid}.jpg` | 0 |
| 网易云 | `api/song/detail?ids=[...]` 一次批量拿所有 `al.picUrl` | 1（整页共用） |
| 酷我 | `artistpicserver.kuwo.cn/pic.web` 返回**纯文本 URL**（不是 302），再取一次图 | 2/首，限前 20 首 |
| 兜底 | 按歌名哈希生成渐变 + 首字 | 0 |

下载与解码都在后台线程（`cover::Store`），解码阶段用 WIC 的 `IWICBitmapScaler` 缩到 192px
长边，之后每帧只是双线性缩放到 44/56px 上屏；图没到货先显示渐变，到货自动替换。

## 构建

```bash
./build.sh          # Git Bash / MSYS
```

或者双击 `build.bat`（自动找 VS 工具集与 Windows SDK，自己拼 INCLUDE/LIB，
不调用 vcvars）。产物 `build/aino_player.exe`，`/MT` 静态链接，目标机不用装运行库。

要求：**MSVC 19.29+**（`/utf-8` 必须加，否则中文字面量按 GBK 编）与 Windows SDK 10；系统 Win8.1+。

链接的系统库：`user32 / gdi32 / winhttp / winmm / shell32 / ole32 / oleaut32 /
windowscodecs / advapi32 / mfplat / mf / mfuuid`。

`vendor/webview2/` 里是 WebView2 SDK 的头文件和 x64 静态 loader（NuGet 包
`Microsoft.Web.WebView2` 解出来的，许可证见同目录 `LICENSE-WebView2.txt`），只为登录用。
目标机需要 **Edge WebView2 Runtime**：Windows 11 自带，Win10 多数随 Edge 装好了；
没装的话设置页会提示，其余功能照常。

`/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup` 是必须的，否则会挂一个黑色控制台窗口。

## 用法

- 启动恢复上次的队列；队列为空时检索 `周杰伦`。也可以带词启动：`aino_player.exe "关键词"`
- 一行 = 一首歌。点行播放；点右侧药丸切源；hover 右侧「＋」加入队列、「↓」下载
- 「发现」页：点热搜词直接搜，点歌单卡片直接载入（能播 / 能下 / 有歌词）
- 「正在播放」页：队列管理，列表循环可关；队列存在 `%APPDATA%\aino-player\queue.txt`
- 「歌单」页：网易云 / QQ 分段切换，点歌单在主列表打开，右侧「同步到此」写回账号
- 底栏：上一首 / 播放暂停 / 下一首，进度条（点哪跳哪、按住可拖），歌词抽屉、下载、音量
- 配置存 `%APPDATA%\aino-player\config.ini`，缓存落在 `%TEMP%\aino-player\`

## 实测数据（2026-11-11，本机 18 核，150% DPI）

| 项 | 数值 |
|---|---|
| 双源并发检索 | 190–480 ms（网易云 ~150ms / QQ ~470ms） |
| 合并结果 | "周杰伦" 12 条/源 → 21–34 首 |
| 空闲 CPU | **0.00%**（主循环阻塞在 `WaitMessage`） |
| 播放中 CPU | 0.4–0.6%（整机占比） |
| 内存 | ~40 MB |
| 帧耗时 | 10–16 ms（1180×760 逻辑 @150%） |

各源可用性（匿名、无 Cookie）：

| 源 | 搜索 | 取流 | 备注 |
|---|---|---|---|
| 网易云 | 可用 | 免费档直出 128k mp3 | 会员曲（fee=1/4）跳到 404 → 报「需要登录 / 会员」 |
| QQ | 常用 `code 2001` 拦匿名 | 必须 `qm_keyst`，否则 `104009` | 登录后走 `vkey.GetVkeyServer` |
| 酷我 | 可用 | 直出 mp3，支持 Range | 受限曲目静默给 11s 片段（默认关闭） |

## 已知限制（不粉饰）

- **不做边下边播**。MediaEngine 播的是本地文件，所以有「缓冲 xx%」这一段
  （3–5 MB 通常 1–2 秒）。宁可老实缓冲，也不做假流式。
- **只有 Windows**。取流用 WinHTTP、播放用 Media Foundation、登录用 WebView2，都是系统组件。
- **音质取决于账号**。网易云要 Cookie（无损还要会员），QQ 按文件名档位递档，
  酷我的直链跟资源走、没有档位参数（选了也忽略）。
- **歌单写入没公开文档**。失败时把服务端 code 原样弹出来，不做假成功。
- **酷我没有歌单广场接口**，发现页那一栏只有热搜。
- 封面只缓存在内存里（最多 160 张），重启要重新取。
- 接口是"公开但不承诺稳定"的 web 接口，平台一改就得改适配器 —— 所以每个源是独立的
  `search_*` / `resolve_*` 一对函数，改一个不影响聚合层和界面。

## 开发用的取证脚本

`tools/` 里这几个是给"肉眼看不出来"的问题准备的，不是运行时依赖：

| 脚本 | 用途 |
|---|---|
| `probe_sources.cpp` | 不开界面的源层探针：打印每源耗时、合并结果、每首每个源的取流结论 |
| `probe_tag.cpp` | 单独验证 mp3 / m4a 内嵌标签的字节结构 |
| `grab_window.py` | 抓指定标题窗口存 BMP（纯 ctypes + GDI，不需要 PIL） |
| `bmp2png.py` | BMP → PNG（标准库 zlib 手写编码器） |
| `click_at.py` / `move_to.py` | 按**逻辑坐标**点窗口 / 悬停（客户区 lParam，自动换算 DPI） |
| `run_bat.py` | 用干净环境跑 .bat 并回收输出 |

```bash
./build.sh probe && ./build/probe_sources.exe "周杰伦"
```

## 对 aino 的改动

上游 aino 没有位图绘制能力（`image_well` 只是占位），播放器封面必须有，所以给
`src/aino.hpp` 加了一个方法，其余一字未改：

```cpp
// 本地扩展，位置在 fill_circle 声明之后
void Ui::draw_bitmap(const Rect& box, const uint8_t* rgba, int w, int h, float radius = 0.0f);
```

实现是逐像素双线性采样 + 圆角遮罩，走 `Canvas::blend`（自带裁剪与 DPI 换算），
120×120 物理像素约 0.35 ms。升级 aino 时注意保留这一段。

## 许可

Apache-2.0（与 aino 一致）。`src/aino.hpp` 版权归 sudoevolve，见 `LICENSE-aino`。
本项目是独立的 C++ 实现，未复制任何第三方项目代码。

## 致谢

- [aino](https://github.com/sudoevolve/aino) —— 整个界面都靠它。
- [Melodex-Music](https://github.com/LunaShiori/Melodex-Music) —— 多音源分层（每个源一个适配器、
  聚合层做并发与合并）的思路参考了它，代码是 C++ 重写，未复制。
