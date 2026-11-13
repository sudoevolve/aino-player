// 酷我公开目录。取流由用户导入的 LX 脚本负责。
(function () {
  function api(url) {
    const r = ctx.http(url, {headers: {'User-Agent': 'Mozilla/5.0', Referer: 'https://www.kuwo.cn/'}});
    if (r.error || r.statusCode < 200 || r.statusCode >= 300) throw new Error(r.error || 'HTTP ' + r.statusCode);
    if (!r.body || typeof r.body !== 'object') throw new Error('酷我响应不是 JSON');
    return r.body;
  }
  function song(s) {
    const id = String(s.MUSICRID || s.musicrid || s.id || s.rid || s.DC_TARGETID || '').replace(/^MUSIC_/, '');
    return {id, native_id: id, title: s.SONGNAME || s.NAME || s.name || '',
      artist: s.ARTIST || s.artist || '', album: s.ALBUM || s.album || '',
      album_id: String(s.ALBUMID || s.albumId || s.albumid || ''),
      cover: s.pic || s.picUrl || s.albumpic || (id ? 'platform-cover:kuwo:' + id : ''),
      duration_ms: (Number(s.song_duration || s.DURATION || s.duration) || 0) * 1000, bitrate: 128000};
  }
  globalThis.provider = {
    key: 'kuwo', name: '酷我',
    search(keyword, page, limit) {
      const r = api('https://search.kuwo.cn/r.s?client=kt&all=' + encodeURIComponent(keyword) +
        '&pn=' + page + '&rn=' + limit + '&ft=music&encoding=utf8&rformat=json&show_copyright_off=1&newver=1');
      if (!Array.isArray(r.abslist)) throw new Error('酷我未返回搜索列表');
      return r.abslist.map(song);
    },
    heats() {
      const r = api('https://hotword.kuwo.cn/hotword.s?prod=kwplayer_ar_9.3.0.1&corp=kuwo&newver=2' +
        '&vipver=9.3.0.1&source=kwplayer_ar_9.3.0.1_40.apk&p2p=1&notrace=0&uid=0&plat=kwplayer_ar&rformat=json&encoding=utf8&tabid=1');
      return (r.tagvalue || []).map(w => ({text: w.key || ''}));
    },
    playlists(options) {
      const charts = [['16', '热歌榜'], ['17', '新歌榜'], ['93', '飙升榜']].map(b =>
        ({id: 'rank:' + b[0], name: b[1], official: true, creator: '酷我音乐'}));
      const limit = Math.max(1, Math.min(50, Number((options || {}).limit) || 24));
      const r = api('https://wapi.kuwo.cn/api/pc/classify/playlist/getRcmPlayList?loginUid=0&loginSid=0&appUid=76039576&pn=1&rn=' + limit + '&order=hot');
      if (!r.data || !Array.isArray(r.data.data)) throw new Error('酷我未返回推荐歌单');
      return charts.concat(r.data.data.filter(p => /^(5|8)$/.test(String(p.digest))).map(p =>
        ({id: 'playlist:' + p.id, name: p.name, cover: p.img || '', creator: p.uname || '',
          plays: Number(p.listencnt) || 0, count: Number(p.total) || 0}))).slice(0, limit);
    },
    board(id, page, limit) {
      if (/^playlist:\d+$/.test(String(id))) {
        const pid = String(id).slice(9), rows = [], size = Math.min(100, limit);
        for (let n = 0; n * size < limit; n++) {
          const r = api('https://nplserver.kuwo.cn/pl.svc?op=getlistinfo&pid=' + pid + '&pn=' + (page * Math.ceil(limit / size) + n) +
            '&rn=' + size + '&encode=utf8&keyset=pl2012&identity=kuwo&pcmp4=1&vipver=MUSIC_9.0.5.0_W1&newver=1');
          if (!Array.isArray(r.musiclist)) throw new Error('酷我未返回歌单曲目');
          rows.push(...r.musiclist.map(song));
          if (r.musiclist.length < size || rows.length >= Number(r.total)) break;
        }
        return rows.slice(0, limit);
      }
      id = String(id).replace(/^rank:/, '');
      if (!/^\d+$/.test(id)) throw new Error('无效酷我榜单 ID');
      const r = api('https://kbangserver.kuwo.cn/ksong.s?from=pc&fmt=json&type=bang&data=content&id=' +
        id + '&pn=' + page + '&rn=' + limit + '&show_copyright_off=0&pcmp4=1&isbang=1');
      if (!Array.isArray(r.musiclist)) throw new Error('酷我未返回榜单曲目');
      return r.musiclist.map(song);
    }
  };
})();
