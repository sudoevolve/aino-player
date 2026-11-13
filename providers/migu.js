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
  // MRC 协议为 64 位有符号 XXTEA、十六进制块与 UTF-16LE 正文。
  // 这里只还原行时间，不把逐字标签交给 LRC 排版器。
  function timedLyric(raw) {
    const hex = raw.trim();
    if (!/^[a-f0-9]+$/i.test(hex) || hex.length < 32 || hex.length % 16) throw new Error('无效咪咕 MRC 歌词');
    const signed = n => BigInt.asIntN(64, n), delta = 0x9e3779b9n;
    const secret = 'karakal@123Qcomyidongtiantianhaoting', key = [];
    for (let p = 0; p < 16; p += 4) {
      let word = 0n;
      for (let i = 0; i < 4; ++i) word |= BigInt(secret.charCodeAt(p + i)) << BigInt(i * 16);
      key.push(signed(word));
    }
    const words = [];
    for (let p = 0; p < hex.length; p += 16) words.push(signed(BigInt('0x' + hex.slice(p, p + 16))));
    const mix = (z, y, sum, p, e) => signed(
      signed((z >> 5n) ^ signed(y << 2n)) + signed((y >> 3n) ^ signed(z << 4n))) ^
      signed((y ^ sum) + (z ^ key[(p & 3) ^ e]));
    let sum = signed(BigInt(6 + Math.floor(52 / words.length)) * delta), y = words[0];
    while (sum !== 0n) {
      const e = Number((sum >> 2n) & 3n);
      for (let p = words.length - 1; p > 0; --p) y = words[p] = signed(words[p] - mix(words[p - 1], y, sum, p, e));
      y = words[0] = signed(words[0] - mix(words[words.length - 1], y, sum, 0, e));
      sum = signed(sum - delta);
    }
    let text = '';
    for (const word of words) for (let i = 0; i < 4; ++i)
      text += String.fromCharCode(Number((word >> BigInt(i * 16)) & 0xffffn));
    const lines = [];
    for (const line of text.replace(/\u0000+$/, '').split(/\r?\n/)) {
      const match = /^\s*\[(\d+),\d+\](.*)$/.exec(line);
      if (!match) continue;
      const ms = Number(match[1]);
      const stamp = '[' + String(Math.floor(ms / 60000)).padStart(2, '0') + ':' +
        String(Math.floor(ms / 1000) % 60).padStart(2, '0') + '.' + String(ms % 1000).padStart(3, '0') + ']';
      lines.push(stamp + match[2].replace(/\(\d+,\d+\)/g, ''));
    }
    if (!lines.length) throw new Error('咪咕 MRC 未返回有效时间轴');
    return lines.join('\n');
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
    lyric(info) {
      const id = String(info.id || ''), copyrightId = String(info.native_id || '');
      if (!/^\d+$/.test(id)) throw new Error('咪咕歌曲缺少 songId');
      const response = ctx.http('https://c.musicapp.migu.cn/MIGUM2.0/v1.0/content/resourceinfo.do?resourceType=2',
        {method: 'POST', headers: {'Content-Type': 'application/x-www-form-urlencoded', 'User-Agent': 'Mozilla/5.0'},
          body: 'resourceId=' + encodeURIComponent(id)});
      if (response.error || response.statusCode !== 200) throw new Error(response.error || '咪咕歌词资料请求失败');
      const rows = (response.body || {}).resource || [];
      const song = rows.find(s => String(s.songId || '') === id && (!copyrightId || String(s.copyrightId || '') === copyrightId));
      if (!song || (!song.lrcUrl && !song.mrcUrl)) throw new Error('咪咕未返回歌词');
      function text(url) {
        if (!url) return '';
        if (url.startsWith('//')) url = 'https:' + url;
        if (!/^https?:\/\//.test(url)) throw new Error('无效咪咕歌词地址');
        const r = ctx.http(url, {headers: {Referer: 'https://app.c.nf.migu.cn/', 'User-Agent': 'Mozilla/5.0', channel: '0146921'}});
        if (r.error || r.statusCode !== 200) throw new Error(r.error || '咪咕歌词 HTTP ' + r.statusCode);
        if (/^\s*</.test(r.bodyText || '')) throw new Error('咪咕返回了网页，未返回歌词');
        return r.bodyText || '';
      }
      let lyric = '';
      if (song.mrcUrl) try { lyric = timedLyric(text(song.mrcUrl)); } catch (_) {}
      if (!lyric) lyric = text(song.lrcUrl);
      let translation = ''; try { translation = text(song.trcUrl); } catch (_) {}
      return {lyric, translation};
    },
    heats() {
      const r = api('https://jadeite.migu.cn/music_search/v3/search/hotword');
      const words = (((r.data || {}).hotwords || [])[0] || {}).hotwordList || [];
      return words.filter(w => !w.resourceType || w.resourceType === 'song').map(w => ({text: w.word || ''}));
    },
    playlists(options) {
      const limit = Math.max(1, Math.min(50, Number((options || {}).limit) || 24));
      const cards = (Number((options || {}).page) ? [] : [['27553319', '尖叫新歌榜'], ['27186466', '尖叫热歌榜'], ['27553408', '尖叫原创榜']]).map(b =>
        ({id: 'rank:' + b[0], name: b[1], official: true, creator: '咪咕音乐'}));
      if (limit <= cards.length) return cards.slice(0, limit);
      const r = api('https://app.c.nf.migu.cn/pc/bmw/page-data/playlist-square-recommend/v1.0?templateVersion=2&pageNo=' + ((Number((options || {}).page) || 0) + 1));
      const seen = new Set();
      function visit(node, depth) {
        if (!node || typeof node !== 'object' || depth > 12 || cards.length >= limit + 3) return;
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
      if (!seen.size && !Number((options || {}).page)) throw new Error('咪咕未返回推荐歌单');
      return {items: cards, more: seen.size > 0};
    },
    playlist_search(options) {
      options = options || {}; const q = String(options.query || '').trim();
      const limit = Math.max(1, Math.min(50, Number(options.limit) || 20));
      const page = Math.max(0, Number(options.page) || 0);
      if (!q) return {items: [], more: false};
      const timestamp = String(Date.now()), deviceId = '963B7AA0D21511ED807EE5846EC87D20';
      const sign = ctx.md5(q + '6cdc72a439cef99a3418d2a78aa28c73' +
        'yyapp2d16148780a1dcc7408e06336b98cfd50' + deviceId + timestamp);
      const switches = {song: 0, album: 0, singer: 0, tagSong: 0, mvSong: 0, bestShow: 0, songlist: 1, lyricSong: 0};
      const r = api('https://jadeite.migu.cn/music_search/v3/search/searchAll?isCorrect=1&isCopyright=1&searchSwitch=' +
        encodeURIComponent(JSON.stringify(switches)) + '&pageSize=' + limit + '&text=' + encodeURIComponent(q) +
        '&pageNo=' + (page + 1) + '&sort=0&sid=USS', {deviceId, timestamp, sign, uiVersion: 'A_music_3.6.1', channel: '0146921'});
      if (!r.songListResultData) throw new Error("咪咕未返回歌单搜索结果");
      const data = r.songListResultData, rows = Array.isArray(data.result) ? data.result : [];
      const total = Number(data.totalCount || data.total || 0);
      return {items: rows.map(p => ({id: 'playlist:' + (p.id || p.songListId || p.contentId), name: p.name || p.songListName || '',
        cover: p.musicListPicUrl || p.picUrl || '', creator: p.userName || p.author || '', plays: Number(p.playNum) || 0, count: Number(p.musicNum || p.songNum) || 0,
        description: p.description || ''})).filter(p => p.id !== 'playlist:undefined' && p.name),
        more: total ? (page + 1) * limit < total : rows.length >= limit};
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
