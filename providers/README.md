# 平台目录与第三方音源

平台负责找歌，第三方音源负责将平台歌曲 ID 换成播放地址，两个入口分别管理。

| 平台 | 搜索 / 发现 | 播放 | 歌手 / 专辑 |
| --- | --- | --- | --- |
| 网易云、QQ | `netease.js` / `qq.js`，热搜、热门歌单 | 自带账号接口或 LX 的 `wy` / `tx` | 已接入 C++ 目录 |
| 酷我、咪咕 | 对应平台 JS，热搜、推荐歌单、官方榜单 | 需要声明支持 `kw` / `mg` 的 LX 音源 | 暂未接入 |
| 酷狗 | `kugou.js`，热搜、推荐歌单、官方榜单 | 需要声明支持 `kg` 的 LX 音源 | 暂未接入 |
| B 站 | C++ 搜索 | 自带音轨、分 P 接口 | UP 主、视频分 P |

搜索只有 JS 一份实现。搜索页、歌词匹配和自动换源复用它，接口报错时直接显示原因，不再暗中重复请求另一份 C++ 搜索。推荐歌单与榜单可打开、播放，也可保存曲目快照到本地歌单并刷新；新平台暂不支持任意歌单链接导入，当前打开最多取 200 首。

QQ 热搜使用官方 `GetHotkeyForQQMusicPC`，主入口失败后限时尝试备用官方入口。热搜先于歌单发布到界面，切换平台会取消旧请求；刷新失败保留旧热词并显示状态。简短错误与完整详情分开显示，详情可滚动、复制，不把 JS 调用栈直接画进热搜区域。酷狗歌单从移动端推荐目录读取，曲目走官方 HTTPS 网关的歌单接口，`playlist:` 与 `rank:` ID 独立保存，支持本地保存及远端刷新。

默认平台 JS 在构建时内嵌到播放器；无需随 EXE 手动复制文件。Windows 可用 EXE 同级 `providers/` 覆盖，Linux 先查可执行文件同级目录，再查 `/usr/share/aino-player/providers`；`AINO_PROVIDER_DIR` 可指定覆盖目录。编辑后重启生效，覆盖脚本出错不会默默改用旧接口。

## 平台脚本契约

```js
globalThis.provider = {
  key: 'netease',
  name: '网易云',
  search(keyword, page, limit) { return [/* song */] }, // page 从 0 开始
  heats() { return [{text: '热搜', heat: 0}] },
  playlists({limit}) { return [{id: '歌单或 rank:榜单ID', name: '标题', cover: '', official: false}] },
  board(id, page, limit) { return [/* song */] } // rank:榜单ID / playlist:歌单ID，page 从 0 开始
}
```

`song` 包含 `id`、`title`、`artist`、`album`、`duration_ms`、`bitrate`、`native_id`、`cover`，还可提供 `needs_login`、`artists: [{id,name}]`、`album_id`。酷狗另存 `album_audio_id` 和 `quality_hashes`，咪咕用 `native_id` 保存 `copyrightId`，这些字段随收藏、队列和歌单持久化。

宿主提供 `ctx.http(url, {method,headers,body,timeout})`、`ctx.cookie('netease'|'qq')`、`ctx.md5`、`ctx.b64encode`、`ctx.b64decode`、`ctx.log`。HTTP 返回 `{statusCode,body,bodyText,headers,error}`；目录方法使用同步 HTTP。可选 `resolve(song, quality)` 仅供平台自带直链，不得反向调用第三方源冒充自带渠道。

集合锁只用于取得 `shared_ptr` 或交换列表；各平台独立运行时锁覆盖参数创建、JS 执行和释放。等待可取消，网络期间不持集合锁；调用线程持有自己的凭据快照。重载不释放仍被请求持有的旧运行时。

## 第三方音源

导入、获取或重载后，后台分别对每个脚本的 QQ、网易云样本请求播放地址，最多两项并行，再用 Range 探测音频头。不会播放声音或下载整首；检测优先让位于点播。界面区分待检测、检测中、样本通过、样本失败、不支持及超时未完成，通过的排前面；取流按当前平台的检测结果排序，用户明确指定的源仍优先。可点击“重新检测”。结果仅保留在本次运行，不代表所有歌曲、所有音质可播。各平台的近期取流错误单独保留在详情中，不因单首歌失败或另一个平台样本失败把已通过的源整行标红。

“移除”只记录应用内禁用，不删除 JS。重新加载尊重移除记录；主动“在线获取”会恢复并加载本次成功下载的脚本，汇总下载与初始化失败数量；查看未加载列表也可逐个恢复或重新导入。Windows 的音源目录默认在 EXE 同级 `sources/`，Linux 在用户配置目录的 `sources/`，两者都支持 `AINO_LX_DIR`。运行错误保留具体原因，不把“初始化成功”称为“可播放”。

LX 宿主契约对照 lx-music-desktop `323e4bac505da428adae8db6b58039fb2b36b86d`：API 版本为 `2.0.0`，`send/on` 返回 Promise，初始化会完成监听器注册。取流保留初始化任务，只清理本次请求的定时器和网络任务；HTTP 独立并行、可取消，每个脚本最多 16 项，JS 回调仍只在运行时线程执行。响应包含 raw 字节和响应头，MD5 / Base64 按字节处理 Buffer，currentScriptInfo 包含 rawScript。

尚未实现 AES / RSA、zlib、multipart formData、脚本的 `lyric` / `pic` 消费及完整 Node API，不能宣称所有脚本兼容；DNS 失败、服务端下线和账号权限也会影响可用性。酷狗榜单通过官方 HTTPS 网关转发，避免旧域名证书不匹配。酷我缺失封面在封面后台队列按需解析，不阻塞榜单读取。各平台播放仍需实际网络与账号确认。
