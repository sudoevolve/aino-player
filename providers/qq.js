// QQ 音乐 provider —— 只做搜索。
// 搜索用 musicu.fcg 的固定信封（和 C++ 版一致）。
//
// 取流不在这里实现，交给 C++ 的 resolve_qq：它按 songmid 精确拼 filename 再查 vkey，
// 拿到的必然是这首 mid 自己的音轨。
// 曾经这里写成 ctx.lxUrl('tx', ...) 转手问落雪音源，结果很糟：
// 音源脚本按「歌名 + 歌手」匹配，遇到会员歌/无版权时会回落到同名的翻唱版本，
// 而这条链路又被标成 builtin，界面上显示"QQ · 自带"——实际放的是别的歌。
// 所以：自带渠道不许借用音源；要用音源请走音源渠道（Lane::JsSource）。

function http(url, opts) {
  const r = ctx.http(url, opts || {})
  if (!r || r.error) return { error: (r && r.error) || 'network' }
  return { status: r.statusCode || r.status, body: r.body, text: r.bodyText }
}

function jsonEscape(s) {
  return String(s).replace(/\\/g, '\\\\').replace(/"/g, '\\"').replace(/[\u0000-\u001f]/g, ' ')
}

function songFrom(item) {
  const mid = item.mid || item.songmid || ''
  const name = item.name || item.songname || ''
  if (!mid || !name) return null
  const singers = item.singer || []
  let artist = ''
  for (let i = 0; i < singers.length; i++) {
    if (i) artist += '/'
    artist += singers[i].name || ''
  }
  const album = (item.album && item.album.name) || item.albumname || ''
  const pmid = (item.album && item.album.pmid) || ''
  const pay = (item.pay && item.pay.pay_play) || 0
  return {
    id: mid,
    title: name,
    artist: artist,
    album: album,
    duration_ms: (Number(item.interval) || 0) * 1000,
    bitrate: 128000,
    native_id: mid,
    cover: pmid ? 'https://y.gtimg.cn/music/photo_new/T002R300x300M000' + pmid + '.jpg' : ''
  }
}

globalThis.provider = {
  key: 'qq',
  name: 'QQ',

  search: function (keyword, page, limit) {
    const body = '{"comm":{"ct":"19","cv":"1859","uin":"0"},' +
      '"req":{"method":"DoSearchForQQMusicDesktop","module":"music.search.SearchCgiService",' +
      '"param":{"grp":1,"num_per_page":' + (limit | 0) + ',"page_num":' + ((page | 0) + 1) +
      ',"query":"' + jsonEscape(keyword) + '","search_type":0}}}'
    const headers = {
      'Content-Type': 'application/json',
      Referer: 'https://y.qq.com/',
      'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)'
    }
    const ck = ctx.cookie('qq') || ''
    if (ck) headers.Cookie = ck

    const r = http('https://u.y.qq.com/cgi-bin/musicu.fcg', { method: 'POST', body: body, headers: headers })
    if (r.error) throw new Error(r.error)
    const root = r.body
    if (!root || !root.req) throw new Error('响应不是 JSON')
    if (Number(root.req.code) !== 0) {
      // 2001 = 匿名搜索被拒，不是"没结果"
      throw new Error(ck ? ('接口返回 code ' + root.req.code) : ('需登录（code ' + root.req.code + '）'))
    }
    const list = (((root.req || {}).data || {}).body || {}).song || {}
    const songs = list.list || []
    const out = []
    for (let i = 0; i < songs.length; i++) {
      const s = songFrom(songs[i])
      if (s) out.push(s)
    }
    return out
  }
}
