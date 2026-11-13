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
    heats() {
      const r = api('https://gateway.kugou.com/api/v3/search/hot_tab?signature=ee44edb9d7155821412d220bcaf509dd&appid=1005&clientver=10026&plat=0',
        {dfid: '1ssiv93oVqMp27cirf2CvoF1', mid: '156798703528610303473757548878786007104',
          clienttime: '1584257267', 'x-router': 'msearch.kugou.com', 'kg-rc': '1'});
      return (((r.data || {}).list) || []).flatMap(g => (g.keywords || []).map(w => ({text: w.keyword || ''})));
    },
    playlists(options) {
      const limit = Math.max(1, Math.min(50, Number((options || {}).limit) || 30));
      const charts = [['8888', 'TOP 500'], ['6666', '飙升榜'], ['52144', '抖音热歌榜']].map(b =>
        ({id: 'rank:' + b[0], name: b[1], official: true, creator: '酷狗音乐'}));
      if (limit <= charts.length) return charts.slice(0, limit);
      const r = api('https://m.kugou.com/plist/index?json=true');
      const rows = (((r.plist || {}).list || {}).info);
      if (!Array.isArray(rows) || !rows.length) throw new Error('酷狗未返回推荐歌单');
      return charts.concat(rows.filter(p => p.specialid && p.specialname).map(p =>
        ({id: 'playlist:' + p.specialid, name: p.specialname,
          cover: String(p.imgurl || p.img || '').replace(/\{size\}/g, '300').replace(/^\/\//, 'https://'),
          creator: p.username || p.nickname || '', plays: Number(p.playcount || p.total_play_count) || 0,
          count: Number(p.songcount) || 0}))).slice(0, limit);
    },
    board(id, page, limit) {
      if (/^playlist:\d+$/.test(String(id))) {
        const pid = String(id).slice(9), rows = [], size = Math.max(1, Math.min(100, limit));
        for (let n = 0; n * size < limit; n++) {
          const r = api('https://gateway.kugou.com/api/v3/special/song?specialid=' + pid + '&page=' +
            (page * Math.ceil(limit / size) + n + 1) + '&pagesize=' + size + '&version=9108&plat=0',
            {'x-router': 'mobilecdnbj.kugou.com'});
          if (!r.data || !Array.isArray(r.data.info)) throw new Error('酷狗未返回歌单曲目');
          rows.push(...r.data.info.map(song));
          if (r.data.info.length < size || rows.length >= Number(r.data.total)) break;
        }
        return rows.slice(0, limit);
      }
      id = String(id).replace(/^rank:/, '');
      if (!/^\d+$/.test(id)) throw new Error('无效酷狗榜单 ID');
      // 旧目录主机的 HTTPS 证书不匹配，通过官方 HTTPS 网关转发。
      const r = api('https://gateway.kugou.com/api/v3/rank/song?version=9108&ranktype=1&plat=0&area_code=1' +
        '&with_res_tag=0&show_portrait_mv=1&rankid=' + id + '&page=' + (page + 1) + '&pagesize=' + limit,
        {'x-router': 'mobilecdnbj.kugou.com'});
      if (!r.data || !Array.isArray(r.data.info)) throw new Error('酷狗未返回榜单曲目');
      return r.data.info.map(song);
    }
  };
})();
