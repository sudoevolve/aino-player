// 网易云 music provider —— 只做搜索。
// 搜索走 music.163.com 的只读接口。
//
// 取流不在这里实现，交给 C++ 的 resolve_netease：它按数字 song id 精确取，不会串歌。
// 平台脚本反向调用第三方音源时，按歌名匹配可能命中同名翻唱，
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
    needs_login: fee === 1 || fee === 4,
    album_id: String(((item.album || item.al || {}).id) || ''),
    artists: artists.map(a => ({id: String(a.id || ''), name: a.name || ''})),
    cover: (item.album || item.al || {}).picUrl || ''
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

provider.heats = function () {
  const r = http('https://music.163.com/api/search/hot?type=1111');
  if (r.error || !r.body || !r.body.result) throw new Error(r.error || '热搜响应无效');
  return (r.body.result.hots || []).map(w => ({text: w.first || ''}));
};
provider.playlists = function (args) {
  const headers = {Referer: 'https://music.163.com/'};
  const top = http('https://music.163.com/api/toplist', {headers});
  const cards = (((top.body || {}).list) || []).slice(0, 3).map(p =>
    ({id: String(p.id), name: p.name, cover: p.coverImgUrl, official: true}));
  const r = http('https://music.163.com/api/playlist/list?cat=' + encodeURIComponent('全部') +
    '&order=hot&offset=0&limit=' + (args.limit || 30), {headers});
  if (r.error || !r.body || !Array.isArray(r.body.playlists)) {
    if (cards.length) return cards;
    throw new Error(r.error || '歌单响应无效');
  }
  return cards.concat(r.body.playlists.map(p => ({id: String(p.id), name: p.name,
    cover: p.coverImgUrl || p.picUrl || '', creator: (p.creator || {}).nickname || '',
    plays: p.playCount || p.subscribedCount || 0, count: p.trackCount || 0})));
};
