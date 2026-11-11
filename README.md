# aino player

用 [aino](https://github.com/sudoevolve/aino) 做的桌面播放器：网易云 / QQ 音乐 / 酷我 三源并发检索与聚合，
播放走 Windows 自带的 Media Foundation。一个 exe，静态链接，不装运行库。

## 功能

- **搜索**：三源并发，同一首自动合并成一行；药丸切源，hover 出「＋」加队列、「↓」下载
- **发现**：三家热搜（点词直接搜）+ 热门歌单（网易云含飙升 / 新歌 / 原创榜）
- **正在播放**：队列管理（点播 / 移除 / 清空 / 列表循环），跨重启保留
- **歌单**：网易云 / QQ 的自建歌单，点开即播，「同步到此」写回账号
- **歌词**：三源歌词，居中跟随、hover 高亮、点行跳转
- **音质**：标准 / 高 / 无损；下载可内嵌封面与歌词（ID3v2.3 / MP4 ilst）
- 深色模式、WebView2 内嵌登录（不去读浏览器 cookie）

## 构建

```bash
./build.sh          # Git Bash；或双击 build.bat
```

MSVC 19.29+ 与 Windows SDK 10（`/utf-8` 必须加）。产物 `build/aino_player.exe`。

登录需要 **Edge WebView2 Runtime**（Win11 自带，Win10 多数随 Edge 装好）；没装只有登录不可用。

## 用法

```bash
aino_player.exe              # 恢复上次队列
aino_player.exe "关键词"      # 带词启动
```

配置在 `%APPDATA%\aino-player\config.ini`。
酷我默认关闭（受限曲目只给 11 秒试听），可在「音源」页手动开。

## 已知限制

- 只有 Windows（WinHTTP + Media Foundation + WebView2）
- 不做边下边播：播的是本地缓存文件，起播有 1–2 秒缓冲
- 音质取决于账号：网易云无损要会员，酷我的直链跟资源走、档位无效
- 歌单写入接口无公开文档，失败时原样回传服务端 code
- 酷我没有公开的歌单广场接口，发现页那一栏只有热搜
- 接口是「公开但不承诺稳定」的 web 接口，平台一改就得改对应适配器

## 许可

Apache-2.0。`src/aino.hpp` 版权归 sudoevolve，见 `LICENSE-aino`。

## 致谢

- [aino](https://github.com/sudoevolve/aino) —— 整个界面都靠它
- [Melodex-Music](https://github.com/LunaShiori/Melodex-Music) —— 多音源分层（每源一个适配器、聚合层做并发与合并）的思路参考了它，代码为 C++ 重写，未复制
