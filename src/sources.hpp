// sources.hpp —— 多音源适配 + 聚合
//
// 每个源一个适配器，只暴露 search / resolve 两件事；
// 聚合层负责并发扇出、超时隔离、跨源同曲合并、相关性排序。
//
// 与参考实现的差别（这份代码是 C++ 重写的，不是移植 Rust）：
//   * 没有 async 运行时，用「每个源一个 std::thread + shared_ptr 结果槽 + 9s 软超时」
//   * 取流只做一次 Range 探测（2 字节），不预先下载，也不需要回环代理
//     —— 播放走本地文件，Referer 由我们自己发，绕开了 WebView 拿不到请求头的问题
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "json.hpp"
#include "net.hpp"

namespace music {

// ---------------------------------------------------------------- 数据结构

enum class Source { Netease = 0, QQ = 1, Kuwo = 2 };
constexpr int kSourceCount = 3;

inline const char* source_name(Source s) {
    switch (s) {
        case Source::Netease: return "网易云";
        case Source::QQ: return "QQ";
        default: return "酷我";
    }
}

inline const char* source_key(Source s) {
    switch (s) {
        case Source::Netease: return "netease";
        case Source::QQ: return "qq";
        default: return "kuwo";
    }
}

struct Variant {  // 某一首歌在某个源上的一条结果
    Source source = Source::Netease;
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    int duration_ms = 0;
    int bitrate = 0;          // 源标称档位（0 = 未知）
    bool needs_login = false; // 该源自己说"要登录"，取流时会明确拒绝
    std::string cover_url;    // 该源能直接给出的封面（QQ 有 pmid，网易云要批量查）
    std::string native_id;    // 源内部用的数字 id（QQ 的 songid；同步歌单/写操作要用它）
};

struct Track {  // 合并后的"同一首歌"
    std::string title;
    std::string artist;
    std::string album;
    int duration_ms = 0;
    std::vector<Variant> variants;
    int chosen = 0;
    double score = 0.0;
    std::string cover_url;  // 合并层解析出来的封面（http 地址）

    const Variant& current() const {
        static const Variant empty;
        if (variants.empty()) return empty;
        return variants[size_t(std::min<int>(chosen, int(variants.size()) - 1))];
    }
};

struct SourceReport {
    Source source = Source::Netease;
    bool ok = false;
    int count = 0;
    int ms = 0;
    std::string err;
};

struct SearchResult {
    std::string keyword;
    std::vector<Track> tracks;
    std::vector<SourceReport> reports;
    int elapsed_ms = 0;
};

struct Settings {
    std::string netease_cookie;  // MUSIC_U=...；留空则只用免费档
    std::string qq_cookie;       // qm_keyst=...
    bool enable[kSourceCount] = {true, true, true};
    // 音质：0 = 标准 128k，1 = 高 320k，2 = 无损
    // 能到哪一档由源决定：网易云要 Cookie（无损还要会员），QQ 要登录，
    // 酷我的直链跟资源走、没有档位参数（填了也忽略）。
    int quality = 1;
    // 下载时把封面 / 歌词写进文件（mp3 走 ID3v2.3，m4a 走 ilst）
    bool embed_cover = false;
    bool embed_lyrics = false;

    bool with_netease_cookie() const { return !netease_cookie.empty(); }
};

inline const char* quality_name(int q) {
    switch (q) {
        case 0: return "标准";
        case 2: return "无损";
        default: return "高";
    }
}

inline const char* quality_short(int q) {
    switch (q) {
        case 0: return "128k";
        case 2: return "无损";
        default: return "320k";
    }
}

struct ResolveResult {
    bool ok = false;
    std::string url;
    std::vector<std::string> headers;  // 下载/探测时要带的头
    std::string reason;                // 结构化原因（失败时）
    std::string detail;                // 人话补充
};

// ---------------------------------------------------------------- 小工具

namespace text {

inline bool is_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

inline std::string lower_ascii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// 把 UTF-8 里的全角括号/标点统一成半角，方便归一化
inline std::string fold_width(const std::string& in) {
    static const struct { const char* from; const char* to; } kMap[] = {
        {"（", "("}, {"）", ")"}, {"【", "["}, {"】", "]"}, {"《", "<"}, {"》", ">"},
        {"「", "["}, {"」", "]"}, {"、", "/"}, {"，", ","}, {"—", "-"}, {"～", "~"},
        {"　", " "}, {"·", " "}, {"．", "."}, {"！", "!"}, {"？", "?"}, {"：", ":"},
    };
    std::string out = in;
    for (const auto& m : kMap) {
        const std::string from = m.from, to = m.to;
        std::string::size_type pos = 0;
        while ((pos = out.find(from, pos)) != std::string::npos) {
            out.replace(pos, from.size(), to);
            pos += to.size();
        }
    }
    return out;
}

// 归一化标题：全角折叠 → 去掉括号后缀 → 只用字母数字与中文 → 折叠空格
inline std::string normalize_title(const std::string& raw) {
    std::string s = fold_width(raw);
    int depth = 0;
    std::string kept;
    kept.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '(' || c == '[') { ++depth; continue; }
        if (c == ')' || c == ']') { if (depth) --depth; continue; }
        if (depth) continue;
        kept += char(c);
    }
    std::string out;
    bool last_space = true;
    for (unsigned char c : kept) {
        if (is_space(c)) {
            if (!last_space) out += ' ';
            last_space = true;
            continue;
        }
        if (c < 0x80) {
            const bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            if (!alnum) continue;  // 标点全部丢掉
            out += char(c);
            last_space = false;
        } else {
            out += char(c);
            last_space = false;
        }
    }
    return lower_ascii(out);
}

inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && is_space((unsigned char)s[a])) ++a;
    while (b > a && is_space((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

// 歌手串切成集合：分隔符含 "/" "," "&" "、" "feat." "ft."
inline std::vector<std::string> split_artists(const std::string& raw) {
    std::string s = fold_width(raw);
    for (const char* sep : {", ", " & ", " x ", " feat. ", " ft. ", " FEAT. ", " FT. ", "&", ",", "、"}) {
        const std::string from = sep;
        const std::string to = "/";
        if (from == to) continue;  // 自己替自己会永远命中同一处（踩过：死循环搜不出结果）
        std::string::size_type pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    }
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '/') {
            const std::string t = trim(cur);
            if (!t.empty()) out.push_back(normalize_title(t));
            cur.clear();
        } else {
            cur += c;
        }
    }
    const std::string t = trim(cur);
    if (!t.empty()) out.push_back(normalize_title(t));
    if (out.empty()) out.push_back(std::string());
    return out;
}

inline bool has_any(const std::string& hay, std::initializer_list<const char*> needles) {
    for (const char* n : needles)
        if (hay.find(n) != std::string::npos) return true;
    return false;
}

// 接口会把非断空格/引号写成 HTML 实体（网易云专辑名里 &nbsp; 特别多）。
// 另外网易云的歌手名里会出现**双重转义**的 \\u0026（解一层后仍是字面的 \u0026），
// 不处理就会把 "&" 原样显示成 "\u0026" —— 所以这里顺手再解一遍字面量。
inline std::string decode_entities(const std::string& in) {
    static const struct { const char* ent; const char* rep; } kMap[] = {
        {"&nbsp;", " "}, {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
        {"&quot;", "\""}, {"&apos;", "'"}, {"&#39;", "'"},
    };
    std::string out = in;
    for (const auto& m : kMap) {
        const std::string from = m.ent, to = m.rep;
        std::string::size_type pos = 0;
        while ((pos = out.find(from, pos)) != std::string::npos) {
            out.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    // 字面量 \uXXXX → UTF-8
    for (size_t i = 0; i + 6 <= out.size(); ++i) {
        if (out[i] != '\\' || out[i + 1] != 'u') continue;
        uint32_t cp = 0;
        bool ok = true;
        for (size_t k = 2; k < 6; ++k) {
            const char c = out[i + k];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= uint32_t(c - 'A' + 10);
            else { ok = false; break; }
        }
        if (!ok) continue;
        std::string rep;
        if (cp <= 0x7F) rep += char(cp);
        else if (cp <= 0x7FF) { rep += char(0xC0 | (cp >> 6)); rep += char(0x80 | (cp & 0x3F)); }
        else {
            rep += char(0xE0 | (cp >> 12));
            rep += char(0x80 | ((cp >> 6) & 0x3F));
            rep += char(0x80 | (cp & 0x3F));
        }
        out.replace(i, 6, rep);
        i += rep.size() - 1;
    }

    // &#12345; 形式的十进制实体
    for (size_t i = 0; i + 3 < out.size(); ++i) {
        if (out[i] != '&' || out[i + 1] != '#') continue;
        size_t j = i + 2;
        uint32_t cp = 0;
        size_t digits = 0;
        while (j < out.size() && out[j] >= '0' && out[j] <= '9' && digits < 7) {
            cp = cp * 10 + uint32_t(out[j] - '0');
            ++j;
            ++digits;
        }
        if (digits == 0 || j >= out.size() || out[j] != ';') continue;
        std::string rep;
        if (cp <= 0x7F) rep += char(cp);
        else if (cp <= 0x7FF) { rep += char(0xC0 | (cp >> 6)); rep += char(0x80 | (cp & 0x3F)); }
        else if (cp <= 0xFFFF) {
            rep += char(0xE0 | (cp >> 12));
            rep += char(0x80 | ((cp >> 6) & 0x3F));
            rep += char(0x80 | (cp & 0x3F));
        } else {
            rep += char(0xF0 | (cp >> 18));
            rep += char(0x80 | ((cp >> 12) & 0x3F));
            rep += char(0x80 | ((cp >> 6) & 0x3F));
            rep += char(0x80 | (cp & 0x3F));
        }
        out.replace(i, j + 1 - i, rep);
        i += rep.size() - 1;
    }
    return out;
}

}  // namespace text

// ---------------------------------------------------------------- 元数据清洗

// 源返回的标题常带 "xxx (Live)" / "xxx【伴奏】"，合并前先剥出"主标题"用于比较
inline std::string core_title(const std::string& raw) {
    const std::string n = text::normalize_title(raw);
    if (!n.empty()) return n;
    return text::fold_width(raw);
}

inline int duration_bucket(int ms) { return int((ms + 1500) / 3000); }

// 指纹 = 归一化标题 | 首位歌手 | 3 秒时长桶
inline std::string fingerprint(const std::string& title, const std::string& artist, int duration_ms) {
    std::string first_artist;
    const auto artists = text::split_artists(artist);
    if (!artists.empty()) first_artist = artists.front();
    return core_title(title) + "|" + first_artist + "|" + std::to_string(duration_bucket(duration_ms));
}

// 二次校验：时长差 ≤ 4s 且歌手集合有交集（指纹只用了首位歌手，这里补一层）
inline bool same_song(const Track& t, const Variant& v) {
    if (std::abs(t.duration_ms - v.duration_ms) > 4000) return false;
    const auto a = text::split_artists(t.artist);
    const auto b = text::split_artists(v.artist);
    for (const auto& x : a)
        for (const auto& y : b)
            if (!x.empty() && x == y) return true;
    return false;
}

// ---------------------------------------------------------------- 适配器：网易云

inline std::vector<Variant> search_netease(const std::string& keyword, int limit, int timeout_ms,
                                           std::string& err, const Settings& st) {
    std::vector<Variant> out;
    const std::string url = "http://music.163.com/api/search/get/web?s=" + net::url_encode(keyword) +
                            "&type=1&limit=" + std::to_string(limit) + "&offset=0";
    std::vector<std::string> headers = {"Referer: https://music.163.com/",
                                        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
                                        "Cookie: appver=2.0.2; os=pc" +
                                            (st.netease_cookie.empty() ? std::string()
                                                                       : "; " + st.netease_cookie)};

    const net::Response r = net::get(url, headers, timeout_ms);
    if (!r.ok()) {
        err = r.error.empty() ? ("HTTP " + std::to_string(r.status)) : r.error;
        return out;
    }
    js::Value root;
    if (!js::parse(r.body, root, true)) {
        err = "响应不是 JSON";
        return out;
    }
    const js::Value& songs = root["result"]["songs"];
    if (!songs.is_array()) {
        err = "接口未返回 songs（可能是风控）";
        return out;
    }
    for (size_t i = 0; i < songs.size(); ++i) {
        const js::Value& s = songs[i];
        Variant v;
        v.source = Source::Netease;
        v.id = std::to_string(s["id"].integer());
        v.title = text::decode_entities(s["name"].str());
        std::string artists;
        const js::Value& arr = s["artists"];
        for (size_t k = 0; k < arr.size(); ++k) {
            if (k) artists += "/";
            artists += text::decode_entities(arr[k]["name"].str());
        }
        v.artist = artists;
        v.album = text::decode_entities(s["album"]["name"].str());
        v.duration_ms = int(s["duration"].integer());
        const long long fee = s["fee"].integer(1);
        // fee: 0 免费 / 8 免费低音质 / 1 会员 / 4 付费专辑
        v.bitrate = (fee == 1 || fee == 4) ? 128000 : 128000;
        v.needs_login = (fee == 1 || fee == 4);
        if (v.id.empty() || v.title.empty()) continue;
        // 去掉网易云标题里自带的 "(Live)" 之类留给合并层处理，展示时保留原样
        out.push_back(std::move(v));
    }
    return out;
}

inline ResolveResult resolve_netease(const Variant& v, const Settings& st) {
    ResolveResult rr;
    std::vector<std::string> headers = {"Referer: https://music.163.com/",
                                        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
                                        "Cookie: appver=2.0.2; os=pc" +
                                            (st.netease_cookie.empty() ? std::string()
                                                                       : "; " + st.netease_cookie)};

    // 有 cookie 时先走播放地址接口（能拿到更高档），失败再回退免费外链
    if (!st.netease_cookie.empty() && st.quality > 0) {
        // br：128000 / 320000 / 999000（999000 = 无损，服务端没有就自己降档返回）
        const int want_br = st.quality >= 2 ? 999000 : 320000;
        const std::string api = "https://music.163.com/api/song/enhance/player/url?ids=%5B" + v.id +
                                "%5D&br=" + std::to_string(want_br);
        const net::Response r = net::get(api, headers, 6000);
        if (r.ok()) {
            js::Value root;
            if (js::parse(r.body, root, true)) {
                const std::string u = root["data"][size_t(0)]["url"].str();
                const long long br = root["data"][size_t(0)]["br"].integer();
                if (!u.empty() && br > 0) {
                    rr.ok = true;
                    rr.url = u;
                    rr.headers = headers;
                    rr.reason = "ok";
                    const std::string type = root["data"][size_t(0)]["type"].str();
                    rr.detail = type.empty() ? (std::to_string(br / 1000) + "kbps")
                                             : (std::to_string(br / 1000) + "kbps " + type);
                    return rr;
                }
            }
        }
    }

    // 免费外链：对 free / fee=8 的曲目直出 mp3；会员曲会跳到 404 页面
    rr.url = "https://music.163.com/song/media/outer/url?id=" + v.id + ".mp3";
    rr.headers = headers;
    const net::AudioProbe p = net::probe_audio(rr.url, headers, 6000);
    if (p.playable) {
        rr.ok = true;
        rr.reason = "ok";
        rr.detail = "免费档 128kbps";
        if (!p.final_url.empty()) rr.url = p.final_url;
        return rr;
    }
    rr.ok = false;
    rr.reason = v.needs_login ? "需要登录 / 会员" : "无版权或直链被拒";
    rr.detail = p.reason;
    return rr;
}

// ---------------------------------------------------------------- 适配器：酷我

inline std::vector<Variant> search_kuwo(const std::string& keyword, int limit, int timeout_ms,
                                        std::string& err) {
    std::vector<Variant> out;
    const std::string url =
        "http://search.kuwo.cn/r.s?all=" + net::url_encode(keyword) +
        "&ft=music&itemset=web_2013&client=kt&pn=0&rn=" + std::to_string(limit) +
        "&rformat=json&encoding=utf8&vipver=1&show_copyright_off=1&newver=1";
    const net::Response r =
        net::get(url, {"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)"}, timeout_ms);
    if (!r.ok()) {
        err = r.error.empty() ? ("HTTP " + std::to_string(r.status)) : r.error;
        return out;
    }
    js::Value root;
    if (!js::parse(r.body, root, /*lenient=*/true)) {
        err = "响应解析失败（酷我返回单引号 JSON）";
        return out;
    }
    const js::Value& list = root["abslist"];
    if (!list.is_array()) {
        err = "接口未返回 abslist";
        return out;
    }
    for (size_t i = 0; i < list.size(); ++i) {
        const js::Value& s = list[i];
        Variant v;
        v.source = Source::Kuwo;
        std::string rid = s["MUSICRID"].str();
        if (rid.rfind("MUSIC_", 0) == 0) rid = rid.substr(6);
        v.id = rid;
        v.title = text::decode_entities(s["SONGNAME"].str(s["NAME"].str()));
        v.artist = text::decode_entities(s["ARTIST"].str());
        v.album = text::decode_entities(s["ALBUM"].str());
        v.duration_ms = int(s["DURATION"].integer()) * 1000;
        // MINFO 里的档位表：取最大 mp3 码率当标称
        const std::string minfo = s["MINFO"].str();
        int best = 0;
        size_t pos = 0;
        while ((pos = minfo.find("bitrate:", pos)) != std::string::npos) {
            pos += 8;
            int br = 0;
            while (pos < minfo.size() && minfo[pos] >= '0' && minfo[pos] <= '9') br = br * 10 + (minfo[pos++] - '0');
            const size_t comma = minfo.find(',', pos);
            const std::string fmt = minfo.substr(pos, comma == std::string::npos ? 8 : comma - pos);
            if (fmt.find("mp3") != std::string::npos && br > best) best = br;
        }
        v.bitrate = best * 1000;
        if (v.id.empty() || v.title.empty() || v.id == "0") continue;
        out.push_back(std::move(v));
    }
    return out;
}

inline ResolveResult resolve_kuwo(const Variant& v) {
    ResolveResult rr;
    const std::string api =
        "http://antiserver.kuwo.cn/anti.s?type=convert_url&format=mp3&response=url&rid=MUSIC_" + v.id;
    const net::Response r = net::get(api, {"User-Agent: Mozilla/5.0"}, 6000);
    if (!r.ok()) {
        rr.reason = "取流接口失败";
        rr.detail = r.error.empty() ? ("HTTP " + std::to_string(r.status)) : r.error;
        return rr;
    }
    const std::string body = text::trim(r.body);
    if (body.rfind("http", 0) != 0) {
        rr.reason = "该曲目无可用直链";
        rr.detail = body.empty() ? "接口返回空" : body.substr(0, 64);
        return rr;
    }
    rr.url = body;
    rr.headers = {"User-Agent: Mozilla/5.0", "Referer: http://www.kuwo.cn/"};
    rr.ok = true;
    rr.reason = "ok";
    rr.detail = v.bitrate ? ("标称 " + std::to_string(v.bitrate / 1000) + "kbps") : "码率未知";
    return rr;
}

// ---------------------------------------------------------------- 适配器：QQ

inline std::vector<Variant> search_qq(const std::string& keyword, int limit, int timeout_ms,
                                      std::string& err, const Settings& st) {
    std::vector<Variant> out;
    // QQ 搜索的请求体是 musicu.fcg 的固定信封
    std::string q;
    for (unsigned char c : keyword) {
        if (c == '"' || c == '\\') { q += '\\'; q += char(c); }
        else if (c < 0x20) { q += ' '; }
        else q += char(c);
    }
    const std::string body =
        std::string("{\"comm\":{\"ct\":\"19\",\"cv\":\"1859\",\"uin\":\"0\"},") +
        "\"req\":{\"method\":\"DoSearchForQQMusicDesktop\",\"module\":\"music.search.SearchCgiService\"," +
        "\"param\":{\"grp\":1,\"num_per_page\":" + std::to_string(limit) +
        ",\"page_num\":1,\"query\":\"" + q + "\",\"search_type\":0}}}";
    std::vector<std::string> headers = {"Content-Type: application/json",
                                        "Referer: https://y.qq.com/",
                                        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)"};
    if (!st.qq_cookie.empty()) headers.push_back("Cookie: " + st.qq_cookie);

    const net::Response r = net::post("https://u.y.qq.com/cgi-bin/musicu.fcg", body, headers, timeout_ms);
    if (!r.ok()) {
        err = r.error.empty() ? ("HTTP " + std::to_string(r.status)) : r.error;
        return out;
    }
    js::Value root;
    if (!js::parse(r.body, root, true)) {
        err = "响应不是 JSON";
        return out;
    }
    if (root["req"]["code"].integer() != 0) {
        const long long code = root["req"]["code"].integer();
        // 2001 = 搜索接口要求登录态（匿名请求的固定表现），不是"没搜到"
        err = st.qq_cookie.empty() ? ("需登录（code " + std::to_string(code) + "）")
                                   : ("接口返回 code " + std::to_string(code));
        return out;
    }
    const js::Value& list = root["req"]["data"]["body"]["song"]["list"];
    if (!list.is_array() || list.size() == 0) {
        // 匿名请求会拿到空列表，登录页地址就在 feedbackURL 里 —— 这是"需登录"，不是"没结果"
        err = st.qq_cookie.empty() ? "需登录（匿名搜索被拒）" : "无结果";
        return out;
    }
    for (size_t i = 0; i < list.size(); ++i) {
        const js::Value& s = list[i];
        Variant v;
        v.source = Source::QQ;
        v.id = s["mid"].str(s["songmid"].str());
        v.title = text::decode_entities(s["name"].str(s["songname"].str()));
        std::string artists;
        const js::Value& singers = s["singer"];
        for (size_t k = 0; k < singers.size(); ++k) {
            if (k) artists += "/";
            artists += text::decode_entities(singers[k]["name"].str());
        }
        v.artist = artists;
        v.album = text::decode_entities(s["album"]["name"].str(s["albumname"].str()));
        // QQ 的专辑封面有稳定的直链模板，pmid 在搜索结果里就有，不用再请求
        const std::string pmid = s["album"]["pmid"].str();
        if (!pmid.empty())
            v.cover_url = "https://y.gtimg.cn/music/photo_new/T002R300x300M000" + pmid + ".jpg";
        v.duration_ms = int(s["interval"].integer()) * 1000;
        const long long pay_play = s["pay"]["pay_play"].integer(1);
        v.needs_login = pay_play != 0;
        v.bitrate = 128000;
        if (v.id.empty() || v.title.empty()) continue;
        out.push_back(std::move(v));
    }
    return out;
}

// 从 cookie 里取一个键（QQ 的 uin / authst 都靠它）
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

namespace detail {

// musicu.fcg 的统一信封：uin + authst 必须**同时**出现在 comm 里，
// 只带 cookie 不带这两个字段，服务端会回 200 但 purl 全是空串 ——
// 之前的实现就是这么写的，所以 QQ 从来没真正放出过声。
inline std::string musicu_body(const std::string& uin, const std::string& authst, const std::string& req) {
    return "{\"loginUin\":\"" + uin + "\",\"comm\":{\"format\":\"json\",\"ct\":24,\"cv\":0,"
           "\"tmeAppID\":\"qqmusic\",\"uin\":\"" + uin + "\",\"qq\":\"" + uin + "\","
           "\"authst\":\"" + authst + "\"}," + req + "}";
}

inline js::Value musicu_call(const std::string& cookie, const std::string& body, int timeout_ms, bool& ok) {
    js::Value root;
    const net::Response r = net::post("https://u.y.qq.com/cgi-bin/musicu.fcg", body,
                                      {"Content-Type: application/json", "Referer: https://y.qq.com/",
                                       "Origin: https://y.qq.com", "Cookie: " + cookie},
                                      timeout_ms);
    if (!r.ok()) {
        ok = false;
        return root;
    }
    ok = js::parse(r.body, root, true);
    return root;
}

}  // namespace detail

inline ResolveResult resolve_qq(const Variant& v, const Settings& st) {
    ResolveResult rr;
    if (st.qq_cookie.empty()) {
        rr.reason = "需要登录";
        rr.detail = "QQ 音乐匿名取流被后端拒绝（104009），填入 qm_keyst 后才能取流";
        return rr;
    }
    std::string uin = cookie_value(st.qq_cookie, "uin");
    if (uin.empty()) uin = cookie_value(st.qq_cookie, "p_uin");
    if (uin.empty()) uin = cookie_value(st.qq_cookie, "wxuin");
    while (!uin.empty() && (uin[0] == 'o' || uin[0] == 'U' || uin[0] == 'u' || uin[0] == '_')) uin.erase(0, 1);
    std::string authst = cookie_value(st.qq_cookie, "authst");
    if (authst.empty()) authst = cookie_value(st.qq_cookie, "qm_keyst");
    if (authst.empty()) authst = cookie_value(st.qq_cookie, "qqmusic_key");
    if (authst.empty()) authst = cookie_value(st.qq_cookie, "musickey");
    if (uin.empty() || authst.empty()) {
        rr.reason = "登录态不完整";
        rr.detail = "cookie 里缺少 uin 或 qm_keyst";
        return rr;
    }

    // 文件名拼法：<档位前缀><songmid><media_mid>.<ext>。media_mid 跟 songmid 经常不是一回事，
    // 拿 songmid 顶上会被回 101404；所以先按 songmid 试一档，不行再去查详情换 media_mid。
    auto filenames_for = [&](const std::string& media) {
        const std::string mid = v.id;
        const std::string tail = mid + media;
        std::vector<std::string> out;
        if (st.quality >= 2) {
            out.push_back("F000" + tail + ".flac");
            out.push_back("F0M0" + media + ".mflac");
        }
        if (st.quality >= 1) out.push_back("M800" + tail + ".mp3");
        out.push_back("M500" + tail + ".mp3");
        // 兜底：有些曲目只有 C400（96k AAC/m4a），连 128k mp3 都不给
        out.push_back("C400" + tail + ".m4a");
        return out;
    };

    auto try_files = [&](const std::string& media, std::string& url, std::string& used) -> bool {
        const std::vector<std::string> files = filenames_for(media);
        std::string list, mids, types;
        for (size_t i = 0; i < files.size(); ++i) {
            if (i) {
                list += ",";
                mids += ",";
                types += ",";
            }
            list += "\"" + files[i] + "\"";
            mids += "\"" + v.id + "\"";  // songmid 与 filename 一一对应
            types += "0";
        }
        const std::string req = "\"vkey.GetVkeyServer.CgiGetVkey\":{\"module\":\"vkey.GetVkeyServer\","
                                "\"method\":\"CgiGetVkey\",\"param\":{\"guid\":\"10000\","
                                "\"songmid\":[" + mids + "],\"songtype\":[" + types + "],"
                                "\"uin\":\"" + uin + "\",\"loginflag\":1,\"platform\":\"20\","
                                "\"filename\":[" + list + "]}}";
        bool ok = false;
        const js::Value root = detail::musicu_call(st.qq_cookie, detail::musicu_body(uin, authst, req), 8000, ok);
        if (!ok) return false;
        const js::Value& data = root["vkey.GetVkeyServer.CgiGetVkey"]["data"];
        const js::Value& infos = data["midurlinfo"];
        for (size_t i = 0; i < infos.size(); ++i) {
            const std::string p = infos[i]["purl"].str();
            if (p.empty()) continue;
            const std::string sip = data["sip"][size_t(0)].str();
            url = (sip.empty() ? std::string("http://ws.stream.qqmusic.qq.com/") : sip) + p;
            used = infos[i]["filename"].str();
            return true;
        }
        return false;
    };

    std::string url, used;
    if (!try_files(v.id, url, used)) {
        // 换 media_mid 再试一次：查一次歌曲详情就够，代价是一次请求
        const std::string req = "\"songinfo\":{\"module\":\"music.pf_song_detail_svr\","
                                "\"method\":\"get_song_detail_yqq\",\"param\":{\"song_type\":0,"
                                "\"song_mid\":\"" + v.id + "\"}}";
        bool ok = false;
        const js::Value root = detail::musicu_call(st.qq_cookie, detail::musicu_body(uin, authst, req), 8000, ok);
        if (!ok) {
            rr.reason = "取流接口失败";
            return rr;
        }
        const std::string media = root["songinfo"]["data"]["track_info"]["file"]["media_mid"].str();
        if (!media.empty() && media != v.id && !try_files(media, url, used)) {
            rr.reason = "该曲目无可用直链";
            rr.detail = "登录态下仍无 purl（可能会员专享或版权受限）";
            return rr;
        }
        if (media.empty() || media == v.id) {
            rr.reason = "该曲目无可用直链";
            rr.detail = "登录态下仍无 purl（可能会员专享或版权受限）";
            return rr;
        }
    }

    rr.url = url;
    rr.headers = {"User-Agent: Mozilla/5.0", "Referer: https://y.qq.com/"};
    rr.ok = true;
    rr.reason = "ok";
    if (!used.empty() && used.size() > 4) {
        const std::string ext = used.substr(used.size() - 5);
        std::string q = "128k";
        if (used.rfind("F000", 0) == 0 || used.rfind("F0M0", 0) == 0) q = "无损";
        else if (used.rfind("M800", 0) == 0) q = "320k";
        rr.detail = q + " · " + (ext == ".flac" ? "flac" : (ext == ".mflac" ? "mflac" : "mp3"));
    } else {
        rr.detail = "登录态取流";
    }
    return rr;
}

// ---------------------------------------------------------------- 聚合

namespace detail {

inline double relevance(const Track& t, const std::string& kw_norm) {
    double best = 0;
    for (const auto& v : t.variants) {
        const std::string n = core_title(v.title);
        double s = 10;
        if (n == kw_norm) s = 100;
        else if (!n.empty() && n.compare(0, std::min(n.size(), kw_norm.size()), kw_norm) == 0) s = 70;
        else if (n.find(kw_norm) != std::string::npos) s = 45;
        else if (kw_norm.find(n) != std::string::npos) s = 30;
        if (s > best) best = s;
    }
    // 多源都命中 = 更可信
    best += 18.0 * double(t.variants.size() - 1);
    // 至少有一个源不需要登录 ≈ 点了就能听，给它一点加权（小到不会压过标题匹配）
    for (const auto& v : t.variants)
        if (!v.needs_login) { best += 6; break; }
    if (!t.album.empty()) best += 2;
    // 搜歌手名时，"这位歌手的歌"比"标题里恰好含这个词"更相关
    const std::string kw = kw_norm;
    for (const auto& v : t.variants) {
        const std::string ar = core_title(v.artist);
        if (!kw.empty() && ar.find(kw) != std::string::npos) { best += 40; break; }
    }

    const std::string hay = core_title(t.title);
    if (text::has_any(hay, {"cover", "banzou", "karaoke", "instrumental", "remix", "dj"}))
        best *= 0.55;
    if (text::has_any(hay, {"live", "xianchang", "演唱会"})) best *= 0.7;
    // 现场版/翻唱常来自标题里的中文标注，这里用小写后的原文再判一次
    const std::string raw = text::lower_ascii(text::fold_width(t.title));
    if (text::has_any(raw, {"伴奏", "翻唱", "现场", "片段", "试听"})) best *= 0.55;
    if (t.duration_ms < 30000 || t.duration_ms > 12 * 60 * 1000) best *= 0.4;
    return best;
}

inline void merge_into(std::vector<Track>& tracks, std::map<std::string, size_t>& index, Variant v) {
    const std::string fp = fingerprint(v.title, v.artist, v.duration_ms);
    auto it = index.find(fp);
    if (it != index.end()) {
        Track& t = tracks[it->second];
        if (same_song(t, v)) {
            for (const auto& existing : t.variants) {
                if (existing.source != v.source) continue;
                // 同一个源只留一条：接口常把同曲的不同合辑/版本各返回一次，
                // 全塞进来会让一行里出现两枚一模一样的来源药丸
                if (existing.id == v.id) return;
                if (double(v.bitrate) <= double(existing.bitrate)) return;
            }
            t.variants.push_back(std::move(v));
            return;
        }
    }
    Track t;
    t.title = v.title;
    t.artist = v.artist;
    t.album = v.album;
    t.duration_ms = v.duration_ms;
    t.variants.push_back(std::move(v));
    tracks.push_back(std::move(t));
    index[fp] = tracks.size() - 1;
}

// 来源轮转交错：避免单源霸屏
inline std::vector<Variant> interleave(std::vector<std::vector<Variant>> per_source) {
    std::vector<Variant> out;
    size_t total = 0;
    for (const auto& v : per_source) total += v.size();
    out.reserve(total);
    size_t cursor[kSourceCount] = {0, 0, 0};
    bool moved = true;
    while (moved) {
        moved = false;
        for (int s = 0; s < kSourceCount; ++s) {
            if (s >= int(per_source.size())) continue;
            if (cursor[s] < per_source[size_t(s)].size()) {
                out.push_back(per_source[size_t(s)][cursor[s]++]);
                moved = true;
            }
        }
    }
    return out;
}

}  // namespace detail

struct Job {
    std::mutex m;
    std::condition_variable cv;
    int done = 0;
    std::vector<char> finished;
    std::vector<std::vector<Variant>> per_source;
    std::vector<SourceReport> reports;
    bool cancelled = false;
};

// 并发检索：每个源一个线程，9 秒软超时；超时/失败的源降级成 SourceReport 里的红字
inline SearchResult run_search(const std::string& keyword, const Settings& st, int limit_per_source = 12,
                               int soft_timeout_ms = 9000) {
    using clock = std::chrono::steady_clock;
    SearchResult result;
    result.keyword = keyword;
    const auto t_start = clock::now();

    auto job = std::make_shared<Job>();
    job->per_source.resize(kSourceCount);
    job->reports.resize(kSourceCount);
    job->finished.assign(kSourceCount, 0);
    for (int s = 0; s < kSourceCount; ++s) {
        job->reports[size_t(s)].source = Source(s);
        job->reports[size_t(s)].ok = false;
        job->reports[size_t(s)].err = st.enable[s] ? "" : "已关闭";
    }

    auto launch = [job, keyword, st, limit_per_source](Source src) {
        const int idx = int(src);
        const auto t0 = clock::now();
        std::string err;
        std::vector<Variant> got;
        try {
            if (src == Source::Netease) got = search_netease(keyword, limit_per_source, 8000, err, st);
            else if (src == Source::Kuwo) got = search_kuwo(keyword, limit_per_source, 8000, err);
            else got = search_qq(keyword, limit_per_source, 8000, err, st);
        } catch (...) {
            err = "适配器异常";
        }
        const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count());
        std::lock_guard<std::mutex> lk(job->m);
        job->per_source[size_t(idx)] = std::move(got);
        SourceReport& rep = job->reports[size_t(idx)];
        rep.count = int(job->per_source[size_t(idx)].size());
        rep.ms = ms;
        rep.ok = !(rep.count == 0 && !err.empty());
        rep.err = rep.count > 0 ? std::string() : err;
        job->finished[size_t(idx)] = 1;
        ++job->done;
        job->cv.notify_all();
    };

    int launched = 0;
    for (int s = 0; s < kSourceCount; ++s) {
        if (!st.enable[s]) continue;
        std::thread(launch, Source(s)).detach();
        ++launched;
    }

    {
        std::unique_lock<std::mutex> lk(job->m);
        job->cv.wait_for(lk, std::chrono::milliseconds(soft_timeout_ms),
                         [&] { return job->done >= launched; });
        for (int s = 0; s < kSourceCount; ++s) {
            if (!st.enable[s] || job->finished[size_t(s)]) continue;
            SourceReport& rep = job->reports[size_t(s)];
            rep.ok = false;
            rep.err = "超时 >" + std::to_string(soft_timeout_ms) + "ms";
        }
    }

    std::vector<std::vector<Variant>> per_source;
    per_source.resize(kSourceCount);
    {
        std::lock_guard<std::mutex> lk(job->m);
        per_source = job->per_source;
        result.reports = job->reports;
    }

    // 合并 → 排序
    const std::string kw_norm = core_title(keyword);
    std::vector<Track> tracks;
    std::map<std::string, size_t> index;
    for (const Variant& v : detail::interleave(per_source)) detail::merge_into(tracks, index, v);
    for (Track& t : tracks) {
        t.score = detail::relevance(t, kw_norm);
        // 默认源优先挑不需要登录的：网易云的会员曲会被直接拒，
        // 让用户点一行就听得到，比"先失败再教他换源"友好得多
        t.chosen = 0;
        for (int i = 0; i < int(t.variants.size()); ++i) {
            if (!t.variants[size_t(i)].needs_login) { t.chosen = i; break; }
        }
    }
    std::stable_sort(tracks.begin(), tracks.end(), [](const Track& a, const Track& b) {
        if (std::abs(a.score - b.score) > 0.01) return a.score > b.score;
        // 同分时：多源命中 > 单源；再同分时长的优先 —— 实测按"短的优先"会把
        // 两分钟的翻唱/片段排到四分钟的原曲前面，那不像搜索结果的直觉。
        if (a.variants.size() != b.variants.size()) return a.variants.size() > b.variants.size();
        return a.duration_ms > b.duration_ms;
    });
    result.tracks = std::move(tracks);
    result.elapsed_ms =
        int(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t_start).count());
    return result;
}

// ---------------------------------------------------------------- 封面

// 酷我：pic.web 返回的是一行纯文本 URL（不是 302），要再取一次
inline std::string kuwo_cover_url(const std::string& rid) {
    const std::string api = "http://artistpicserver.kuwo.cn/pic.web?corp=kuwo&type=rid_pic&pictype=500"
                            "&size=500&rid=" + rid;
    const net::Response r = net::get(api, {"User-Agent: Mozilla/5.0"}, 6000);
    if (!r.ok()) return std::string();
    const std::string body = text::trim(r.body);
    if (body.rfind("http", 0) != 0) return std::string();
    return body;
}

// 给合并后的曲目补封面：
//   1. 有 QQ 变体的，pmid 直链直接用（零请求）
//   2. 其余用网易云的 song/detail 批量查（一次请求覆盖所有网易云 id）
//   3. 还没有的，按酷我 rid 问一次 pic.web（最贵，限流到前 20 首）
// 全失败就保持空，界面回落到"程序化渐变封面"。
inline void fill_covers(std::vector<Track>& tracks, const Settings& st, int kuwo_budget = 20) {
    std::vector<std::string> netease_ids;
    for (Track& t : tracks) {
        for (const auto& v : t.variants) {
            if (!v.cover_url.empty()) { t.cover_url = v.cover_url; break; }
        }
        if (!t.cover_url.empty()) continue;
        for (const auto& v : t.variants) {
            if (v.source == Source::Netease && !v.id.empty()) { netease_ids.push_back(v.id); break; }
        }
    }

    if (!netease_ids.empty()) {
        std::string ids = "%5B";
        for (size_t i = 0; i < netease_ids.size(); ++i) {
            if (i) ids += "%2C";
            ids += netease_ids[i];
        }
        ids += "%5D";
        std::vector<std::string> headers = {"Referer: https://music.163.com/",
                                            "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
                                            "Cookie: appver=2.0.2; os=pc"};
        if (!st.netease_cookie.empty()) headers.push_back("Cookie: " + st.netease_cookie);
        const net::Response r =
            net::get("https://music.163.com/api/song/detail?ids=" + ids, headers, 8000);
        if (r.ok()) {
            js::Value root;
            if (js::parse(r.body, root, true)) {
                std::map<std::string, std::string> pic;  // id → picUrl
                const js::Value& songs = root["songs"];
                for (size_t i = 0; i < songs.size(); ++i) {
                    const std::string id = std::to_string(songs[i]["id"].integer());
                    const std::string url = songs[i]["album"]["picUrl"].str();
                    if (!id.empty() && !url.empty()) pic[id] = url;
                }
                for (Track& t : tracks) {
                    if (!t.cover_url.empty()) continue;
                    for (const auto& v : t.variants) {
                        if (v.source != Source::Netease) continue;
                        auto it = pic.find(v.id);
                        if (it != pic.end()) { t.cover_url = it->second; break; }
                    }
                }
            }
        }
    }

    for (Track& t : tracks) {
        if (!t.cover_url.empty() || kuwo_budget <= 0) continue;
        for (const auto& v : t.variants) {
            if (v.source != Source::Kuwo) continue;
            const std::string url = kuwo_cover_url(v.id);
            if (!url.empty()) {
                t.cover_url = url;
                --kuwo_budget;
            }
            break;
        }
    }
}

// 取流：按当前选中源解析，失败就沿"其它源"换源
inline ResolveResult resolve(const Variant& v, const Settings& st) {
    switch (v.source) {
        case Source::Netease: return resolve_netease(v, st);
        case Source::Kuwo: return resolve_kuwo(v);
        default: return resolve_qq(v, st);
    }
}

// ---------------------------------------------------------------- 跨源补位
//
// 歌单里的曲目通常只有本源那一条 id（QQ 歌单里全是 QQ mid）。这条取不到流时，
// 在列表内部换源是没有出路的 —— 必须真的按「歌名 + 歌手」去别的源搜一遍。
// 判据和歌词反查一致：标题要能对上，**时长差**是主要过滤器（同名翻唱 / 现场版）。
inline std::string main_artist(const std::string& artist) {
    size_t cut = artist.find(" / ");
    if (cut == std::string::npos) cut = artist.find('/');
    if (cut == std::string::npos) cut = artist.find('&');
    return text::trim(cut == std::string::npos ? artist : artist.substr(0, cut));
}

inline bool find_on_source(Source src, const Track& t, const Settings& st, Variant& out, std::string& err) {
    if (t.title.empty()) {
        err.clear();
        return false;
    }
    const std::string kw = t.title + " " + main_artist(t.artist);
    std::vector<Variant> got;
    if (src == Source::Netease) got = search_netease(kw, 10, 6000, err, st);
    else if (src == Source::QQ) got = search_qq(kw, 10, 6000, err, st);
    else got = search_kuwo(kw, 10, 6000, err);

    if (got.empty()) {
        if (err.empty()) err = std::string(source_name(src)) + " 没搜到";
        return false;
    }

    const std::string want_raw = text::trim(t.title);
    const std::string want = text::normalize_title(t.title);
    const std::string want_artist = text::normalize_title(main_artist(t.artist));
    double best = -1e9;
    int best_i = -1;
    for (int i = 0; i < int(got.size()); ++i) {
        const Variant& v = got[i];
        const std::string have_raw = text::trim(v.title);
        const std::string have = text::normalize_title(v.title);
        double s = 0.0;
        if (have_raw == want_raw) s += 60.0;
        else if (have == want) s += 20.0;
        else if (have.find(want) != std::string::npos || want.find(have) != std::string::npos) s += 6.0;
        else continue;

        if (t.duration_ms > 0 && v.duration_ms > 0) {
            const double diff = double(v.duration_ms > t.duration_ms ? v.duration_ms - t.duration_ms
                                                                     : t.duration_ms - v.duration_ms) /
                                1000.0;
            if (diff > 20.0) continue;
            s += 30.0 - diff;
        }
        if (!want_artist.empty() && text::normalize_title(v.artist).find(want_artist) != std::string::npos)
            s += 20.0;
        if (!v.needs_login) s += 6.0;  // 不用会员的那条优先：点了就能听
        if (s > best) {
            best = s;
            best_i = i;
        }
    }
    if (best_i < 0 || best < 20.0) {
        err = std::string(source_name(src)) + " 没有对得上的版本";
        return false;
    }
    out = got[size_t(best_i)];
    err.clear();
    return true;
}

}  // namespace music
