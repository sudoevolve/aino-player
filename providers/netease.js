// 网易云 music provider —— 只做搜索。
// 搜索走 music.163.com 的只读接口。
//
// 取流不在这里实现，交给 C++ 的 resolve_netease：它按数字 song id 精确取，不会串歌。
// 曾经这里用 ctx.lxUrl('wy', ...) 转手问音源，音源按歌名匹配会命中同名翻唱，
// 而链路被标成 builtin 显示成"自带"，等于把翻唱冒充成平台原曲。
// 规则：自带渠道只用自己的接口；音源渠道（Lane::JsSource）才允许问脚本。

function http(url, opts) {
  const r = ctx.http(url, opts || {})
  if (!r || r.error) return { error: (r && r.error) || 'network' }
  return { status: r.statusCode || r.status, body: r.body, text: r.bodyText }
}

function cookieHeader() {
  const c = ctx.cookie('netease') || ''
  let h = 'appver=2.0.2; os=pc'
  if (c) h += '; ' + c
  return h
}

function songFrom(item) {
  const id = String(item.id || '')
  const name = item.name || ''
  if (!id || !name) return null
  const artists = item.artists || item.ar || []
  let artist = ''
  for (let i = 0; i < artists.length; i++) {
    if (i) artist += '/'
    artist += artists[i].name || ''
  }
  const album = (item.album && item.album.name) || (item.al && item.al.name) || ''
  const fee = item.fee === undefined ? 1 : Number(item.fee)
  return {
    id: id,
    title: name,
    artist: artist,
    album: album,
    duration_ms: Number(item.duration || item.dt || 0),
    // fee: 0 免费 / 8 免费低音质 / 1 会员 / 4 付费专辑
    bitrate: 128000,
    native_id: id,
    cover: ''
  }
}

globalThis.provider = {
  key: 'netease',
  name: '网易云',

  search: function (keyword, page, limit) {
    const url = 'http://music.163.com/api/search/get/web?s=' + encodeURIComponent(keyword) +
      '&type=1&limit=' + (limit | 0) + '&offset=' + ((page | 0) * (limit | 0))
    const r = http(url, {
      headers: {
        Referer: 'https://music.163.com/',
        'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)',
        Cookie: cookieHeader()
      }
    })
    if (r.error) throw new Error(r.error)
    const body = r.body
    if (!body || !body.result || !body.result.songs) throw new Error('接口未返回 songs（可能是风控）')
    const out = []
    for (let i = 0; i < body.result.songs.length; i++) {
      const s = songFrom(body.result.songs[i])
      if (s) out.push(s)
    }
    return out
  }
}
