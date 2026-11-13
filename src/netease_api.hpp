// netease_api.hpp —— 网易云音乐账号相关接口（需要登录 cookie）
//
// 三个能力：
//   1. fetch_profile      账号资料 + 「我的歌单」列表（含"我喜欢的音乐"）
//   2. fetch_playlist     某个歌单里的歌（转成 music::Track，直接复用播放/下载/歌词）
//   3. add_tracks         把歌加进歌单（写操作，靠 cookie 里的 __csrf 过鉴权）
//
// 这些都是网页版自己在用的明文接口（不走 weapi 的 AES 加密），
// 只要 cookie 里有 MUSIC_U，带 Referer 就能通。
#pragma once

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "json.hpp"
#include "net.hpp"
#include "sources.hpp"

namespace ne {

struct Playlist {
    std::string id;
    std::string name;
    std::string cover;
    int count = 0;
};

struct Profile {
    std::string uid;
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

inline std::vector<std::string> ne_headers(const std::string& cookie) {
    return {"Referer: https://music.163.com/",
            "Origin: https://music.163.com",
            "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
            "Chrome/122.0 Safari/537.36",
            "Accept: application/json, text/plain, */*",
            "Cookie: " + cookie};
}

inline std::string snippet(const std::string& body) {
    std::string s = body.substr(0, 160);
    for (char& c : s)
        if (c == '\n' || c == '\r') c = ' ';
    return s;
}

inline std::string join_artists(const js::Value& arr) {
    std::string out;
    for (size_t i = 0; i < arr.size(); ++i) {
        if (i) out += " / ";
        out += music::text::decode_entities(arr[i]["name"].str());
    }
    return out;
}

// ---------------------------------------------------------------- 账号与歌单列表

inline Profile fetch_profile(const std::string& cookie, const std::string& uid_hint, int timeout_ms = 10000) {
    Profile p;
    if (cookie.empty()) {
        p.error = "未登录";
        return p;
    }
    if (cookie_value(cookie, "MUSIC_U").empty()) {
        p.error = "Cookie 里没有 MUSIC_U（登录态不完整，重新登录一次）";
        return p;
    }

    // 账号接口顺带给出 userId 和昵称，省得从 cookie 里猜
    const net::Response acc = net::get("https://music.163.com/api/nuser/account/get", ne_headers(cookie), timeout_ms);
    if (acc.ok()) {
        js::Value root;
        if (js::parse(acc.body, root, true)) {
            const js::Value& profile = root["profile"];
            // 匿名请求也是 code 200，只是 profile 为 null —— 这才是"cookie 过期"的真实表现
            if (profile.is_null()) {
                p.error = "登录态已失效（重新登录一次）";
                return p;
            }
            p.uid = std::to_string(profile["userId"].integer());
            if (p.uid == "0") p.uid.clear();
            p.nickname = music::text::decode_entities(profile["nickname"].str());
        }
    }
    if (p.uid.empty()) p.uid = uid_hint;
    if (p.uid.empty()) {
        p.error = "取不到账号 id（登录态不完整，重新登录一次）";
        return p;
    }

    const std::string url = "https://music.163.com/api/user/playlist?uid=" + p.uid +
                            "&limit=1000&offset=0&includeVideo=true";
    const net::Response r = net::get(url, ne_headers(cookie), timeout_ms);
    if (!r.ok()) {
        p.error = "歌单接口失败（HTTP " + std::to_string(r.status) + "）";
        return p;
    }
    js::Value root;
    if (!js::parse(r.body, root, true)) {
        p.error = "歌单返回不是 JSON：" + snippet(r.body);
        return p;
    }
    const long long code = root["code"].integer(200);
    if (code != 200) {
        p.error = "歌单接口返回 code " + std::to_string(code) + "（多半是登录过期，重新登录一次）";
        return p;
    }
    const js::Value& list = root["playlist"];
    for (size_t i = 0; i < list.size(); ++i) {
        Playlist pl;
        const long long id = list[i]["id"].integer();
        if (id <= 0) continue;
        pl.id = std::to_string(id);
        pl.name = music::text::decode_entities(list[i]["name"].str());
        pl.cover = list[i]["coverImgUrl"].str();
        pl.count = int(list[i]["trackCount"].integer());
        if (pl.name.empty()) continue;
        p.playlists.push_back(std::move(pl));
    }
    if (p.playlists.empty()) p.error = "这个账号下没有歌单";
    return p;
}

// ---------------------------------------------------------------- 歌单内容

inline std::vector<music::Track> fetch_playlist(const std::string& cookie, const std::string& pid,
    std::string& err, int timeout_ms = 15000, Playlist* metadata = nullptr,
    const std::function<net::Response(const std::string&, const std::vector<std::string>&, int)>& request = net::get) {
    std::vector<music::Track> out;
    // 公开歌单（热门 / 官方榜）匿名就能读，只有"我喜欢的音乐"这类私有歌单才依赖登录态
    if (pid.empty()) {
        err = "歌单号为空";
        return out;
    }
    // n 不保证返回完整 tracks；trackIds 才是歌单顺序，缺少的资料需另行批量补齐。
    const std::string url = "https://music.163.com/api/v6/playlist/detail?id=" + pid + "&n=1000&s=0";
    const net::Response r = request(url, ne_headers(cookie), timeout_ms);
    if (!r.ok()) {
        err = "歌单详情失败（HTTP " + std::to_string(r.status) + "）";
        return out;
    }
    js::Value root;
    if (!js::parse(r.body, root, true)) {
        err = "歌单详情不是 JSON：" + snippet(r.body);
        return out;
    }
    const long long code = root["code"].integer(200);
    if (code != 200) {
        err = "歌单详情返回 code " + std::to_string(code) + "（登录过期或歌单不可见）";
        return out;
    }
    const js::Value& tracks = root["playlist"]["tracks"];
    if (metadata) {
        metadata->id = pid;
        metadata->name = music::text::decode_entities(root["playlist"]["name"].str());
        metadata->cover = root["playlist"]["coverImgUrl"].str();
        metadata->count = int(root["playlist"]["trackCount"].integer());
    }
    std::map<long long, music::Track> loaded;
    auto read_song = [&](const js::Value& s) {
        const long long id = s["id"].integer();
        if (id <= 0 || loaded.count(id)) return;
        music::Track t;
        t.title = music::text::decode_entities(s["name"].str());
        // 新旧两套字段名：v6 是 ar/al/dt，老接口是 artists/album/duration
        const js::Value& ar = s["ar"].is_array() ? s["ar"] : s["artists"];
        t.artist = join_artists(ar);
        const js::Value& al = s["al"].is_object() ? s["al"] : s["album"];
        t.album = music::text::decode_entities(al["name"].str());
        const long long dt = s["dt"].integer();
        t.duration_ms = int(dt > 0 ? dt : s["duration"].integer());
        t.cover_url = al["picUrl"].str();

        music::Variant v;
        v.source = music::Source::Netease;
        v.id = std::to_string(id);
        v.native_id = v.id;
        v.title = t.title;
        v.artist = t.artist;
        v.album = t.album;
        v.duration_ms = t.duration_ms;
        v.cover_url = t.cover_url;
        const int fee = int(s["fee"].integer(1));
        v.bitrate = 128000;
        v.needs_login = (fee == 1 || fee == 4);
        music::attach_catalog_metadata(v, s);
        t.variants.push_back(std::move(v));
        if (!t.title.empty()) loaded.emplace(id, std::move(t));
    };
    for (const auto& song : tracks.array) read_song(song);

    std::vector<long long> order, missing;
    std::set<long long> seen;
    const auto& ids = root["playlist"]["trackIds"];
    const auto& sequence = ids.is_array() && !ids.array.empty() ? ids : tracks;
    for (const auto& item : sequence.array) {
        const auto id = item["id"].integer();
        if (id <= 0 || !seen.insert(id).second) continue;
        order.push_back(id);
        if (!loaded.count(id)) missing.push_back(id);
    }
    const auto expected = root["playlist"]["trackCount"].integer();
    if (expected > static_cast<long long>(order.size())) {
        err = "网易云仅返回部分曲目 ID（" + std::to_string(order.size()) + "/" + std::to_string(expected) + "），请登录后重试";
        return {};
    }
    // 每批最多 100 首，限制 URL 和响应大小；只补缺失资料，不改变顺序或取播放地址。
    for (size_t begin = 0; begin < missing.size(); begin += 100) {
        if (net::cancelled()) { err = "歌单加载已取消"; return {}; }
        const auto end = std::min(missing.size(), begin + 100);
        std::string request_ids = "[";
        for (size_t i = begin; i < end; ++i) {
            if (i != begin) request_ids += ',';
            request_ids += std::to_string(missing[i]);
        }
        request_ids += ']';
        const auto response = request("https://music.163.com/api/song/detail?ids=" + net::url_encode(request_ids),
            ne_headers(cookie), timeout_ms);
        js::Value details;
        if (!response.ok() || !js::parse(response.body, details, true) ||
            details["code"].integer() != 200 || !details["songs"].is_array()) {
            err = "网易云歌单曲目补全失败，请重试（HTTP " + std::to_string(response.status) +
                "，code " + std::to_string(details["code"].integer()) + "）";
            return {};
        }
        for (const auto& song : details["songs"].array) read_song(song);
    }
    if (net::cancelled()) { err = "歌单加载已取消"; return {}; }
    out.reserve(order.size());
    for (const auto id : order) {
        const auto found = loaded.find(id);
        if (found != loaded.end()) out.push_back(std::move(found->second));
    }
    if (!order.empty() && out.size() != order.size()) {
        err = "网易云未返回完整曲目资料（" + std::to_string(out.size()) + "/" + std::to_string(order.size()) + "），请重试";
        return {};
    }
    if (out.empty()) err = "歌单里没有可读取的曲目";
    return out;
}

// ---------------------------------------------------------------- 写入歌单

inline bool add_tracks(const std::string& cookie, const std::string& pid, const std::vector<std::string>& ids,
                       std::string& err, int timeout_ms = 15000) {
    if (cookie.empty() || pid.empty()) {
        err = "未登录";
        return false;
    }
    if (ids.empty()) {
        err = "没有可同步的歌";
        return false;
    }
    const std::string csrf = cookie_value(cookie, "__csrf");
    if (csrf.empty()) {
        err = "Cookie 里没有 __csrf，重新登录一次";
        return false;
    }

    std::string arr = "[";
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i) arr += ",";
        arr += ids[i];
    }
    arr += "]";

    const std::string body =
        "op=add&pid=" + pid + "&trackIds=" + net::url_encode(arr) + "&csrf=" + net::url_encode(csrf);
    std::vector<std::string> headers = ne_headers(cookie);
    headers.push_back("Content-Type: application/x-www-form-urlencoded; charset=UTF-8");
    const net::Response r =
        net::post("https://music.163.com/api/playlist/manipulate/tracks", body, headers, timeout_ms);
    if (!r.ok()) {
        err = "同步请求失败（HTTP " + std::to_string(r.status) + "）";
        return false;
    }
    js::Value root;
    if (!js::parse(r.body, root, true)) {
        err = "同步返回不是 JSON：" + snippet(r.body);
        return false;
    }
    // 正常返回：{"status":200,"body":{"code":200,...}}；失败时 status/code 是 4xx/5xx
    const long long code = root["body"]["code"].integer(root["code"].integer(root["status"].integer(0)));
    const long long status = root["status"].integer(200);
    if (status == 200 && (code == 200 || code == 0)) return true;
    err = "同步被拒（code " + std::to_string(code) + "）：" + snippet(r.body);
    return false;
}

}  // namespace ne
