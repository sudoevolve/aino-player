# providers/ —— 各平台接口（JS，可热改）

这些平台的**自带接口**（搜索 / 歌单 / 取流）变得比客户端发版快得多，所以不写在 C++ 里：
**改一个 `.js`、重启应用就生效，不用重新编译。**

> 这里只放「自带接口」——需要登录态、负责搜索与歌单的那一侧。
> 第三方音源脚本（落雪/六音那类，免登录、只管取流）是另一套东西，放在 `sources/` 目录，
> 契约完全不同（`lx.on('request', ...)` + `musicUrl`），别混在一起。

目录位置：`<exe 所在目录>/providers/*.js`，或用环境变量 `AINO_PROVIDER_DIR` 指定。

## 契约

一个文件对应一个平台，向 `globalThis.provider` 挂一个对象：

```js
globalThis.provider = {
  key: 'netease',      // 和 C++ 侧 Source 对应：netease / qq（B 站走内置实现）
  name: '网易云',       // 界面上显示的名字

  // 可选。返回歌曲数组；抛异常表示这次失败（上层会显示红字）
  search: function (keyword, page, limit) { return [song, ...] },

  // 可选。不给就退回 C++ 内置实现
  resolve: function (song, quality) { return 'http://...' }
}
```

`song` 对象：

| 字段                           | 说明                                  |
| ------------------------------ | ------------------------------------- |
| `id`                         | 平台内唯一 ID（酷我 rid / 酷狗 hash） |
| `title` `artist` `album` | 显示用                                |
| `duration_ms`                | 毫秒，用于跨源合并同曲                |
| `bitrate`                    | 标称码率，0 表示未知                  |
| `native_id`                  | 需要额外 ID 的平台（如酷狗 sqhash）   |
| `cover`                      | 封面直链，可留空                      |

## 宿主注入的 `ctx`

```js
ctx.http(url, opts)          // 同步 HTTP：{ statusCode, body, bodyText, headers }
                             // body 能解析成 JSON 就给对象，否则给字符串
ctx.lxUrl(key, song, quality) // 让落雪音源取直链，返回 { url } 或 { error }
ctx.cookie('netease' | 'qq')   // 宿主注入的登录 Cookie（provider 读不到应用设置）
ctx.md5(s) / ctx.b64encode(s) / ctx.b64decode(s) / ctx.log(...)
```

`opts`：`{ method, headers: {...}, body, timeout }`。

## 现成的两个

| 文件           | 搜索                                          | 取流                              |
| -------------- | --------------------------------------------- | --------------------------------- |
| `netease.js` | music.163.com 只读接口 + 登录 Cookie          | `ctx.lxUrl('wy', ...)` 交给音源 |
| `qq.js`      | musicu.fcg（需登录 Cookie，匿名会被 2001 拒） | `ctx.lxUrl('tx', ...)` 交给音源 |

B 站仍在内置实现里：它依赖 Cookie 登录态、WBI 签名、分 P 会话，
迁移收益不如风险大，等接口真变了再说。

## 加一个新平台

1. 复制 `kugou.js` 改名，改 `key` / `name` / 接口地址；
2. 如果 C++ 侧还没有对应的 `Source` 枚举值，需要加一个（这一步仍要改代码）；
3. 取流优先 `ctx.lxUrl(...)`，把平台细节留在 JS 里。

## 排查

- 接口挂了：先 `curl` 一下脚本里的 URL，确认是接口变了还是被墙。
- 想让脚本的 `console.log` 打出来：日志走 stderr（`[lx] ...`）。
- 自检：`aino_player.exe --prov-test <providers 目录> 52144 5`
  —— 拉抖音热歌榜前 5 首，并解析第一条直链。
