// 咪咕公开目录。songId 和 copyrightId 各自保留，不互相代替。
(function () {
  function api(url, headers) {
    const r = ctx.http(url, {headers: Object.assign({'User-Agent': 'Mozilla/5.0 (Linux; Android 11) AppleWebKit/537.36 Mobile Safari/537.36'}, headers || {})});
    if (r.error || r.statusCode < 200 || r.statusCode >= 300) throw new Error(r.error || 'HTTP ' + r.statusCode);
    if (!r.body || String(r.body.code) !== '000000') throw new Error('咪咕接口拒绝请求或响应无效');
    return r.body;
  }
  function song(s) {
    let cover = s.img3 || s.img2 || s.img1 || ((s.albumImgs || s.imgItems || [])[0] || {}).img || '';
    if (cover.startsWith('//')) cover = 'https:' + cover;
    else if (cover[0] === '/') cover = 'https://d.musicapp.migu.cn' + cover;
    const singers = s.singerList || s.singers || s.artists || [];
    const duration = s.duration || s.length;
    let seconds = Number(duration) || 0;
    if (typeof duration === 'string' && duration.indexOf(':') >= 0)
      seconds = duration.split(':').reduce((a, b) => a * 60 + Number(b), 0);
    return {id: String(s.songId || s.id || ''), native_id: String(s.copyrightId || ''),
      title: s.name || s.songName || '', artist: singers.map(a => a.name).join(' / '),
      artists: singers.map(a => ({id: String(a.id || ''), name: a.name || ''})),
      album: s.album || ((s.albums || [])[0] || {}).name || '',
      album_id: String(s.albumId || ((s.albums || [])[0] || {}).id || ''),
      cover, duration_ms: seconds * 1000, bitrate: 128000};
  }
  globalThis.provider = {
    key: 'migu', name: '咪咕',
    search(keyword, page, limit) {
      const timestamp = String(Date.now()), deviceId = '963B7AA0D21511ED807EE5846EC87D20';
      const sign = ctx.md5(keyword + '6cdc72a439cef99a3418d2a78aa28c73' +
        'yyapp2d16148780a1dcc7408e06336b98cfd50' + deviceId + timestamp);
      const switches = {song: 1, album: 0, singer: 0, tagSong: 1, mvSong: 0, bestShow: 1, songlist: 0, lyricSong: 0};
      const r = api('https://jadeite.migu.cn/music_search/v3/search/searchAll?isCorrect=0&isCopyright=1&searchSwitch=' +
        encodeURIComponent(JSON.stringify(switches)) + '&pageSize=' + limit + '&text=' + encodeURIComponent(keyword) +
        '&pageNo=' + (page + 1) + '&sort=0&sid=USS',
        {deviceId, timestamp, sign, uiVersion: 'A_music_3.6.1', channel: '0146921'});
      if (!r.songResultData || !Array.isArray(r.songResultData.resultList)) throw new Error('咪咕未返回搜索列表');
      return r.songResultData.resultList.flat().map(song);
    },
    heats() {
      const r = api('https://jadeite.migu.cn/music_search/v3/search/hotword');
      const words = (((r.data || {}).hotwords || [])[0] || {}).hotwordList || [];
      return words.filter(w => !w.resourceType || w.resourceType === 'song').map(w => ({text: w.word || ''}));
    },
    playlists(options) {
      const limit = Math.max(1, Math.min(50, Number((options || {}).limit) || 24));
      const cards = [['27553319', '尖叫新歌榜'], ['27186466', '尖叫热歌榜'], ['27553408', '尖叫原创榜']].map(b =>
        ({id: 'rank:' + b[0], name: b[1], official: true, creator: '咪咕音乐'}));
      if (limit <= cards.length) return cards.slice(0, limit);
      const r = api('https://app.c.nf.migu.cn/pc/bmw/page-data/playlist-square-recommend/v1.0?templateVersion=2&pageNo=1');
      const seen = new Set();
      function visit(node, depth) {
        if (!node || typeof node !== 'object' || depth > 12 || cards.length >= limit) return;
        const id = String(node.resType) === '2021' ? node.resId : ((node.logEvent || {}).contentId);
        const name = node.txt || node.title;
        if (id && name && !seen.has(String(id)) && /^\d+$/.test(String(id))) {
          seen.add(String(id));
          let cover = node.img || node.imageUrl || '';
          if (cover.startsWith('//')) cover = 'https:' + cover;
          else if (cover[0] === '/') cover = 'https://d.musicapp.migu.cn' + cover;
          cards.push({id: 'playlist:' + id, name, cover, creator: '咪咕音乐'});
        }
        Object.keys(node).forEach(k => { if (typeof node[k] === 'object') visit(node[k], depth + 1); });
      }
      visit(r.data, 0);
      if (!seen.size) throw new Error('咪咕未返回推荐歌单');
      return cards;
    },
    board(id, page, limit) {
      if (/^playlist:\d+$/.test(String(id))) {
        const pid = String(id).slice(9), rows = [], size = Math.min(50, limit);
        for (let n = 0; n * size < limit; n++) {
          const r = api('https://app.c.nf.migu.cn/MIGUM3.0/resource/playlist/song/v2.0?pageNo=' +
            (page * Math.ceil(limit / size) + n + 1) + '&pageSize=' + size + '&playlistId=' + pid);
          if (!r.data || !Array.isArray(r.data.songList)) throw new Error('咪咕未返回歌单曲目');
          rows.push(...r.data.songList.map(song));
          if (r.data.songList.length < size || rows.length >= Number(r.data.totalCount)) break;
        }
        return rows.slice(0, limit);
      }
      id = String(id).replace(/^rank:/, '');
      if (!/^\d+$/.test(id)) throw new Error('无效咪咕榜单 ID');
      const r = api('https://app.c.nf.migu.cn/MIGUM2.0/v1.0/content/querycontentbyId.do?columnId=' + id + '&needAll=0');
      if (!r.columnInfo || !Array.isArray(r.columnInfo.contents)) throw new Error('咪咕未返回榜单曲目');
      return r.columnInfo.contents.slice(page * limit, (page + 1) * limit).map(s => song(s.objectInfo || s));
    }
  };
})();
