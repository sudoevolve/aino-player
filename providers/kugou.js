// 酷狗公开目录；保留不同音质的 hash 和 MixSongID，避免取流串 ID。
(function () {
  function api(url, headers) {
    const r = ctx.http(url, {headers: Object.assign({'User-Agent': 'Mozilla/5.0', Referer: 'https://www.kugou.com/'}, headers || {})});
    if (r.error || r.statusCode < 200 || r.statusCode >= 300) throw new Error(r.error || 'HTTP ' + r.statusCode);
    if (!r.body || typeof r.body !== 'object') throw new Error('酷狗响应不是 JSON');
    if (Number(r.body.error_code || r.body.errcode || 0) !== 0) throw new Error('酷狗接口拒绝请求');
    return r.body;
  }
  function song(s) {
    const hash = s.FileHash || s.hash || '';
    const artist = (s.Singers || s.authors || []).map(a => a.name || a.author_name).join(' / ') || s.SingerName || s.singername || String(s.filename || '').split(' - ')[0];
    const title = s.OriSongName || s.SongName || s.songname || String(s.FileName || s.filename || '').replace(/^.*? - /, '');
    const hashes = {'128k': hash, '320k': s.HQFileHash || s['320hash'], flac: s.SQFileHash || s.sqhash,
      flac24bit: s.ResFileHash || s.hash_high};
    Object.keys(hashes).forEach(k => {if (!hashes[k]) delete hashes[k];});
    return {id: String(s.Audioid || s.audio_id || s.MixSongID || hash), native_id: hash,
      album_audio_id: String(s.MixSongID || s.album_audio_id || ''), quality_hashes: hashes,
      title, artist, album: s.AlbumName || s.remark || '', album_id: String(s.AlbumID || s.album_id || ''),
      duration_ms: (Number(s.Duration || s.duration) || 0) * 1000, bitrate: 128000,
      cover: String(s.Image || s.img || s.album_sizable_cover || (s.trans_param || {}).union_cover || '').replace(/\{size\}/g, '300').replace(/^\/\//, 'https://')};
  }
  globalThis.provider = {
    key: 'kugou', name: '酷狗',
    search(keyword, page, limit) {
      const r = api('https://songsearch.kugou.com/song_search_v2?platform=AndroidFilter&iscorrection=1&keyword=' +
        encodeURIComponent(keyword) + '&hifiquality=0&pagesize=' + limit + '&PrivilegeFilter=0&page=' + (page + 1));
      if (!r.data || !Array.isArray(r.data.lists)) throw new Error('酷狗未返回搜索列表');
      return r.data.lists.map(song);
    },
    lyric(info) {
      const hash = String(info.native_id || '');
      if (!/^[a-f0-9]{32}$/i.test(hash)) throw new Error('酷狗歌曲缺少有效 hash');
      // 歌词检索使用精确音频 hash；不使用未经校验的同名候选。
      const r = ctx.http('https://lyrics.kugou.com/search?ver=1&man=yes&client=pc&lrctxt=1&keyword=' +
        encodeURIComponent(info.title || '') + '&hash=' + hash + '&timelength=' + (Number(info.duration_ms) || 0),
        {headers: {'User-Agent': 'KuGou2012-9020-ExpandSearchManager', 'KG-RC': '1'}});
      const candidates = ((r.body || {}).candidates || []).filter(c => !info.duration_ms ||
        !c.duration || Math.abs(Number(c.duration) - Number(info.duration_ms)) < 5000);
      if (r.error || !candidates.length) throw new Error(r.error || '酷狗未返回匹配歌词');
      const candidate = candidates[0];
      const d = ctx.http('https://lyrics.kugou.com/download?ver=1&client=pc&id=' + encodeURIComponent(candidate.id) +
        '&accesskey=' + encodeURIComponent(candidate.accesskey) + '&fmt=lrc&charset=utf8',
        {headers: {'User-Agent': 'KuGou2012-9020-ExpandSearchManager', 'KG-RC': '1'}});
      if (d.error || !d.body || !d.body.content || d.body.fmt !== 'lrc') throw new Error(d.error || '酷狗未返回 LRC 歌词');
      const bytes = ctx.b64decode(d.body.content);
      const encoded = Array.from(bytes, b => '%' + b.toString(16).padStart(2, '0')).join('');
      return {lyric: decodeURIComponent(encoded)};
    },
    heats() {
      const r = api('https://gateway.kugou.com/api/v3/search/hot_tab?signature=ee44edb9d7155821412d220bcaf509dd&appid=1005&clientver=10026&plat=0',
        {dfid: '1ssiv93oVqMp27cirf2CvoF1', mid: '156798703528610303473757548878786007104',
          clienttime: '1584257267', 'x-router': 'msearch.kugou.com', 'kg-rc': '1'});
      return (((r.data || {}).list) || []).flatMap(g => (g.keywords || []).map(w => ({text: w.keyword || ''})));
    },
    playlists(options) {
      const limit = Math.max(1, Math.min(50, Number((options || {}).limit) || 30));
      const charts = (Number((options || {}).page) ? [] : [['8888', 'TOP 500'], ['6666', '飙升榜'], ['52144', '抖音热歌榜']]).map(b =>
        ({id: 'rank:' + b[0], name: b[1], official: true, creator: '酷狗音乐'}));
      if (limit <= charts.length) return charts.slice(0, limit);
      const r = api('https://m.kugou.com/plist/index?json=true&page=' + ((Number((options || {}).page) || 0) + 1));
      const rows = (((r.plist || {}).list || {}).info);
      if (!Array.isArray(rows)) throw new Error('酷狗未返回推荐歌单');
      return {more: rows.length > 0, items: charts.concat(rows.filter(p => p.specialid && p.specialname).map(p =>
        ({id: 'playlist:' + p.specialid, name: p.specialname,
          cover: String(p.imgurl || p.img || '').replace(/\{size\}/g, '300').replace(/^\/\//, 'https://'),
          creator: p.username || p.nickname || '', plays: Number(p.playcount || p.total_play_count) || 0,
          count: Number(p.songcount) || 0})))};
    },
    playlist_search(options) {
      options = options || {}; const q = String(options.query || '').trim();
      const limit = Math.max(1, Math.min(50, Number(options.limit) || 20));
      const page = Math.max(0, Number(options.page) || 0);
      if (!q) return {items: [], more: false};
      const r = api('https://gateway.kugou.com/api/v3/search/special?keyword=' + encodeURIComponent(q) +
        '&page=' + (page + 1) + '&pagesize=' + limit + '&showtype=10&filter=0&version=7910&sver=2',
        {'x-router': 'msearchretry.kugou.com'});
      if (!r.data || (!Array.isArray(r.data.info) && Number(r.data.total) !== 0)) throw new Error("酷狗未返回歌单搜索结果");
      const data = r.data, rows = Array.isArray(data.info) ? data.info : [];
      const total = Number(data.total || r.total || 0);
      return {items: rows.map(p => ({id: 'playlist:' + p.specialid, name: p.specialname || '',
        cover: String(p.imgurl || '').replace(/\{size\}/g, '300').replace(/^\/\//, 'https://'),
        creator: p.nickname || '', plays: Number(p.playcount) || 0, count: Number(p.songcount) || 0,
        description: p.intro || ''})).filter(p => p.id !== 'playlist:undefined' && p.name),
        more: total ? (page + 1) * limit < total : rows.length >= limit};
    },
    board(id, page, limit) {
      if (/^playlist:\d+$/.test(String(id))) {
        const pid = String(id).slice(9), size = Math.max(1, Math.min(100, limit));
        const r = api('https://gateway.kugou.com/api/v3/special/song?specialid=' + pid + '&page=' +
          (page + 1) + '&pagesize=' + size + '&version=9108&plat=0', {'x-router': 'mobilecdnbj.kugou.com'});
        if (!r.data || !Array.isArray(r.data.info)) throw new Error('酷狗未返回歌单曲目');
        const total = Number(r.data.total) || 0;
        return {items: r.data.info.map(song), total: total || undefined,
          more: total ? (page + 1) * size < total : r.data.info.length >= size};
      }
      id = String(id).replace(/^rank:/, '');
      if (!/^\d+$/.test(id)) throw new Error('无效酷狗榜单 ID');
      // 旧目录主机的 HTTPS 证书不匹配，通过官方 HTTPS 网关转发。
      const r = api('https://gateway.kugou.com/api/v3/rank/song?version=9108&ranktype=1&plat=0&area_code=1' +
        '&with_res_tag=0&show_portrait_mv=1&rankid=' + id + '&page=' + (page + 1) + '&pagesize=' + limit,
        {'x-router': 'mobilecdnbj.kugou.com'});
      if (!r.data || !Array.isArray(r.data.info)) throw new Error('酷狗未返回榜单曲目');
      const total = Number(r.data.total) || 0;
      return {items: r.data.info.map(song), total: total || undefined,
        more: total ? (page + 1) * limit < total : r.data.info.length >= limit};
    }
  };
})();
