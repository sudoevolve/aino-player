// QQ 音乐 provider —— 只做搜索。
// 搜索用 musicu.fcg 的固定信封（和 C++ 版一致）。
//
// 取流不在这里实现，交给 C++ 的 resolve_qq：它按 songmid 精确拼 filename 再查 vkey，
// 拿到的必然是这首 mid 自己的音轨。
// 平台脚本反向调用第三方音源会混淆渠道：
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
    needs_login: pay !== 0,
    album_id: String((item.album || {}).mid || item.albummid || ''),
    artists: singers.map(a => ({id: String(a.mid || ''), name: a.name || ''})),
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

provider.heats = function () {
  const headers = {'User-Agent': 'Mozilla/5.0', Referer: 'https://y.qq.com/portal/player.html'};
  const body = JSON.stringify({comm: {ct: '19', cv: '1803', uin: '0'},
    hotkey: {method: 'GetHotkeyForQQMusicPC', module: 'tencent_musicsoso_hotkey.HotkeyService',
      param: {search_id: '', uin: 0}}});
  const failures = [];
  // 只在主请求失败时走备用官方入口，不再使用旧热搜地址。
  for (const host of ['u.y.qq.com', 'u6.y.qq.com']) {
    const r = http('https://' + host + '/cgi-bin/musicu.fcg', {method: 'POST', body,
      headers: Object.assign({'Content-Type': 'application/json'}, headers), timeout: 3000});
    const root = r.body || {}, hotkey = root.hotkey || {}, data = hotkey.data || {};
    if (!r.error && r.status === 200 && Number(root.code) === 0 && Number(hotkey.code) === 0 &&
        (!('ret_code' in data) || Number(data.ret_code) === 0) && Array.isArray(data.vec_hotkey)) {
      const rows = data.vec_hotkey.map(w => ({text: String(w.query || '').trim(), heat: Number(w.score) || 0})).filter(w => w.text);
      if (rows.length) return rows;
    }
    failures.push(host + '：' + (r.error || 'HTTP ' + r.status + ' / code ' + root.code + ',' + hotkey.code));
  }
  throw new Error('QQ 热搜暂时不可用：' + failures.join('；'));
};
provider.playlists = function (args) {
  const r = http('https://c.y.qq.com/splcloud/fcgi-bin/fcg_get_diss_by_tag.fcg?picmid=1&rnd=0.1' +
    '&g_tk=5381&loginUin=0&hostUin=0&format=json&inCharset=utf8&outCharset=utf-8' +
    '&notice=0&platform=yqq.json&needNewCode=0&categoryId=10000000&sortId=5&sin=0&ein=' + (args.limit || 30),
    {headers: {Referer: 'https://y.qq.com/'}});
  if (r.error || !r.body || Number(r.body.code) !== 0 || !r.body.data) throw new Error(r.error || '歌单响应无效');
  return (r.body.data.list || []).map(p => ({id: String(p.dissid), name: p.dissname, cover: p.imgurl,
    creator: (p.creator || {}).name || '', plays: Number(p.listennum) || 0}));
};
