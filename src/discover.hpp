// discover.hpp —— 发现页的数据来源：热搜词 + 热门歌单
//
// 三家平台的接口情况（都是实测，不是照抄文档）：
//   网易云  热搜 /api/search/hot?type=1111     → result.hots[].first
//           歌单 /api/playlist/list?cat=全部&order=hot → playlists[]
//           官方榜 /api/toplist                → list[]（id 就是歌单 id，可以直接打开）
//   QQ      热搜 c.y.qq.com/splcloud/fcgi-bin/gethotkey.fcg → data.hotkey[].k
//           歌单 c.y.qq.com/splcloud/fcgi-bin/fcg_get_diss_by_tag.fcg?sortId=5 → data.list[]
//   酷我     热搜 hotword.kuwo.cn/hotword.s?...  → 纯文本，每行一个词
//           歌单 **没有**：web 端 api/www 要 kw_token + csrf 双校验，匿名一律
//                "The request is illegal!"，老 qukudata 域名已经 404。
//                所以酷我这一栏只有热搜，卡片区直接说明原因，不装作有。
#pragma once

#include <string>
#include <vector>

#include "json.hpp"
#include "net.hpp"
#include "sources.hpp"

namespace disc {

struct Word {
    std::string text;
    long long heat = 0;  // 只有 QQ 给热度数字，其余是 0
};

struct Heats {
    std::vector<Word> words;
    std::string err;
};

struct Card {
    music::Source source = music::Source::Netease;
    std::string id;   // 网易云 playlist id / QQ disstid
    std::string name;
    std::string cover;
    std::string creator;
    long long plays = 0;
    int count = 0;
    bool official = false;  // 官方排行榜（网易云那种）
};

inline std::string brief(const std::string& body) {
    std::string s = body.substr(0, 140);
    for (char& c : s)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return s;
}

inline std::vector<std::string> headers_of(music::Source s, bool with_json) {
    std::vector<std::string> h;
    h.push_back("User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                "Chrome/122.0 Safari/537.36");
    switch (s) {
        case music::Source::Netease:
            h.push_back("Referer: https://music.163.com/");
            break;
        case music::Source::QQ:
            h.push_back("Referer: https://y.qq.com/");
            break;
        default:
            h.push_back("Referer: https://www.kuwo.cn/");
            break;
    }
    if (with_json) h.push_back("Accept: application/json, text/plain, */*");
    return h;
}

// 播放量：接口给的是 8558024 这种原始数，界面上要看 855.8 万
inline std::string human_count(long long n) {
    char buf[48];
    if (n >= 100000000LL)
        std::snprintf(buf, sizeof buf, "%.1f 亿", double(n) / 100000000.0);
    else if (n >= 10000)
        std::snprintf(buf, sizeof buf, "%.1f 万", double(n) / 10000.0);
    else
        std::snprintf(buf, sizeof buf, "%lld", n);
    return buf;
}

// ---------------------------------------------------------------- 热搜

inline Heats heats_netease(int timeout_ms) {
    Heats h;
    const net::Response r =
        net::get("https://music.163.com/api/search/hot?type=1111", headers_of(music::Source::Netease, true), timeout_ms);
    if (!r.ok()) {
        h.err = "热搜接口失败（HTTP " + std::to_string(r.status) + "）";
        return h;
    }
    js::Value root;
    if (!js::parse(r.body, root, true)) {
        h.err = "热搜返回不是 JSON：" + brief(r.body);
        return h;
    }
    const js::Value& hots = root["result"]["hots"];
    for (size_t i = 0; i < hots.size(); ++i) {
        Word w;
        w.text = music::text::decode_entities(hots[i]["first"].str());
        if (w.text.empty()) continue;
        h.words.push_back(std::move(w));
    }
    if (h.words.empty()) h.err = "热搜是空的（接口结构可能又变了）";
    return h;
}

inline Heats heats_qq(int timeout_ms) {
    Heats h;
    const net::Response r = net::get("https://c.y.qq.com/splcloud/fcgi-bin/gethotkey.fcg?format=json",
                                     headers_of(music::Source::QQ, true), timeout_ms);
    if (!r.ok()) {
        h.err = "热搜接口失败（HTTP " + std::to_string(r.status) + "）";
        return h;
    }
    js::Value root;
    if (!js::parse(r.body, root, true)) {
        h.err = "热搜返回不是 JSON：" + brief(r.body);
        return h;
    }
    const js::Value& list = root["data"]["hotkey"];
    for (size_t i = 0; i < list.size(); ++i) {
        Word w;
        w.text = list[i]["k"].str();
        while (!w.text.empty() && (w.text.back() == ' ' || w.text.back() == '\t')) w.text.pop_back();
        w.heat = list[i]["n"].integer();
        if (w.text.empty()) continue;
        h.words.push_back(std::move(w));
    }
    if (h.words.empty()) h.err = "热搜是空的";
    return h;
}

inline Heats heats(music::Source s, int timeout_ms = 8000) {
    switch (s) {
        case music::Source::QQ: return heats_qq(timeout_ms);
        default: return heats_netease(timeout_ms);
    }
}

// ---------------------------------------------------------------- 热门歌单

inline std::vector<Card> playlists_netease(int limit, std::string& err, int timeout_ms) {
    std::vector<Card> out;

    // 官方榜单独占前几张卡：这种卡点进去就是整张榜，比分页翻歌单更实用
    const net::Response top = net::get("https://music.163.com/api/toplist", headers_of(music::Source::Netease, true),
                                       timeout_ms);
    if (top.ok()) {
        js::Value root;
        if (js::parse(top.body, root, true)) {
            const js::Value& list = root["list"];
            for (size_t i = 0; i < list.size() && int(out.size()) < 3; ++i) {
                Card c;
                c.source = music::Source::Netease;
                c.id = std::to_string(list[i]["id"].integer());
                c.name = music::text::decode_entities(list[i]["name"].str());
                c.cover = list[i]["coverImgUrl"].str();
                c.official = true;
                if (c.id == "0" || c.name.empty()) continue;
                out.push_back(std::move(c));
            }
        }
    }

    const std::string url = "https://music.163.com/api/playlist/list?cat=" + net::url_encode("全部") +
                            "&order=hot&offset=0&limit=" + std::to_string(limit) + "&total=true";
    const net::Response r = net::get(url, headers_of(music::Source::Netease, true), timeout_ms);
    if (!r.ok()) {
        err = "歌单接口失败（HTTP " + std::to_string(r.status) + "）";
        return out;
    }
    js::Value root;
    if (!js::parse(r.body, root, true)) {
        err = "歌单返回不是 JSON：" + brief(r.body);
        return out;
    }
    const js::Value& list = root["playlists"];
    for (size_t i = 0; i < list.size(); ++i) {
        Card c;
        c.source = music::Source::Netease;
        c.id = std::to_string(list[i]["id"].integer());
        c.name = music::text::decode_entities(list[i]["name"].str());
        c.cover = list[i]["coverImgUrl"].str(list[i]["picUrl"].str());
        c.creator = music::text::decode_entities(list[i]["creator"]["nickname"].str());
        c.plays = list[i]["playCount"].integer();
        if (c.plays == 0) c.plays = list[i]["subscribedCount"].integer();
        c.count = int(list[i]["trackCount"].integer());
        if (c.id == "0" || c.name.empty() || c.cover.empty()) continue;
        out.push_back(std::move(c));
    }
    if (out.empty()) err = "歌单列表是空的（接口结构可能又变了）";
    return out;
}

inline std::vector<Card> playlists_qq(int limit, std::string& err, int timeout_ms) {
    std::vector<Card> out;
    // sortId=5 是"最热"，categoryId=10000000 是"全部"。匿名可访问。
    const std::string url = "https://c.y.qq.com/splcloud/fcgi-bin/fcg_get_diss_by_tag.fcg?picmid=1&rnd=0.1"
                            "&g_tk=5381&loginUin=0&hostUin=0&format=json&inCharset=utf8&outCharset=utf-8"
                            "&notice=0&platform=yqq.json&needNewCode=0&categoryId=10000000&sortId=5&sin=0&ein=" +
                            std::to_string(limit);
    const net::Response r = net::get(url, headers_of(music::Source::QQ, true), timeout_ms);
    if (!r.ok()) {
        err = "歌单接口失败（HTTP " + std::to_string(r.status) + "）";
        return out;
    }
    js::Value root;
    if (!js::parse(r.body, root, true)) {
        err = "歌单返回不是 JSON：" + brief(r.body);
        return out;
    }
    if (root["code"].integer() != 0) {
        err = "歌单接口返回 code " + std::to_string(root["code"].integer());
        return out;
    }
    const js::Value& list = root["data"]["list"];
    for (size_t i = 0; i < list.size(); ++i) {
        Card c;
        c.source = music::Source::QQ;
        c.id = std::to_string(list[i]["dissid"].integer());
        if (c.id == "0") c.id = list[i]["dissid"].str();
        c.name = music::text::decode_entities(list[i]["dissname"].str());
        c.cover = list[i]["imgurl"].str();
        c.creator = music::text::decode_entities(list[i]["creator"]["name"].str());
        c.plays = list[i]["listennum"].integer();
        // 这个接口不返回曲目数，只能等打开歌单时才知道
        if (c.id == "0" || c.name.empty() || c.cover.empty()) continue;
        out.push_back(std::move(c));
    }
    if (out.empty()) err = "歌单列表是空的（接口结构可能又变了）";
    return out;
}

inline std::vector<Card> playlists(music::Source s, int limit, std::string& err, int timeout_ms = 10000) {
    switch (s) {
        case music::Source::QQ: return playlists_qq(limit, err, timeout_ms);
        default: return playlists_netease(limit, err, timeout_ms);
    }
}

}  // namespace disc
