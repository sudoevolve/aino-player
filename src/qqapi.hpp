// qqapi.hpp —— QQ 音乐 web 接口（需要登录 cookie）
//
// 三个能力：
//   1. fetch_profile      用户资料 + 歌单列表
//   2. fetch_playlist     某个歌单里的歌（转成 music::Track，直接复用播放/下载/歌词）
//   3. add_to_playlist    把歌加进歌单（写操作，接口无公开文档，失败时原样带回错误码）
//
// 这些接口都是 QQ 音乐网页自己在用的，参数里必须带 g_tk（由 cookie 里的 skey 算哈希），
// 少了它一律返回 1000 / -1000 之类的鉴权错误。
#pragma once

#include <string>
#include <vector>

#include "json.hpp"
#include "net.hpp"
#include "sources.hpp"

namespace qq {

struct Playlist {
    std::string dirid;   // disstid
    std::string name;
    std::string cover;
    int count = 0;
};

struct Profile {
    std::string uin;
    std::string nickname;
    std::vector<Playlist> playlists;
    std::string error;
    bool ok() const { return error.empty(); }
};

// ---------------------------------------------------------------- 小工具

inline std::string cookie_value(const std::string& jar, const char* key) {
    const std::string k = std::string(key) + "=";
    size_t pos = 0;
    while (pos < jar.size()) {
        while (pos < jar.size() && (jar[pos] == ' ' || jar[pos] == ';')) ++pos;
        const size_t end = jar.find(';', pos);
        const std::string item = jar.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (item.rfind(k, 0) == 0) return item.substr(k.size());
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return std::string();
}

// QQ 的 g_tk：djb2 变体，取低 31 位
inline int calc_g_tk(const std::string& skey) {
    uint32_t h = 5381;
    for (unsigned char c : skey) h += (h << 5) + uint32_t(c);
    return h & 0x7fffffff;
}

inline std::string g_tk_of(const std::string& jar) {
    std::string skey = cookie_value(jar, "p_skey");
    if (skey.empty()) skey = cookie_value(jar, "skey");
    if (skey.empty()) skey = cookie_value(jar, "qqmusic_key");
    if (skey.empty()) skey = cookie_value(jar, "musickey");
    if (skey.empty()) skey = cookie_value(jar, "qm_keyst");
    return std::to_string(calc_g_tk(skey));
}

inline std::string clean_uin(std::string uin) {
    if (uin.size() > 1 && (uin[0] == 'o' || uin[0] == 'O')) uin = uin.substr(1);
    size_t i = 0;
    while (i + 1 < uin.size() && uin[i] == '0') ++i;
    return uin.substr(i);
}

inline std::vector<std::string> qq_headers(const std::string& cookie) {
    return {"Referer: https://y.qq.com/",
            "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
            "Chrome/122.0 Safari/537.36",
            "Cookie: " + cookie};
}

// 解析失败时把响应开头带回来，否则用户只看到"失败了"三个字，没法判断是登录过期还是接口变了
inline std::string snippet(const std::string& body) {
    std::string s = body.substr(0, 160);
    for (char& c : s)
        if (c == '\n' || c == '\r') c = ' ';
    return s;
}

// ---------------------------------------------------------------- 用户歌单

inline Profile fetch_profile(const std::string& cookie, const std::string& uin_raw, int timeout_ms = 10000) {
    Profile p;
    p.uin = clean_uin(uin_raw);
    if (cookie.empty() || p.uin.empty()) {
        p.error = "未登录";
        return p;
    }
    const std::string url = "https://c.y.qq.com/rsc/fcgi-bin/fcg_user_created_diss?hostuin=" + p.uin +
                            "&size=200&g_tk=" + g_tk_of(cookie) +
                            "&format=json&inCharset=utf8&outCharset=utf-8&notice=0&platform=yqq.json&needNewCode=0";
    const net::Response r = net::get(url, qq_headers(cookie), timeout_ms);
    if (!r.ok()) {
        p.error = "歌单接口失败（HTTP " + std::to_string(r.status) + "）";
        return p;
    }
    js::Value root;
    if (!js::parse(r.body, root, true)) {
        p.error = "歌单返回不是 JSON：" + snippet(r.body);
        return p;
    }
    const int code = int(root["code"].integer());
    if (code != 0) {
        p.error = "歌单接口返回 code " + std::to_string(code) + "（多半是登录过期，重新登录一次）";
        return p;
    }
    // 字段名和网上老教程写的不是一套：现在返回的是 diss_name / diss_cover / song_cnt / dirid，
    // 旧版是 dissname / logo / songnum / dissid。两套都读，免得服务端再改一次就瞎。
    p.nickname = root["data"]["hostname"].str(root["data"]["nickname"].str());
    const js::Value& list = root["data"]["disslist"];
    for (size_t i = 0; i < list.size(); ++i) {
        Playlist pl;
        pl.dirid = std::to_string(list[i]["dirid"].integer());
        if (pl.dirid.empty() || pl.dirid == "0") {
            pl.dirid = std::to_string(list[i]["dissid"].integer());
            if (pl.dirid == "0") pl.dirid = list[i]["dissid"].str();
        }
        pl.name = music::text::decode_entities(list[i]["diss_name"].str(list[i]["dissname"].str()));
        pl.cover = list[i]["diss_cover"].str(list[i]["logo"].str());
        pl.count = int(list[i]["song_cnt"].integer());
        if (pl.count == 0) pl.count = int(list[i]["songnum"].integer());
        if (pl.dirid.empty() || pl.name.empty()) continue;
        if (pl.count <= 0) continue;   // QZone背景音乐 / 本地上传 这类空占位
        p.playlists.push_back(std::move(pl));
    }
    if (p.playlists.empty()) p.error = "这个账号下没有自建歌单（收藏的歌单不在这里）";
    return p;
}

// ---------------------------------------------------------------- 歌单内容

// 歌单分两种，参数正好相反（这是实测出来的，文档里没有）：
//   自建歌单  disstid=0,      dirid=<编号>
//   广场歌单  disstid=<编号>,  dirid 不传
// 传错一律 -100008 param error。plaza=true 表示后者（热门歌单 / 别人分享的歌单）。
inline std::vector<music::Track> fetch_playlist(const std::string& cookie, const std::string& dirid,
                                                const std::string& uin_raw, std::string& err,
                                                int timeout_ms = 15000, bool plaza = false, Playlist* metadata = nullptr) {
    std::vector<music::Track> out;
    const std::string uin = clean_uin(uin_raw);
    // 广场歌单匿名就能读，uin 给 0 即可；自建歌单必须有登录态
    if (dirid.empty() || (!plaza && cookie.empty())) {
        err = "未登录或歌单号为空";
        return out;
    }

    for (int page = 0; page < 20; ++page) {
        char body[640];
        const std::string uin_json = uin.empty() ? std::string("0") : uin;
        if (plaza)
            std::snprintf(body, sizeof body,
                          "{\"comm\":{\"ct\":24,\"cv\":0,\"format\":\"json\"},"
                          "\"req\":{\"module\":\"music.srfDissInfo.aiDissInfo\",\"method\":\"uniform_get_Dissinfo\","
                          "\"param\":{\"disstid\":%s,\"enc_host_uin\":\"\",\"tag\":1,\"userinfo\":1,"
                          "\"song_begin\":%d,\"song_num\":200,\"order\":1}}}",
                          dirid.c_str(), page * 200);
        else
            std::snprintf(body, sizeof body,
                          "{\"comm\":{\"ct\":19,\"cv\":1859,\"uin\":\"%s\",\"format\":\"json\"},"
                          "\"req\":{\"module\":\"music.srfDissInfo.aiDissInfo\",\"method\":\"uniform_get_Dissinfo\","
                          "\"param\":{\"disstid\":0,\"dirid\":%s,\"tag\":1,\"userinfo\":1,"
                          "\"song_begin\":%d,\"song_num\":200}}}",
                          uin_json.c_str(), dirid.c_str(), page * 200);

        std::vector<std::string> headers = qq_headers(cookie);
        headers.push_back("Content-Type: application/json");
        const net::Response r =
            net::post("https://u.y.qq.com/cgi-bin/musicu.fcg?format=json", body, headers, timeout_ms);
        if (!r.ok()) {
            err = "歌单详情失败（HTTP " + std::to_string(r.status) + "）";
            return out;
        }
        js::Value root;
        if (!js::parse(r.body, root, true)) {
            err = "歌单详情不是 JSON：" + snippet(r.body);
            return out;
        }
        const js::Value& data = root["req"]["data"];
        const long long code = data["code"].integer();
        if (code != 0) {
            err = "歌单详情被拒（code " + std::to_string(code) + "）：" + data["msg"].str();
            return out;
        }
        const js::Value& list = data["songlist"];
        if (metadata && page == 0) {
            metadata->dirid = dirid;
            metadata->name = music::text::decode_entities(data["dirinfo"]["title"].str(data["dissinfo"]["dissname"].str()));
            metadata->cover = data["dirinfo"]["picurl"].str(data["dissinfo"]["logo"].str());
            metadata->count = int(data["total_song_num"].integer());
        }
        if (list.size() == 0) break;

        for (size_t i = 0; i < list.size(); ++i) {
            const js::Value& s = list[i];
            const std::string mid = s["mid"].str(s["songmid"].str());
            if (mid.empty()) continue;
            music::Track t;
            t.title = music::text::decode_entities(s["name"].str(s["title"].str()));
            std::string artists;
            const js::Value& singers = s["singer"];
            for (size_t k = 0; k < singers.size(); ++k) {
                if (k) artists += " / ";
                artists += music::text::decode_entities(singers[k]["name"].str());
            }
            t.artist = artists;
            t.album = music::text::decode_entities(s["album"]["name"].str());
            const long long interval = s["interval"].integer();
            t.duration_ms = int(interval > 0 ? interval * 1000 : 0);
            const std::string album_mid = s["album"]["mid"].str();
            if (!album_mid.empty())
                t.cover_url = "https://y.gtimg.cn/music/photo_new/T002R300x300M000" + album_mid + ".jpg";

            music::Variant v;
            v.source = music::Source::QQ;
            v.id = mid;
            v.title = t.title;
            v.artist = t.artist;
            v.album = t.album;
            v.duration_ms = t.duration_ms;
            v.cover_url = t.cover_url;
            v.native_id = std::to_string(s["id"].integer());
            t.variants.push_back(v);
            out.push_back(std::move(t));
        }
        if (list.size() < 200) break;
    }

    if (out.empty()) err = "这个歌单是空的，或者接口没返回歌曲";
    return out;
}

// ---------------------------------------------------------------- 写入歌单

// 把歌曲加进指定歌单。成功返回 true；失败时 err 带上服务端 code，
// 因为这类写接口的失败原因（风控 / 歌单满 / 版权区限制）只能从 code 判断。
inline bool add_to_playlist(const std::string& cookie, const std::string& uin_raw, const std::string& dirid,
                            const std::vector<std::string>& song_ids, std::string& err) {
    if (song_ids.empty()) {
        err = "没有可同步的歌";
        return false;
    }
    const std::string uin = clean_uin(uin_raw);

    std::string ids;
    for (size_t i = 0; i < song_ids.size(); ++i) {
        // songId 要数值：歌单里的 mid 是字符串，这里只能用 songid 的话就传 mid 让服务端自己解
        if (i) ids += ",";
        ids += "\"" + song_ids[i] + "\"";
    }

    const std::string body =
        "{\"comm\":{\"ct\":24,\"cv\":0,\"uin\":\"" + uin + "\",\"format\":\"json\"},"
        "\"req\":{\"module\":\"music.musicAsset.PlaylistAddSong\",\"method\":\"addSonglist\",\"param\":{"
        "\"dirId\":" + (dirid.empty() ? std::string("0") : dirid) + ","
        "\"songInfoList\":[" + [&] {
            std::string arr;
            for (size_t i = 0; i < song_ids.size(); ++i) {
                if (i) arr += ",";
                arr += "{\"songId\":" + song_ids[i] + ",\"songType\":0}";
            }
            return arr;
        }() + "],"
        "\"dirInfo\":{\"id\":" + (dirid.empty() ? std::string("0") : dirid) +
        ",\"title\":\"\",\"desc\":\"\",\"cover\":\"\",\"tag\":[],\"songNum\":0,\"dirType\":0,"
        "\"createTime\":\"\",\"updateTime\":\"\"}}}}";

    std::vector<std::string> headers = qq_headers(cookie);
    headers.push_back("Content-Type: application/json");
    const net::Response r = net::post("https://u.y.qq.com/cgi-bin/musicu.fcg?format=json&g_tk=" + g_tk_of(cookie) +
                                          "&loginUin=" + uin,
                                      body, headers, 12000);
    if (!r.ok()) {
        err = "同步请求失败（HTTP " + std::to_string(r.status) + "）";
        return false;
    }
    js::Value root;
    if (!js::parse(r.body, root, true)) {
        err = "同步返回不是 JSON：" + snippet(r.body);
        return false;
    }
    const long long code = root["req"]["code"].integer();
    if (code != 0) {
        err = "同步被拒（code " + std::to_string(code) + "）：" + snippet(r.body);
        return false;
    }
    return true;
}

}  // namespace qq
