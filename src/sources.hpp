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
#include "platform.hpp"
#include <cstring>
#include <ctime>
#include <functional>
#include <fstream>
#include <set>

namespace music {

// ---------------------------------------------------------------- 数据结构

enum class Source { Netease = 0, QQ = 1, Kuwo = 2, YouTube = 3, Bilibili = 4, Local = 5 };
constexpr int kSourceCount = 5;

inline const char* source_name(Source s) {
    switch (s) {
        case Source::Netease: return "网易云";
        case Source::QQ: return "QQ";
        case Source::Kuwo: return "酷我";
        case Source::YouTube: return "YouTube";
        case Source::Bilibili: return "B 站";
        case Source::Local: return "本地";
        default: return "未知";
    }
}

inline const char* source_key(Source s) {
    switch (s) {
        case Source::Netease: return "netease";
        case Source::QQ: return "qq";
        case Source::Kuwo: return "kuwo";
        case Source::YouTube: return "youtube";
        case Source::Bilibili: return "bilibili";
        case Source::Local: return "local";
        default: return "unknown";
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
    bool enable[kSourceCount] = {true, true, true, false, false};
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

inline std::vector<Variant> search_bilibili(const std::string&, int, int, std::string&);
inline std::vector<Variant> search_youtube(const std::string&, int, int, std::string&);

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
        "https://search.kuwo.cn/r.s?all=" + net::url_encode(keyword) +
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
    rr.headers = {"User-Agent: Mozilla/5.0", "Referer: https://www.kuwo.cn/"};
    const auto response = net::get("https://antiserver.kuwo.cn/anti.s?type=convert_url3&format=mp3&br=320kmp3&rid=MUSIC_" +
        net::url_encode(v.id), rr.headers, 5000);
    js::Value root;
    if (response.ok() && js::parse(response.body, root)) rr.url = root["url"].str();
    if (rr.url.rfind("https://", 0) != 0 && rr.url.rfind("http://", 0) != 0) {
        const auto fallback = net::get("https://antiserver.kuwo.cn/anti.s?type=convert_url&format=mp3&response=url&rid=MUSIC_" +
            net::url_encode(v.id), rr.headers, 5000);
        rr.url = text::trim(fallback.body);
    }
    rr.ok = rr.url.rfind("https://", 0) == 0 || rr.url.rfind("http://", 0) == 0;
    rr.reason = rr.ok ? "ok" : "酷我未返回可用直链";
    rr.detail = rr.ok ? "实际音质以音频为准，受限曲目可能仅试听" : "平台版权、网络或取流限制";
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
            else if (src == Source::QQ) got = search_qq(keyword, limit_per_source, 8000, err, st);
            else if (src == Source::YouTube) got = search_youtube(keyword, limit_per_source, 8000, err);
            else if (src == Source::Bilibili) got = search_bilibili(keyword, limit_per_source, 8000, err);
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

// 新适配器复用 net/js/platform；不依赖参考工程的 Windows 专属网络层。
namespace providers {
inline bool http_url(const std::string& url) { return url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0; }
inline std::string quote(const std::string& input) {
    std::string out = "\"";
    for (unsigned char c : input) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 32) out += ' '; else out += char(c);
    }
    return out + '"';
}
inline bool json_response(const net::Response& r, js::Value& root, std::string& error) {
    if (!r.ok()) { error = r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error; return false; }
    if (r.body.size() > 8 * 1024 * 1024 || !js::parse(r.body, root) || !root.is_object()) {
        error = "接口返回非 JSON（可能需要验证）"; return false;
    }
    return true;
}
inline int clock_ms(const std::string& text) {
    if (text.empty() || text.find(':') == std::string::npos || text.size() > 12) return 0;
    long long total = 0, part = 0;
    for (char c : text) {
        if (c == ':') { total = total * 60 + part; part = 0; }
        else if (c >= '0' && c <= '9') part = part * 10 + c - '0';
        else return 0;
    }
    const auto seconds = total * 60 + part;
    return seconds <= 2147483 ? int(seconds * 1000) : 0;
}
namespace detail {

inline uint32_t Rotl32(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

inline void Md5Block(uint32_t st[4], const unsigned char* p) {
    static const uint32_t K[64] = {
        0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au,
        0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
        0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u,
        0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
        0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
        0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
        0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
        0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
        0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u,
        0xffeff47du, 0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
        0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};
    static const int S[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

    uint32_t m[16];
    for (int i = 0; i < 16; ++i)
        m[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) |
               ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);

    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        int g;
        if (i < 16)      { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) & 15; }
        else             { f = c ^ (b | ~d);       g = (7 * i) & 15; }
        uint32_t tmp = d;
        d = c;
        c = b;
        b = b + Rotl32(a + f + K[i] + m[g], S[i]);
        a = tmp;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

}  // namespace detail

inline std::string Md5Hex(const std::string& input) {
    uint32_t st[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    const unsigned char* data = (const unsigned char*)input.data();
    size_t len = input.size();
    size_t full = len / 64;
    for (size_t i = 0; i < full; ++i) detail::Md5Block(st, data + i * 64);

    unsigned char tail[128];
    size_t rem = len - full * 64;
    std::memcpy(tail, data + full * 64, rem);
    tail[rem] = 0x80;
    size_t padLen = (rem < 56) ? 64 : 128;
    std::memset(tail + rem + 1, 0, padLen - rem - 1 - 8);
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; ++i) tail[padLen - 8 + i] = (unsigned char)((bits >> (8 * i)) & 0xFF);
    detail::Md5Block(st, tail);
    if (padLen == 128) detail::Md5Block(st, tail + 64);

    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.resize(32);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            unsigned char byte = (unsigned char)((st[i] >> (8 * j)) & 0xFF);
            out[(size_t)(i * 8 + j * 2)] = kHex[byte >> 4];
            out[(size_t)(i * 8 + j * 2 + 1)] = kHex[byte & 0x0F];
        }
    return out;
}


inline std::string wbi_key(const std::string& img, const std::string& sub) {
    auto stem = [](const std::string& url) {
        const auto slash = url.find_last_of('/'); auto name = url.substr(slash == std::string::npos ? 0 : slash + 1);
        return name.substr(0, name.find('.'));
    };
    const auto raw = stem(img) + stem(sub);
    if (raw.size() != 64) return {};
    static const int indices[] = {46,47,18,2,53,8,23,32,15,50,10,31,58,3,45,35,27,43,5,49,33,9,42,19,29,28,14,39,12,38,41,13};
    std::string key; for (const int i : indices) key += raw[size_t(i)]; return key;
}
inline std::string wbi_query(std::map<std::string, std::string> params, const std::string& key, long long timestamp) {
    params["wts"] = std::to_string(timestamp);
    std::string query;
    for (auto& pair : params) {
        auto& value = pair.second;
        value.erase(std::remove_if(value.begin(), value.end(), [](char c) { return c == '!' || c == '\'' || c == '(' || c == ')' || c == '*'; }), value.end());
        if (!query.empty()) query += '&'; query += net::url_encode(pair.first) + '=' + net::url_encode(value);
    }
    return query + "&w_rid=" + Md5Hex(query + key);
}
inline std::string strip_tags(const std::string& text) {
    std::string out; bool tag = false;
    for (char c : text) { if (c == '<') tag = true; else if (c == '>') tag = false; else if (!tag) out += c; }
    return music::text::decode_entities(out);
}
inline std::vector<Variant> parse_bilibili_search(const js::Value& root, int limit) {
    std::vector<Variant> out;
    for (const auto& item : root["data"]["result"].array) {
        Variant v; v.source = Source::Bilibili; v.id = item["bvid"].str();
        v.title = strip_tags(item["title"].str()); v.artist = music::text::decode_entities(item["author"].str());
        v.duration_ms = clock_ms(item["duration"].str()); v.cover_url = item["pic"].str();
        if (v.cover_url.rfind("//", 0) == 0) v.cover_url = "https:" + v.cover_url;
        if (v.id.rfind("BV", 0) != 0 || v.title.empty()) continue;
        out.push_back(std::move(v)); if (int(out.size()) >= limit) break;
    }
    return out;
}
struct BiliSession { std::string key, cookie; std::time_t expires = 0; };
inline BiliSession bili_session(int timeout, std::string& error) {
    static std::mutex mutex; static BiliSession cached;
    std::lock_guard<std::mutex> lock(mutex);
    if (!cached.key.empty() && cached.expires > std::time(nullptr)) return cached;
    const std::vector<std::string> headers = {"User-Agent: Mozilla/5.0", "Referer: https://www.bilibili.com/"};
    js::Value finger, nav;
    if (!json_response(net::get("https://api.bilibili.com/x/frontend/finger/spi", headers, timeout), finger, error)) return {};
    const auto buvid = finger["data"]["b_3"].str();
    if (buvid.empty()) { error = "B 站会话指纹获取失败"; return {}; }
    auto request_headers = headers;
    const auto cookie = "buvid3=" + buvid + "; buvid4=" + finger["data"]["b_4"].str();
    request_headers.push_back("Cookie: " + cookie);
    if (!json_response(net::get("https://api.bilibili.com/x/web-interface/nav", request_headers, timeout), nav, error)) return {};
    const auto key = wbi_key(nav["data"]["wbi_img"]["img_url"].str(), nav["data"]["wbi_img"]["sub_url"].str());
    if (key.empty()) { error = "B 站 WBI 密钥缺失"; return {}; }
    cached = {key, cookie, std::time(nullptr) + 3600}; return cached;
}
inline ResolveResult parse_bilibili_audio(const js::Value& root, int quality) {
    ResolveResult rr; long long best = -1;
    auto push = [&](const js::Value& format) {
        auto url = format["baseUrl"].str(format["base_url"].str());
        const auto bandwidth = format["bandwidth"].integer();
        if (!http_url(url)) return;
        if (best < 0 || (quality == 0 ? bandwidth < best : bandwidth > best)) { best = bandwidth; rr.url = url; }
    };
    const auto& dash = root["data"]["dash"];
    for (const auto& f : dash["audio"].array) push(f);
    if (quality == 2) {
        for (const auto& f : dash["dolby"]["audio"].array) push(f);
        push(dash["flac"]["audio"]);
    }
    rr.ok = !rr.url.empty(); rr.headers = {"User-Agent: Mozilla/5.0", "Referer: https://www.bilibili.com/"};
    rr.reason = rr.ok ? "ok" : "B 站无可用音轨（版权、地区或登录限制）";
    rr.detail = rr.ok ? std::to_string(best / 1000) + "kbps · 视频音轨" : root["message"].str();
    return rr;
}
} // namespace providers

inline std::vector<Variant> search_bilibili(const std::string& keyword, int limit, int timeout, std::string& error) {
    auto session = providers::bili_session(std::max(1000, timeout / 3), error);
    if (session.key.empty()) return {};
    const auto query = providers::wbi_query({{"keyword", keyword}, {"search_type", "video"}, {"page", "1"},
        {"page_size", std::to_string(std::clamp(limit, 1, 50))}}, session.key, std::time(nullptr));
    js::Value root;
    if (!providers::json_response(net::get("https://api.bilibili.com/x/web-interface/wbi/search/type?" + query,
        {"User-Agent: Mozilla/5.0", "Referer: https://www.bilibili.com/", "Cookie: " + session.cookie}, timeout / 3), root, error)) return {};
    if (root["code"].integer(-1) != 0) { error = "B 站 code=" + std::to_string(root["code"].integer()) + " " + root["message"].str(); return {}; }
    auto tracks = providers::parse_bilibili_search(root, limit);
    if (tracks.empty()) error = "B 站没有返回可用的视频结果";
    return tracks;
}
inline ResolveResult resolve_bilibili(const Variant& v, const Settings& st) {
    ResolveResult rr; js::Value view, streams; std::string error;
    const std::vector<std::string> headers = {"User-Agent: Mozilla/5.0", "Referer: https://www.bilibili.com/"};
    if (!providers::json_response(net::get("https://api.bilibili.com/x/web-interface/view?bvid=" + net::url_encode(v.id), headers, 5000), view, error)) {
        rr.reason = error; return rr;
    }
    const auto cid = view["data"]["cid"].integer();
    if (view["code"].integer(-1) != 0 || cid <= 0) { rr.reason = "B 站视频详情不可用"; rr.detail = view["message"].str(); return rr; }
    if (!providers::json_response(net::get("https://api.bilibili.com/x/player/playurl?bvid=" + net::url_encode(v.id) +
        "&cid=" + std::to_string(cid) + "&fnval=16&fnver=0&fourk=1", headers, 5000), streams, error)) {
        rr.reason = error; return rr;
    }
    return providers::parse_bilibili_audio(streams, st.quality);
}

namespace providers {
inline std::string runs_text(const js::Value& text) {
    std::string out = text["simpleText"].str();
    if (out.empty()) for (const auto& run : text["runs"].array) out += run["text"].str();
    return out;
}
inline std::string video_id(const js::Value& node, int depth = 0) {
    if (depth > 10) return {};
    auto id = node["videoId"].str();
    if (!id.empty()) return id;
    for (const auto& pair : node.object) {
        id = video_id(pair.second, depth + 1); if (!id.empty()) return id;
    }
    for (const auto& item : node.array) {
        id = video_id(item, depth + 1); if (!id.empty()) return id;
    }
    return {};
}
inline bool valid_youtube_id(const std::string& id) {
    return id.size() == 11 && id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos;
}
inline std::vector<Variant> parse_youtube_search(const js::Value& root, int limit) {
    std::vector<Variant> out; std::set<std::string> seen;
    std::function<void(const js::Value&, int)> walk;
    walk = [&](const js::Value& node, int depth) {
        if (depth > 64 || int(out.size()) >= limit) return;
        const auto& responsive = node["musicResponsiveListItemRenderer"];
        const auto& two_row = node["musicTwoRowItemRenderer"];
        const auto& video = node["videoRenderer"];
        const js::Value* item = nullptr;
        Variant v; v.source = Source::YouTube;
        if (responsive.is_object()) {
            item = &responsive;
            const auto& columns = responsive["flexColumns"];
            v.id = responsive["playlistItemData"]["videoId"].str();
            if (v.id.empty()) v.id = video_id(columns[size_t(0)]);
            v.title = runs_text(columns[size_t(0)]["musicResponsiveListItemFlexColumnRenderer"]["text"]);
            const auto& metadata = columns[size_t(1)]["musicResponsiveListItemFlexColumnRenderer"]["text"];
            for (const auto& run : metadata["runs"].array) {
                const auto text = run["text"].str();
                if (clock_ms(text) > 0) v.duration_ms = clock_ms(text);
                else if (v.artist.empty() && text != " • " && text != " · ") v.artist = text;
            }
            for (const auto& fixed : responsive["fixedColumns"].array) {
                const auto duration = clock_ms(runs_text(fixed["musicResponsiveListItemFixedColumnRenderer"]["text"]));
                if (duration) v.duration_ms = duration;
            }
        } else if (two_row.is_object()) {
            item = &two_row; v.id = video_id(two_row["navigationEndpoint"]);
            v.title = runs_text(two_row["title"]); v.artist = runs_text(two_row["subtitle"]);
        } else if (video.is_object()) {
            item = &video; v.id = video["videoId"].str(); v.title = runs_text(video["title"]);
            v.artist = runs_text(video["ownerText"]); v.duration_ms = clock_ms(runs_text(video["lengthText"]));
        }
        if (item) {
            const auto& thumbs = (*item)["thumbnail"]["musicThumbnailRenderer"]["thumbnail"]["thumbnails"].is_array() ?
                (*item)["thumbnail"]["musicThumbnailRenderer"]["thumbnail"]["thumbnails"] : (*item)["thumbnail"]["thumbnails"];
            if (!thumbs.array.empty()) v.cover_url = thumbs.array.back()["url"].str();
            if (valid_youtube_id(v.id) && !v.title.empty() && seen.insert(v.id).second) out.push_back(std::move(v));
            return;
        }
        for (const auto& pair : node.object) walk(pair.second, depth + 1);
        for (const auto& child : node.array) walk(child, depth + 1);
    };
    walk(root, 0); return out;
}
// 只接受明确直链；signatureCipher 需要解签，不能把其 URL 直接当成可播地址。
inline ResolveResult parse_youtube_audio(const js::Value& root, const std::vector<std::string>& headers, int quality) {
    ResolveResult rr; rr.headers = headers;
    const auto status = root["playabilityStatus"]["status"].str();
    if (status != "OK") { rr.reason = root["playabilityStatus"]["reason"].str("YouTube " + status); return rr; }
    long long best = -1; bool best_mp4 = false, cipher = false;
    for (const auto& format : root["streamingData"]["adaptiveFormats"].array) {
        const auto mime = format["mimeType"].str();
        if (mime.rfind("audio/", 0) != 0) continue;
        if (!format["signatureCipher"].str().empty() || !format["cipher"].str().empty()) { cipher = true; continue; }
        const auto url = format["url"].str();
        if (!http_url(url)) continue;
        const bool mp4 = mime.find("mp4") != std::string::npos;
        const auto bitrate = format["bitrate"].integer();
        if (best < 0 || (mp4 && !best_mp4) || (mp4 == best_mp4 && (quality == 0 ? bitrate < best : bitrate > best))) {
            best = bitrate; best_mp4 = mp4; rr.url = url;
        }
    }
    rr.ok = !rr.url.empty(); rr.reason = rr.ok ? "ok" : cipher ? "YouTube 音轨需要签名解析" : "YouTube 未提供直接音轨";
    if (rr.ok) rr.detail = std::to_string(best / 1000) + "kbps · YouTube Music";
    return rr;
}
inline js::Value youtube_page_config(const std::string& page) {
    const auto begin = page.find("ytcfg.set({");
    if (begin == std::string::npos) return {};
    const auto start = page.find('{', begin);
    bool quoted = false, escaped = false; int depth = 0;
    for (size_t i = start; i < page.size(); ++i) {
        const char c = page[i];
        if (quoted) {
            if (escaped) escaped = false; else if (c == '\\') escaped = true; else if (c == '"') quoted = false;
        } else {
            if (c == '"') quoted = true; else if (c == '{') ++depth; else if (c == '}' && --depth == 0) {
                js::Value config; js::parse(page.substr(start, i - start + 1), config); return config;
            }
        }
    }
    return {};
}
struct YouTubeConfig {
    // yt-dlp WEB_REMIX 后备值；有网络时优先读取官网当前 ytcfg。
    std::string version = "1.20260707.12.00";
    std::string key = "AIzaSyA8eiZmM1FaDVjRy-df2KTyQ_vz_yYM39w";
    std::string visitor;
};
inline YouTubeConfig youtube_config(int timeout) {
    static std::mutex mutex; static YouTubeConfig cached; static std::time_t expires = 0;
    std::lock_guard<std::mutex> lock(mutex);
    if (expires > std::time(nullptr)) return cached;
    const auto response = net::get("https://music.youtube.com/", {"User-Agent: Mozilla/5.0"}, timeout);
    const auto config = response.ok() && response.body.size() < 8 * 1024 * 1024 ? youtube_page_config(response.body) : js::Value();
    const auto& client = config["INNERTUBE_CONTEXT"]["client"];
    cached.version = client["clientVersion"].str(config["INNERTUBE_CLIENT_VERSION"].str(cached.version));
    cached.key = config["INNERTUBE_API_KEY"].str(cached.key); cached.visitor = client["visitorData"].str();
    expires = std::time(nullptr) + (config.is_object() ? 3600 : 60); return cached;
}
inline std::string youtube_context(const YouTubeConfig& config) {
    return "{\"client\":{\"clientName\":\"WEB_REMIX\",\"clientVersion\":" + quote(config.version) +
        ",\"hl\":\"zh-CN\",\"gl\":\"US\",\"visitorData\":" + quote(config.visitor) + "}}";
}
inline std::vector<std::string> youtube_headers(const YouTubeConfig& config) {
    auto headers = std::vector<std::string>{"Content-Type: application/json", "User-Agent: Mozilla/5.0",
        "Origin: https://music.youtube.com", "Referer: https://music.youtube.com/", "X-YouTube-Client-Name: 67",
        "X-YouTube-Client-Version: " + config.version};
    if (!config.visitor.empty()) headers.push_back("X-Goog-Visitor-Id: " + config.visitor);
    return headers;
}
} // namespace providers

inline std::vector<Variant> search_youtube(const std::string& keyword, int limit, int timeout, std::string& error) {
    const auto config = providers::youtube_config(std::max(1000, timeout / 3));
    const auto body = "{\"context\":" + providers::youtube_context(config) + ",\"query\":" + providers::quote(keyword) + "}";
    const auto response = net::post("https://music.youtube.com/youtubei/v1/search?prettyPrint=false&key=" + net::url_encode(config.key),
        body, providers::youtube_headers(config), timeout * 2 / 3);
    js::Value root;
    if (!providers::json_response(response, root, error)) { error = "YouTube Music 网络/接口不可用：" + error; return {}; }
    auto out = providers::parse_youtube_search(root, limit);
    if (out.empty()) error = "YouTube Music 无可用结果（可能需要验证）";
    return out;
}

// 可选系统工具兜底签名，不自动下载、不装浏览器扩展、不读用户浏览器 Cookie。
inline ResolveResult resolve_youtube_tool(const Variant& v) {
    ResolveResult rr;
    static std::atomic<unsigned> serial{0};
    const auto file = platform::join(platform::temp_dir(), "yt-resolve-" + std::to_string(platform::process_id()) + "-" + std::to_string(++serial) + ".json");
    const int rc = platform::run_process({"yt-dlp", "--ignore-config", "--no-playlist", "--skip-download", "--quiet", "--no-warnings",
        "--no-cache-dir", "--socket-timeout", "8", "--retries", "0", "--extractor-retries", "0", "-f", "bestaudio[ext=m4a]/bestaudio",
        "--print-to-file", "%()j", file, "--", "https://music.youtube.com/watch?v=" + v.id}, 25000);
    if (rc != 0 || platform::file_size(file) > 8 * 1024 * 1024) {
        platform::remove(file);
        rr.reason = rc == -1 ? "可选 yt-dlp 未安装" : rc == 124 ? "yt-dlp 解析超时" : "yt-dlp 无法取得音轨（网络或平台验证）";
        return rr;
    }
    std::ifstream input(platform::fs::u8path(file), std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(input)), {}); input.close(); platform::remove(file);
    js::Value info;
    if (!js::parse(text, info)) { rr.reason = "yt-dlp 返回无效数据"; return rr; }
    const auto* selected = &info;
    if (!info["requested_downloads"].array.empty()) selected = &info["requested_downloads"].array.front();
    rr.url = (*selected)["url"].str();
    const auto protocol = (*selected)["protocol"].str();
    rr.ok = providers::http_url(rr.url) && (protocol.empty() || protocol == "https" || protocol == "http");
    for (const auto& pair : (*selected)["http_headers"].object) {
        const auto value = pair.second.str();
        if (pair.first.find_first_of("\r\n:") == std::string::npos && value.find_first_of("\r\n") == std::string::npos)
            rr.headers.push_back(pair.first + ": " + value);
    }
    rr.reason = rr.ok ? "ok" : "yt-dlp 未返回直接音轨"; rr.detail = "YouTube Music · yt-dlp"; return rr;
}
inline ResolveResult resolve_youtube(const Variant& v, const Settings& st) {
    ResolveResult rr;
    if (!providers::valid_youtube_id(v.id)) { rr.reason = "无效的 YouTube 曲目 ID"; return rr; }
    const auto config = providers::youtube_config(2000);
    const auto headers = providers::youtube_headers(config);
    const auto body = "{\"context\":" + providers::youtube_context(config) + ",\"videoId\":" + providers::quote(v.id) +
        ",\"contentCheckOk\":true,\"racyCheckOk\":true}";
    js::Value root; std::string error;
    const auto response = net::post("https://music.youtube.com/youtubei/v1/player?prettyPrint=false&key=" + net::url_encode(config.key), body, headers, 5000);
    if (providers::json_response(response, root, error)) {
        rr = providers::parse_youtube_audio(root, {"User-Agent: Mozilla/5.0", "Referer: https://music.youtube.com/"}, st.quality);
        if (rr.ok) {
            const auto probe = net::probe_audio(rr.url, rr.headers, 4000);
            if (probe.playable) return rr;
            rr.reason = "YouTube 音轨被拒（需要签名或平台验证）";
        }
    } else rr.reason = error;
    auto fallback = resolve_youtube_tool(v);
    if (fallback.ok) return fallback;
    rr.ok = false; rr.url.clear(); rr.reason += "；" + fallback.reason; return rr;
}

// 取流：按当前选中源解析，失败就沿"其它源"换源
inline ResolveResult resolve(const Variant& v, const Settings& st) {
    switch (v.source) {
        case Source::Netease: return resolve_netease(v, st);
        case Source::Kuwo: return resolve_kuwo(v);
        case Source::QQ: return resolve_qq(v, st);
        case Source::Bilibili: return resolve_bilibili(v, st);
        case Source::YouTube: return resolve_youtube(v, st);
        default: { ResolveResult rr; rr.reason = "此音源暂不支持网络取流"; return rr; }
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
    else if (src == Source::Kuwo) got = search_kuwo(kw, 10, 6000, err);
    else if (src == Source::YouTube) got = search_youtube(kw, 10, 6000, err);
    else if (src == Source::Bilibili) got = search_bilibili(kw, 10, 6000, err);

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
