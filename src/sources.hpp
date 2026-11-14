// sources.hpp —— 多音源适配 + 聚合
//
// 每个源适配搜索、取流及歌手/专辑目录；
// 聚合层负责并发扇出、超时隔离、跨源同曲合并、相关性排序。
//
// 检索由平台提供；播放地址由独立渠道解析，播放业务决定流式或文件播放。
#pragma once

#include <algorithm>
#include <array>
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

#include "background.hpp"
#include "json.hpp"
#include "lxsource.hpp"
#include "provider.hpp"
#include "net.hpp"
#include "platform.hpp"
#include "login.h"
#include <cstring>
#include <ctime>
#include <functional>
#include <fstream>
#include <set>

namespace music {

// ---------------------------------------------------------------- 数据结构

// 注意：这个枚举的数值会写进资料库（library.hpp 存的是 int），只能往后追加，
// 不能插队改值，否则老用户的歌单会串源。
// 平台编号持久化后固定；3 是已移除平台的历史编号，不复用。
//
// 数值故意留空洞（Bilibili 仍是 4、Local 仍是 5）：资料库把 source 存成 int，
// 重排数值会让老歌单串源。恢复平台使用原编号，新平台只在末尾追加。
enum class Source { Netease = 0, QQ = 1, Kuwo = 2, Bilibili = 4, Local = 5, Kugou = 6, Migu = 7 };
constexpr int kSourceCount = 8;  // Migu = 7，3 保留空位。

// 平台身份与能力在这里集中登记；协议 URL 和响应解析归 providers/*.js。
struct PlatformInfo {
    Source source;
    const char *name, *key, *lx_key;
    bool discovery, script_playback;
};
inline constexpr PlatformInfo kPlatformInfo[] = {
    {Source::Netease, "网易云", "netease", "wy", true, false},
    {Source::QQ, "QQ", "qq", "tx", true, false},
    {Source::Kuwo, "酷我", "kuwo", "kw", true, true},
    {Source::Kugou, "酷狗", "kugou", "kg", true, true},
    {Source::Migu, "咪咕", "migu", "mg", true, true},
    {Source::Bilibili, "B 站", "bilibili", nullptr, false, false},
};
constexpr int kPlatformCount = int(sizeof(kPlatformInfo) / sizeof(kPlatformInfo[0]));
inline constexpr auto kPlatforms = [] {
    std::array<Source, kPlatformCount> out{};
    for (int i = 0; i < kPlatformCount; ++i) out[i] = kPlatformInfo[i].source;
    return out;
}();
constexpr int kDiscoveryCount = [] { int n = 0; for (const auto& p : kPlatformInfo) if (p.discovery) ++n; return n; }();
inline constexpr auto kDiscoveryPlatforms = [] {
    std::array<Source, kDiscoveryCount> out{}; int n = 0;
    for (const auto& p : kPlatformInfo) if (p.discovery) out[n++] = p.source;
    return out;
}();
inline const PlatformInfo* platform_info(Source source) {
    for (const auto& p : kPlatformInfo) if (p.source == source) return &p;
    return nullptr;
}
inline bool is_platform(Source source) { return platform_info(source) != nullptr; }
inline bool script_platform(Source source) { const auto* p = platform_info(source); return p && p->script_playback; }

// 渠道（Lane）—— 和平台（Source）正交的一维。
//
// 同一个"网易云"，可能有两条完全不同的取流路径：
//   JsSource：第三方音源脚本（落雪/六音这类），免登录、能到无损，但随时可能失效
//   Builtin ：自带接口（providers/*.js + C++ 兜底），可登录、音质上限由账号决定
//
// 检索/榜单只有 Builtin 能提供 —— 音源脚本的契约只有 musicUrl/pic/lyric，
// 它不负责"从哪找歌"。这个区分以前被藏起来了，是 UI 混乱的根源。
enum class Lane { JsSource = 0, Builtin = 1 };
constexpr int kLaneCount = 2;

inline const char* lane_name(Lane l) { return l == Lane::JsSource ? "音源" : "自带"; }
inline const char* lane_full_name(Lane l) { return l == Lane::JsSource ? "第三方音源" : "自带接口"; }

inline const char* source_name(Source source) {
    const auto* p = platform_info(source);
    return p ? p->name : source == Source::Local ? "本地" : "未知";
}
inline const char* source_key(Source source) {
    const auto* p = platform_info(source);
    return p ? p->key : source == Source::Local ? "local" : "unknown";
}

struct ArtistRef { std::string id, name; };
enum class CatalogKind { Artist, Album };
struct CatalogTarget {
    Source source = Source::Netease;
    CatalogKind kind = CatalogKind::Artist;
    std::string id, name, cover, subtitle;
};

struct Variant {  // 某一首歌在某个源上的一条结果
    Source source = Source::Netease;
    Lane lane = Lane::Builtin;  // 这条结果由哪条渠道产出（检索永远是 Builtin）
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    int duration_ms = 0;
    int bitrate = 0;          // 源标称档位（0 = 未知）
    bool needs_login = false; // 该源自己说"要登录"，取流时会明确拒绝
    std::string cover_url;    // 该源能直接给出的封面（QQ 有 pmid，网易云要批量查）
    std::string native_id;    // 源内部 ID：QQ songid、B 站分 P cid 等。
    std::vector<ArtistRef> artists;
    std::string album_id;
    std::string album_audio_id; // 酷狗混合歌曲编号，与普通 Audioid 分开。
    std::map<std::string, std::string> quality_hashes; // 酷狗各音质对应的 hash。
};

inline bool same_variant(const Variant& a, const Variant& b) {
    return a.source == b.source && !a.id.empty() && a.id == b.id &&
        (a.source != Source::Bilibili || a.native_id == b.native_id);
}
inline std::string variant_uid(const Variant& v) {
    return std::string(source_key(v.source)) + "#" + v.id +
        (v.source == Source::Bilibili && !v.native_id.empty() ? "#" + v.native_id : "");
}

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
    Lane lane = Lane::Builtin;  // 检索渠道：目前恒为 Builtin（音源脚本不提供检索）
    bool ok = false;
    bool pending = false, has_more = false;
    int count = 0;
    int ms = 0;
    std::string err;
};

struct SearchResult {
    std::string keyword;
    std::vector<Track> tracks;
    std::vector<SourceReport> reports;
    int elapsed_ms = 0;
    std::array<std::string, kSourceCount> continuation;
};

struct Settings {
    std::string netease_cookie;  // MUSIC_U=...；留空则只用免费档
    std::string qq_cookie;       // qm_keyst=...
    std::function<bool()> cancelled; // 当前播放/下载任务的取消信号，不持久化。
    bool enable[kSourceCount] = {true, true, false, false, false, false, false, false};
    // 音质：0 = 标准 128k，1 = 高 320k，2 = 无损
    // 可用档位由平台和账号权限决定：网易云无损需要会员，QQ 取流需要登录。
    int quality = 1;
    // 渠道：取流时先问谁。0 = 第三方音源优先（默认，免登录且能到无损），1 = 自带接口优先。
    // 两条渠道可以单独关掉：关了音源就只剩自带，反之亦然。
    int lane_pref = 0;
    bool lane_enable[kLaneCount] = {true, true};
    // 下载时把封面 / 歌词写进文件（mp3 走 ID3v2.3，m4a 走 ilst）
    bool embed_cover = false;
    bool embed_lyrics = false;

    bool with_netease_cookie() const { return !netease_cookie.empty(); }
    bool lane_on(Lane l) const { return lane_enable[int(l)]; }
    // 按偏好排出的尝试顺序
    std::array<Lane, kLaneCount> lane_order() const {
        return lane_pref == 0 ? std::array<Lane, kLaneCount>{Lane::JsSource, Lane::Builtin}
                              : std::array<Lane, kLaneCount>{Lane::Builtin, Lane::JsSource};
    }
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
    Lane lane = Lane::Builtin;         // 成功时：链接由哪条渠道给出
    std::string engine;                // 具体是谁给的：音源脚本名 / provider 文件名 / "内置"
    std::string url;
    std::vector<std::string> headers;  // 下载/探测时要带的头
    std::string reason;                // 结构化原因（失败时）
    std::string detail;                // 人话补充（渠道 · 实现 · 音质 · 容器）
    std::shared_ptr<lx::Engine::LoadTiming> load_timing;
};
// 消费端确认候选能实际使用后才结束取流；拒绝时将原因交给下一候选。
using ResolveAccept = std::function<bool(const ResolveResult&, std::string&)>;
inline bool accept_resolved(const ResolveResult& candidate, const ResolveAccept& accept, std::string& error) {
    if (net::cancelled()) { error = "取流已取消"; return false; }
    try { return !accept || accept(candidate, error); }
    catch (const std::exception& exception) { error = exception.what(); return false; }
    catch (...) { error = "准备候选音频失败"; return false; }
}

inline std::vector<Variant> search_bilibili(const std::string&, int, int, std::string&, int = 0);

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

inline std::string catalog_id(const js::Value& value) {
    const auto text = value.str();
    if (!text.empty()) return text == "0" ? std::string() : text;
    const auto number = value.integer();
    return number > 0 ? std::to_string(number) : std::string();
}
inline void attach_catalog_metadata(Variant& v, const js::Value& song) {
    if (v.source == Source::Netease) {
        const auto& artists = song["ar"].is_array() ? song["ar"] : song["artists"];
        for (const auto& a : artists.array) {
            if (v.artists.size() >= 32) break;
            const auto name = text::decode_entities(a["name"].str());
            if (!name.empty()) v.artists.push_back({catalog_id(a["id"]), name});
        }
        const auto& album = song["al"].is_object() ? song["al"] : song["album"];
        v.album_id = catalog_id(album["id"]);
    } else if (v.source == Source::QQ) {
        for (const auto& a : song["singer"].array) {
            if (v.artists.size() >= 32) break;
            const auto name = text::decode_entities(a["name"].str());
            if (!name.empty()) v.artists.push_back({a["mid"].str(), name});
        }
        v.album_id = song["album"]["mid"].str(song["albummid"].str());
    }
}
inline std::vector<ArtistRef> track_artists(const Track& track) {
    const auto& v = track.current();
    if (!v.artists.empty()) return v.artists;
    const auto raw = v.artist.empty() ? track.artist : v.artist;
    std::vector<ArtistRef> out;
    // With no structured metadata, only split the separators emitted by our parsers.
    size_t begin = 0;
    while (begin < raw.size() && out.size() < 32) {
        const auto end = raw.find(" / ", begin);
        auto name = text::trim(raw.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (!name.empty()) out.push_back({{}, std::move(name)});
        if (end == std::string::npos) break;
        begin = end + 3;
    }
    return out;
}
inline std::string track_album(const Track& track) {
    return track.current().album.empty() ? track.album : track.current().album;
}

inline bool search_via_provider(Source src, const std::string& keyword, int limit, int page,
                                std::vector<Variant>& out, std::string& err);
inline std::vector<Variant> search_platform(Source src, const std::string& keyword, int limit, int timeout_ms,
                                           std::string& err, const Settings& st, int page = 0) {
    prov::ExecutionTime budget(timeout_ms);
    prov::Host::instance().set_credentials(st.netease_cookie, st.qq_cookie);
    std::vector<Variant> out;
    if (!search_via_provider(src, keyword, limit, page, out, err) && err.empty())
        err = std::string(source_name(src)) + " 平台脚本未加载，请检查 providers 目录";
    return out;
}
inline std::vector<Variant> search_netease(const std::string& keyword, int limit, int timeout_ms,
                                      std::string& err, const Settings& st, int page = 0) {
    return search_platform(Source::Netease, keyword, limit, timeout_ms, err, st, page);
}

/** @brief 解析网易云播放地址，保留服务端拒绝码；码率仅作展示，不决定播放权限。 */
inline ResolveResult parse_netease_audio(const js::Value& root) {
    ResolveResult rr;
    if (root["code"].integer() != 200) {
        rr.reason = root["code"].is_null() ? "播放地址响应缺少 code"
            : "播放地址接口拒绝（code " + std::to_string(root["code"].integer()) + "）";
        return rr;
    }
    const auto& data = root["data"];
    if (!data.is_array() || !data.size()) { rr.reason = "播放地址响应缺少 data"; return rr; }
    const auto& item = data[size_t(0)];
    if (item["code"].integer() != 200) {
        rr.reason = item["code"].is_null() ? "播放地址响应缺少歌曲 code"
            : "平台拒绝播放地址（code " + std::to_string(item["code"].integer()) + "）";
        return rr;
    }
    const auto url = item["url"].str();
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
        rr.reason = "平台未提供有效播放地址"; return rr;
    }
    rr.ok = true; rr.url = url; rr.reason = "ok";
    const auto bitrate = item["br"].integer();
    const auto type = item["type"].str();
    rr.detail = bitrate > 0 ? std::to_string(bitrate / 1000) + "kbps" : "在线音频";
    if (!type.empty()) rr.detail += " " + type;
    return rr;
}

inline ResolveResult resolve_netease(const Variant& v, const Settings& st) {
    ResolveResult rr;
    std::vector<std::string> headers = {"Referer: https://music.163.com/",
                                        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
                                        "Cookie: appver=2.0.2; os=pc" +
                                            (st.netease_cookie.empty() ? std::string()
                                                                       : "; " + st.netease_cookie)};

    // 标准档同样需要携带账号权限，不能仅因音质设置而跳过登录态取流。
    std::string account_error;
    if (!st.netease_cookie.empty()) {
        // br：128000 / 320000 / 999000（999000 = 无损，服务端没有就自己降档返回）
        const int want_br = st.quality >= 2 ? 999000 : st.quality == 0 ? 128000 : 320000;
        const std::string api = "https://music.163.com/api/song/enhance/player/url?ids=%5B" + v.id +
                                "%5D&br=" + std::to_string(want_br);
        const net::Response r = net::get(api, headers, 6000);
        if (r.ok()) {
            js::Value root;
            if (js::parse(r.body, root, true)) {
                rr = parse_netease_audio(root);
                if (rr.ok) {
                    rr.headers = headers;
                    return rr;
                }
                account_error = rr.reason;
            } else account_error = "播放地址响应不是 JSON";
        } else account_error = "播放地址请求失败（" +
            (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error) + "）";
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
    rr.reason = !account_error.empty() ? account_error :
        (v.needs_login ? "请先登录并确认账号播放权限" : "免费外链不可用");
    if (!account_error.empty() && !p.reason.empty()) rr.reason += "；免费外链：" + p.reason;
    rr.detail = p.reason;
    return rr;
}

// ---------------------------------------------------------------- 适配器：QQ

inline std::vector<Variant> search_qq(const std::string& keyword, int limit, int timeout_ms,
                                      std::string& err, const Settings& st, int page = 0) {
    return search_platform(Source::QQ, keyword, limit, timeout_ms, err, st, page);
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
        const std::string container = ext == ".flac" ? "flac" : (ext == ".mflac" ? "mflac" : (ext == ".m4a" ? "AAC/m4a" : "mp3"));
        // 前缀就是 QQ 的档位编码，如实翻出来。C400 是最后那条 96k AAC 兜底，
        // 以前把它一并写成 "128k · mp3"，听起来发闷却显示得挺好，等于骗人。
        std::string q;
        if (used.rfind("F000", 0) == 0 || used.rfind("F0M0", 0) == 0) q = "无损";
        else if (used.rfind("M800", 0) == 0) q = "320k";
        else if (used.rfind("M500", 0) == 0) q = "128k";
        else if (used.rfind("C400", 0) == 0) q = "96k（平台最低档兜底）";
        else q = "码率未知";
        rr.detail = q + " · " + container;
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

// 定义在下面（provider 桥接段），这里先声明给 run_search 用
inline bool search_via_provider(Source src, const std::string& keyword, int limit, int page,
                                std::vector<Variant>& out, std::string& err);
inline bool board_via_provider(Source src, const std::string& board_id, int page, int limit,
                               std::vector<Variant>& out, std::string& err);

struct Job {
    std::mutex m;
    std::condition_variable cv;
    int done = 0;
    std::vector<char> finished;
    std::vector<std::vector<Variant>> per_source;
    std::vector<SourceReport> reports;
};

// 并发检索：每个源一个线程，9 秒软超时；超时/失败的源降级成 SourceReport 里的红字
inline SearchResult run_search(const std::string& keyword, const Settings& st, int limit_per_source = 12,
                               int soft_timeout_ms = 9000, int page = 0,
                               const std::array<std::string, kSourceCount>& continuations = {},
                                const std::function<void(const SearchResult&)>& publish = {},
                                const std::array<int, kSourceCount>* pages = nullptr) {
    using clock = std::chrono::steady_clock;
    const auto t_start = clock::now();
    auto job = std::make_shared<Job>();
    job->per_source.resize(kSourceCount); job->reports.resize(kSourceCount); job->finished.assign(kSourceCount, 0);
    std::array<std::string, kSourceCount> next;
    std::atomic<bool> stopped{false};
    background::ThreadGroup workers;
    for (int i = 0; i < kSourceCount; ++i) {
        job->reports[size_t(i)].source = Source(i);
        // 历史平台编号只留空位，不参与检索
        if (!is_platform(Source(i)) && Source(i) != Source::Local) {
            job->reports[size_t(i)].pending = false;
            job->reports[size_t(i)].err = "已移除";
            job->finished[size_t(i)] = 1;
            continue;
        }
        job->reports[size_t(i)].pending = st.enable[i];
        job->reports[size_t(i)].err = st.enable[i] ? "检索中" : "已关闭";
        if (!st.enable[i]) continue;
        workers.emplace_back([&, i] {
            net::CancelScope cancellation([&] { return stopped.load() || (st.cancelled && st.cancelled()); });
            std::string error, cursor;
            std::vector<Variant> got;
            const int source_page = pages ? (*pages)[size_t(i)] : page;
            const auto began = clock::now();
            try {
                if (i == int(Source::Bilibili)) got = search_bilibili(keyword, limit_per_source, 8000, error, source_page);
                else if (is_platform(Source(i))) got = search_platform(Source(i), keyword, limit_per_source, 8000, error, st, source_page);
            } catch (...) { error = "适配器异常"; }
            std::lock_guard<std::mutex> lock(job->m);
            auto& report = job->reports[size_t(i)];
            report.pending = false; report.count = int(got.size()); report.ok = !got.empty() || error.empty(); report.err = error;
            report.has_more = int(got.size()) >= limit_per_source;
            report.ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - began).count());
            job->per_source[size_t(i)] = std::move(got); next[size_t(i)] = std::move(cursor);
            job->finished[size_t(i)] = 1; ++job->done; job->cv.notify_all();
        });
    }
    auto snapshot = [&] {
        SearchResult result; result.keyword = keyword;
        std::vector<std::vector<Variant>> per_source;
        { std::lock_guard<std::mutex> lock(job->m); per_source = job->per_source; result.reports = job->reports; result.continuation = next; }
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
        result.elapsed_ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t_start).count());
        return result;
    };
    int seen = -1;
    for (;;) {
        std::unique_lock<std::mutex> lock(job->m);
        job->cv.wait_for(lock, std::chrono::milliseconds(30), [&] { return job->done != seen; });
        const int done = job->done;
        lock.unlock();
        if (done != seen) { seen = done; if (publish) publish(snapshot()); }
        if (done >= int(workers.size())) break;
        if ((st.cancelled && st.cancelled()) || clock::now() - t_start >= std::chrono::milliseconds(soft_timeout_ms)) { stopped.store(true); break; }
    }
    workers.join();
    return snapshot();
}

// ---------------------------------------------------------------- 封面

// 给合并后的曲目补封面：
//   1. 有 QQ 变体的，pmid 直链直接用（零请求）
//   2. 其余用网易云的 song/detail 批量查（一次请求覆盖所有网易云 id）
// 全失败就保持空，界面回落到"程序化渐变封面"。
inline void fill_covers(std::vector<Track>& tracks, const Settings& st) {
    if (st.cancelled && st.cancelled()) return;
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

    if (!netease_ids.empty() && !(st.cancelled && st.cancelled())) {
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
        v.artists.push_back({catalog_id(item["mid"]), v.artist});
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

inline std::vector<Variant> search_bilibili(const std::string& keyword, int limit, int timeout, std::string& error, int page) {
    auto session = providers::bili_session(std::max(1000, timeout / 3), error);
    if (session.key.empty()) return {};
    const auto query = providers::wbi_query({{"keyword", keyword}, {"search_type", "video"}, {"page", std::to_string(std::max(0, page) + 1)},
        {"page_size", std::to_string(std::clamp(limit, 1, 50))}}, session.key, std::time(nullptr));
    js::Value root;
    if (!providers::json_response(net::get("https://api.bilibili.com/x/web-interface/wbi/search/type?" + query,
        {"User-Agent: Mozilla/5.0", "Referer: https://www.bilibili.com/", "Cookie: " + session.cookie}, timeout / 3), root, error)) return {};
    if (root["code"].integer(-1) != 0) { error = "B 站 code=" + std::to_string(root["code"].integer()) + " " + root["message"].str(); return {}; }
    auto tracks = providers::parse_bilibili_search(root, limit);
    if (tracks.empty()) error = "B 站没有返回可用的视频结果";
    return tracks;
}
struct BilibiliParts {
    std::string title, cover;
    std::vector<Track> tracks;
};
namespace providers {
inline bool bili_cid(const std::string& cid) {
    return !cid.empty() && cid.size() <= 18 && cid[0] != '0' &&
        cid.find_first_not_of("0123456789") == std::string::npos;
}
inline BilibiliParts parse_bilibili_parts(const js::Value& root, const std::string& bvid, std::string& error) {
    error.clear();
    BilibiliParts out;
    if (root["code"].integer(-1) != 0) { error = "B 站视频详情不可用：" + root["message"].str(); return out; }
    const auto& data = root["data"];
    out.title = data["title"].str(); out.cover = data["pic"].str();
    std::set<std::string> seen;
    for (const auto& page : data["pages"].array) {
        const auto cid = std::to_string(page["cid"].integer());
        if (!bili_cid(cid) || !seen.insert(cid).second) continue;
        Variant v; v.source = Source::Bilibili; v.id = bvid; v.native_id = cid;
        v.title = "P" + std::to_string(page["page"].integer(out.tracks.size() + 1)) + " · " + page["part"].str(out.title);
        v.artist = data["owner"]["name"].str(); v.album = out.title; v.cover_url = out.cover;
        v.artists.push_back({catalog_id(data["owner"]["mid"]), v.artist});
        v.duration_ms = int(std::clamp<long long>(page["duration"].integer(), 0, 2147483) * 1000);
        Track t; t.title = v.title; t.artist = v.artist; t.album = v.album;
        t.duration_ms = v.duration_ms; t.cover_url = v.cover_url; t.variants.push_back(std::move(v));
        out.tracks.push_back(std::move(t));
    }
    if (out.tracks.empty()) error = "B 站没有返回可读取的分 P";
    return out;
}
inline std::string bili_playurl(const Variant& v, const std::string& cid) {
    if (v.id.rfind("BV", 0) != 0 || !bili_cid(cid)) return {};
    return "https://api.bilibili.com/x/player/playurl?bvid=" + net::url_encode(v.id) +
        "&cid=" + cid + "&fnval=16&fnver=0&fourk=1";
}
} // namespace providers
inline BilibiliParts bilibili_parts(const std::string& bvid, std::string& error) {
    error.clear(); js::Value root;
    if (bvid.rfind("BV", 0) != 0 || bvid.size() > 32) { error = "无效的 B 站视频 ID"; return {}; }
    if (!providers::json_response(net::get("https://api.bilibili.com/x/web-interface/view?bvid=" + net::url_encode(bvid),
        {"User-Agent: Mozilla/5.0", "Referer: https://www.bilibili.com/"}, 8000), root, error)) return {};
    return providers::parse_bilibili_parts(root, bvid, error);
}
inline ResolveResult resolve_bilibili(const Variant& v, const Settings& st) {
    ResolveResult rr; js::Value view, streams; std::string error;
    const std::vector<std::string> headers = {"User-Agent: Mozilla/5.0", "Referer: https://www.bilibili.com/"};
    auto cid = v.native_id;
    if (cid.empty()) {
        if (!providers::json_response(net::get("https://api.bilibili.com/x/web-interface/view?bvid=" + net::url_encode(v.id), headers, 5000), view, error)) {
            rr.reason = error; return rr;
        }
        if (view["code"].integer(-1) != 0) { rr.reason = "B 站视频详情不可用"; rr.detail = view["message"].str(); return rr; }
        cid = std::to_string(view["data"]["cid"].integer());
    }
    const auto url = providers::bili_playurl(v, cid);
    if (url.empty()) { rr.reason = "B 站分 P cid 无效，请重新打开分 P 列表"; return rr; }
    if (!providers::json_response(net::get(url, headers, 5000), streams, error)) {
        rr.reason = error; return rr;
    }
    return providers::parse_bilibili_audio(streams, st.quality);
}

// ---------------------------------------------------------------- 平台 provider（JS）
//
// 各平台接口的易变部分都在 providers/*.js 里；C++ 只负责调用和类型转换。
// 同平台搜索只有一份 JS 实现，失败直接报告，不再隐式重复请求。
inline const char* provider_key_of(Source source) {
    const auto* p = platform_info(source);
    return p && p->lx_key ? p->key : nullptr; // B 站的 WBI / 分 P 适配保留在 C++。
}

inline void attach_provider_song(Variant& v, const prov::Song& s) {
    v.id = s.id;
    v.title = text::decode_entities(s.title);
    v.artist = text::decode_entities(s.artist);
    v.album = text::decode_entities(s.album);
    v.duration_ms = s.duration_ms;
    v.bitrate = s.bitrate;
    v.native_id = s.native_id;
    v.cover_url = s.cover;
    v.album_id = s.metadata["album_id"].str();
    v.album_audio_id = s.metadata["album_audio_id"].str();
    v.needs_login = s.metadata["needs_login"].truthy();
    for (const auto& a : s.metadata["artists"].array)
        if (v.artists.size() < 32 && !a["name"].str().empty()) v.artists.push_back({a["id"].str(), text::decode_entities(a["name"].str())});
    for (const auto& item : s.metadata["quality_hashes"].object)
        v.quality_hashes[item.first] = item.second.str();
}

inline bool search_via_provider(Source src, const std::string& keyword, int limit, int page,
                                std::vector<Variant>& out, std::string& err) {
    const char* key = provider_key_of(src);
    if (!key) { err = "平台不支持此目录接口"; return false; }
    std::vector<prov::Song> songs;
    if (!prov::Host::instance().search(key, keyword, page, limit, songs, err)) return false;
    out.clear();
    for (const auto& song : songs) {
        if (song.id.empty() || song.title.empty()) continue;
        Variant v;
        v.source = src;
        attach_provider_song(v, song);
        out.push_back(std::move(v));
    }
    return true;
}

inline bool board_via_provider(Source src, const std::string& board_id, int page, int limit,
                               std::vector<Variant>& out, std::string& err, bool* more = nullptr, int* total = nullptr) {
    const char* key = provider_key_of(src);
    if (!key) { err = "平台不支持此目录接口"; return false; }
    std::vector<prov::Song> songs;
    if (!prov::Host::instance().board(key, board_id, page, limit, songs, err, more, total)) return false;
    out.clear();
    for (const auto& song : songs) {
        if (song.id.empty() || song.title.empty()) continue;
        Variant v;
        v.source = src;
        attach_provider_song(v, song);
        out.push_back(std::move(v));
    }
    return true;
}

// 榜单：provider 给的 Variant 直接包成 Track（榜单内不需要跨源合并）
inline std::vector<Track> board_track_page(Source src, const std::string& board_id, int page, int limit,
                                          bool& more, int& total, std::string& err) {
    std::vector<Variant> variants;
    std::vector<Track> out;
    if (!board_via_provider(src, board_id, page, limit, variants, err, &more, &total)) return out;
    out.reserve(variants.size());
    for (auto& v : variants) {
        Track t;
        t.title = v.title;
        t.artist = v.artist;
        t.album = v.album;
        t.duration_ms = v.duration_ms;
        t.cover_url = v.cover_url;
        t.chosen = 0;
        t.variants.push_back(std::move(v));
        out.push_back(std::move(t));
    }
    return out;
}

// 保存远端快照必须完整成功；分页失败、重复页或达到保护上限均不覆盖旧歌单。
inline std::vector<Track> board_tracks(Source src, const std::string& board_id, int limit, std::string& err) {
    std::vector<Track> out;
    std::set<std::string> ids;
    const int page_size = src == Source::Migu ? 50 : 100;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    net::CancelScope budget([parent = net::cancel_current, deadline] {
        return (parent && parent()) || std::chrono::steady_clock::now() >= deadline;
    });
    for (int page = 0; page < 201; ++page) {
        if (net::cancelled()) { err = "歌单读取已取消或超时，原快照保留"; return {}; }
        bool more = false;
        int total = -1;
        auto tracks = board_track_page(src, board_id, page, page_size, more, total, err);
        if (!err.empty()) return {};
        const auto before = out.size();
        for (auto& track : tracks) if (ids.insert(variant_uid(track.current())).second) out.push_back(std::move(track));
        if (out.size() > size_t(limit)) { err = "歌单超过安全上限，原快照保留"; return {}; }
        if (!more) {
            if (total >= 0 && out.size() < size_t(total)) { err = "平台未返回完整歌单，原快照保留"; return {}; }
            return out;
        }
        if (out.size() == before) { err = "平台返回重复或空白分页，原快照保留"; return {}; }
    }
    err = "歌单分页数量超限，原快照保留";
    return {};
}

// ---------------------------------------------------------------- 落雪音源取流
//
// 策略：先让落雪音源试，失败再回落内置实现。
// 音源只负责"给一条能用的直链"，歌还是 aino 自己搜的 —— 这样两边的 id 体系天然对齐。
inline const char* lx_platform_key(Source source) {
    const auto* p = platform_info(source); return p ? p->lx_key : nullptr;
}

inline std::string lx_interval(int ms) {
    if (ms <= 0) return "00:00";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", ms / 60000, (ms / 1000) % 60);
    return buf;
}

// 拼成 lx 音源认得的 musicInfo：各平台字段名不一样，别想当然用同一套
inline std::string lx_music_info(const Variant& v, const char* key) {
    std::string s = "{\"songmid\":" + lx::detail::js_quote(v.id);
    s += ",\"name\":" + lx::detail::js_quote(v.title);
    s += ",\"singer\":" + lx::detail::js_quote(v.artist);
    s += ",\"albumName\":" + lx::detail::js_quote(v.album);
    s += ",\"interval\":" + lx::detail::js_quote(lx_interval(v.duration_ms));
    s += ",\"source\":" + lx::detail::js_quote(key);
    if (v.source == Source::Kugou) {
        s += ",\"hash\":" + lx::detail::js_quote(v.native_id);
        s += ",\"albumId\":" + lx::detail::js_quote(v.album_id);
        s += ",\"albumAudioId\":" + lx::detail::js_quote(v.album_audio_id);
        s += ",\"_types\":{";
        bool first = true;
        for (const auto& h : v.quality_hashes) {
            if (!first) s += ","; first = false;
            s += lx::detail::js_quote(h.first) + ":{\"hash\":" + lx::detail::js_quote(h.second) + "}";
        }
        s += "}";
    } else if (v.source == Source::Migu) {
        s += ",\"copyrightId\":" + lx::detail::js_quote(v.native_id);
    }
    s += ",\"img\":" + lx::detail::js_quote(v.cover_url) + ",\"lrc\":null}";
    return s;
}

// ============================================================================
//  渠道一：第三方音源脚本（sources/*.js）
//
//  契约只有 musicUrl —— 它不管"从哪找歌"，只负责把 (平台, songmid) 换成直链。
//  免登录，档位由脚本自己决定（多源聚合脚本通常 320k / 无损都能给）。
// ============================================================================
inline bool resolve_via_source(const Variant& v, int quality, ResolveResult& out, std::string* why,
                               const ResolveAccept& accept = {}) {
    const char* key = lx_platform_key(v.source);
    if (!key) {
        if (why) *why = std::string(source_name(v.source)) + " 不在音源支持的平台内";
        return false;
    }
    auto& engine = lx::Engine::instance();
    // 启动恢复在后台进行；第一次点播等待恢复完成，切歌或退出可立即取消。
    while (engine.restoring() && !net::cancelled()) platform::sleep_ms(20);
    if (net::cancelled()) { if (why) *why = "取流已取消"; return false; }
    if (!engine.supports(key)) {
        if (why) *why = std::string("没有音源声明支持 ") + key;
        return false;
    }
    const std::string q = lx::quality_key(quality);
    const std::string info = lx_music_info(v, key);
    std::string url, err, by;
    auto timing = std::make_shared<lx::Engine::LoadTiming>();
    if (!engine.resolve(key, q, info, url, err, &by,
        [&](const std::string& candidate, const std::string& name, const std::string& actual_quality, std::string& error) {
            ResolveResult rr;
            rr.ok = true; rr.lane = Lane::JsSource; rr.engine = name; rr.url = candidate;
            rr.headers = {"User-Agent: Mozilla/5.0"}; rr.reason = "ok";
            rr.detail = "音源 · " + name + " · " + actual_quality;
            rr.load_timing = timing;
            if (!accept_resolved(rr, accept, error)) return false;
            timing->verified = bool(accept);
            out = std::move(rr);
            return true;
        }, timing.get())) {
        if (why) *why = err.empty() ? "音源没有返回链接" : err;
        return false;
    }
    return true;
}

// ============================================================================
//  渠道二：自带接口（providers/*.js → C++ 内置兜底）
//
//  平台接口的易变部分在 providers/*.js；这里只做调用与兜底。
//  登录态从这里注入，音质上限由账号决定。
// ============================================================================
inline bool resolve_via_builtin(const Variant& v, const Settings& st, ResolveResult& out, std::string* why,
                                const ResolveAccept& accept = {}) {
    if (net::cancelled()) { if (why) *why = "已取消"; return false; }
    prov::Host::instance().set_credentials(st.netease_cookie, st.qq_cookie);
    const std::string q = lx::quality_key(st.quality);

    // 1) 平台 provider 的可选取流入口（随附脚本未实现，继续走内置接口）
    if (const char* pkey = provider_key_of(v.source)) {
        if (prov::Host::instance().has(pkey)) {
            prov::Song song;
            song.id = v.id;
            song.title = v.title;
            song.artist = v.artist;
            song.album = v.album;
            song.native_id = v.native_id;
            song.duration_ms = v.duration_ms;
            std::string url, perr;
            if (prov::Host::instance().resolve(pkey, song, q, url, perr)) {
                out.ok = true;
                out.lane = Lane::Builtin;
                out.engine = std::string(pkey) + ".js";
                out.url = url;
                out.headers = {"User-Agent: Mozilla/5.0"};
                out.reason = "ok";
                out.detail = std::string("自带 · ") + out.engine + " · " + q;
                if (accept_resolved(out, accept, perr)) return true;
            }
            if (why) *why = perr;
        }
    }

    // 2) C++ 内置实现
    if (net::cancelled()) { if (why) *why = "已取消"; return false; }
    ResolveResult rr;
    switch (v.source) {
        case Source::Netease: rr = resolve_netease(v, st); break;
        case Source::QQ: rr = resolve_qq(v, st); break;
        case Source::Bilibili: rr = resolve_bilibili(v, st); break;
        default:
            if (why) *why = std::string("自带接口不提供 ") + source_name(v.source) + " 的取流";
            return false;
    }
    if (!rr.ok) {
        if (why) *why = rr.reason.empty() ? (rr.detail.empty() ? "自带接口没有返回链接" : rr.detail) : rr.reason;
        return false;
    }
    rr.lane = Lane::Builtin;
    if (rr.engine.empty()) rr.engine = "内置";
    // 内置实现自己给的是 "128k · mp3" 这种音质描述，补上前面的渠道与实现
    rr.detail = "自带 · " + rr.engine + (rr.detail.empty() ? " · " + q : " · " + rr.detail);
    out = std::move(rr);
    std::string error;
    if (!accept_resolved(out, accept, error)) { if (why) *why = error; return false; }
    return true;
}

// ============================================================================
//  渠道编排
//
//  按 Settings::lane_pref 的顺序依次尝试；两条渠道关掉的就不再问。
//  都失败时把两条的原因分别带回去 —— 不然用户只看到一句"失败了"，
//  根本不知道是音源没覆盖、还是登录态过期。
// ============================================================================
inline ResolveResult resolve(const Variant& v, const Settings& st, const ResolveAccept& accept = {}) {
    net::CancelScope network([parent = net::cancel_current, cancelled = st.cancelled] {
        return (parent && parent()) || (cancelled && cancelled());
    });
    std::string why[2];
    for (Lane lane : st.lane_order()) {
        if (net::cancelled()) { ResolveResult fail; fail.reason = "取流已取消"; return fail; }
        if (!st.lane_on(lane)) {
            why[int(lane)] = "已关闭";
            continue;
        }
        ResolveResult rr;
        std::string reason;
        const bool ok = lane == Lane::JsSource ? resolve_via_source(v, st.quality, rr, &reason, accept)
                                               : resolve_via_builtin(v, st, rr, &reason, accept);
        if (ok) return rr;
        why[int(lane)] = reason;
    }

    ResolveResult fail;
    fail.ok = false;
    if (st.lane_on(Lane::JsSource) && st.lane_on(Lane::Builtin) && !why[0].empty() && !why[1].empty()) {
        fail.reason = "音源：" + why[0] + "；自带：" + why[1];
    } else {
        fail.reason = why[0].empty() ? why[1] : why[0];
    }
    if (fail.reason.empty()) fail.reason = "没有可用的取流渠道";
    return fail;
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
    if (src == Source::Bilibili) got = search_bilibili(kw, 10, 6000, err);
    else if (is_platform(src)) got = search_platform(src, kw, 10, 6000, err, st);

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
        if (have_raw != want_raw) {
            // 标题不完全一样时，明写翻唱/伴奏/纯音乐的直接跳过：
            // 补位宁可失败，也不能把网友翻唱当成原曲放出来。
            static const char* kFake[] = {"cover", "翻唱", "伴奏", "纯音乐", "karaoke", "instrumental", "remix"};
            const std::string low = text::lower_ascii(have_raw);
            bool fake = false;
            for (const char* k : kFake) if (low.find(k) != std::string::npos) { fake = true; break; }
            if (fake) continue;
        }

        if (t.duration_ms > 0 && v.duration_ms > 0) {
            const double diff = double(v.duration_ms > t.duration_ms ? v.duration_ms - t.duration_ms
                                                                     : t.duration_ms - v.duration_ms) /
                                1000.0;
            if (diff > 20.0) continue;
            s += 30.0 - diff;
        }
        if (!want_artist.empty() && text::normalize_title(v.artist).find(want_artist) != std::string::npos)
            s += 20.0;
        // 这里曾经写 `if (!v.needs_login) s += 6.0;`（优先不用会员的那条）。
        // 后果是：正版原曲需要会员，而同名翻唱/网友上传版免费，于是补位经常落到翻唱上，
        // 用户听到的是"盗版"。宁可取不到流如实报错，也不能用免费与否来挑版本。
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


// 目录浏览只读取平台实体，不借用在线搜索结果或播放队列。
struct CatalogPage {
    CatalogTarget target;
    std::vector<Track> tracks;
    std::vector<CatalogTarget> albums;
    std::string description, error, continuation;
    int next_offset = 0;
    bool more = false;
};
inline bool valid_catalog_id(const std::string& id) {
    return !id.empty() && id.size() <= 160 &&
        id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos;
}
inline bool numeric_catalog_id(const std::string& id) {
    return !id.empty() && id.size() <= 24 && id.find_first_not_of("0123456789") == std::string::npos;
}
inline Track catalog_track(Source source, const js::Value& input) {
    const auto& song = input["musicData"].is_object() ? input["musicData"] :
        input["songInfo"].is_object() ? input["songInfo"] : input;
    Variant v; v.source = source;
    if (source == Source::Netease) {
        v.id = catalog_id(song["id"]); v.native_id = v.id;
        v.title = text::decode_entities(song["name"].str());
        const auto& artists = song["ar"].is_array() ? song["ar"] : song["artists"];
        for (const auto& artist : artists.array) {
            if (!v.artist.empty()) v.artist += " / "; v.artist += text::decode_entities(artist["name"].str());
        }
        const auto& album = song["al"].is_object() ? song["al"] : song["album"];
        v.album = text::decode_entities(album["name"].str()); v.cover_url = album["picUrl"].str();
        v.duration_ms = int(song["dt"].integer(song["duration"].integer()));
        const int fee = int(song["fee"].integer()); v.needs_login = fee == 1 || fee == 4;
    } else if (source == Source::QQ) {
        v.id = song["mid"].str(song["songmid"].str()); v.native_id = catalog_id(song["id"]);
        v.title = text::decode_entities(song["name"].str(song["songname"].str(song["title"].str())));
        for (const auto& artist : song["singer"].array) {
            if (!v.artist.empty()) v.artist += " / "; v.artist += text::decode_entities(artist["name"].str());
        }
        v.album = text::decode_entities(song["album"]["name"].str(song["albumname"].str()));
        v.duration_ms = int(song["interval"].integer()) * 1000;
        const auto mid = song["album"]["mid"].str(song["albummid"].str());
        if (!mid.empty()) v.cover_url = "https://y.gtimg.cn/music/photo_new/T002R300x300M000" + mid + ".jpg";
        v.needs_login = song["pay"]["pay_play"].integer() != 0;
    }
    attach_catalog_metadata(v, song);
    Track track; track.title = v.title; track.artist = v.artist; track.album = v.album;
    track.duration_ms = v.duration_ms; track.cover_url = v.cover_url;
    if (!v.id.empty() && !v.title.empty()) track.variants.push_back(std::move(v));
    return track;
}
inline bool catalog_qq_request(const Settings& st, const std::string& module, const std::string& method,
                              const std::string& params, js::Value& data, std::string& error) {
    js::Value root;
    const auto body = "{\"comm\":{\"ct\":24,\"cv\":0,\"format\":\"json\"},\"req\":{\"module\":" + providers::quote(module) +
        ",\"method\":" + providers::quote(method) + ",\"param\":" + params + "}}";
    if (!providers::json_response(net::post("https://u.y.qq.com/cgi-bin/musicu.fcg", body,
        {"Content-Type: application/json", "Referer: https://y.qq.com/", "User-Agent: Mozilla/5.0", "Cookie: " + st.qq_cookie}, 10000), root, error)) return false;
    if (root["req"]["code"].integer(-1) != 0) { error = "QQ 目录接口拒绝（code " + std::to_string(root["req"]["code"].integer(-1)) + "）"; return false; }
    data = root["req"]["data"]; return true;
}
inline bool hydrate_catalog_variant(Variant& variant, const Settings& st, std::string& error) {
    js::Value root;
    if (!valid_catalog_id(variant.id)) { error = "歌曲 ID 无效，无法读取目录资料"; return false; }
    if (variant.source == Source::Netease) {
        if (!numeric_catalog_id(variant.id)) { error = "网易云歌曲 ID 无效"; return false; }
        if (!providers::json_response(net::get("https://music.163.com/api/song/detail?ids=" + net::url_encode("[" + variant.id + "]"),
            {"Referer: https://music.163.com/", "User-Agent: Mozilla/5.0", "Cookie: " + st.netease_cookie}, 8000), root, error)) return false;
        if (root["code"].integer() != 200 || root["songs"].array.empty()) { error = "网易云未返回这首歌的资料"; return false; }
        const auto track = catalog_track(variant.source, root["songs"][size_t(0)]);
        if (track.variants.empty() || track.current().id != variant.id) { error = "网易云歌曲资料不完整或身份不符"; return false; }
        variant = track.current();
    } else if (variant.source == Source::QQ) {
        if (!catalog_qq_request(st, "music.pf_song_detail_svr", "get_song_detail_yqq",
            "{\"song_mid\":" + providers::quote(variant.id) + ",\"song_type\":0}", root, error)) return false;
        auto track = catalog_track(variant.source, root["track_info"]);
        if (track.variants.empty() || track.current().id != variant.id) { error = "QQ 未返回这首歌的资料"; return false; }
        variant = track.current();
    } else if (variant.source == Source::Bilibili) {
        if (!providers::json_response(net::get("https://api.bilibili.com/x/web-interface/view?bvid=" + net::url_encode(variant.id),
            {"Referer: https://www.bilibili.com/", "User-Agent: Mozilla/5.0"}, 8000), root, error)) return false;
        if (root["code"].integer(-1) != 0) { error = root["message"].str("B 站视频资料不可用"); return false; }
        const auto& owner = root["data"]["owner"];
        variant.artists = {{catalog_id(owner["mid"]), owner["name"].str()}};
    }
    return true;
}
inline bool catalog_target_from_track(const Track& track, CatalogKind kind, int artist_index, const Settings& st,
                                      CatalogTarget& target, std::string& error) {
    if (track.variants.empty()) { error = "没有曲目资料"; return false; }
    if (script_platform(track.current().source)) { error = "该平台目前支持搜索和榜单，尚未接入歌手与专辑目录"; return false; }
    auto variant = track.current();
    const auto displayed = track_artists(track);
    const auto wanted = artist_index >= 0 && artist_index < int(displayed.size()) ? displayed[size_t(artist_index)].name : std::string();
    target.source = variant.source; target.kind = kind;
    const bool missing = kind == CatalogKind::Album ? variant.album_id.empty() :
        artist_index < 0 || artist_index >= int(variant.artists.size()) || variant.artists[size_t(artist_index)].id.empty();
    if (variant.source != Source::Local && missing && !hydrate_catalog_variant(variant, st, error)) return false;
    if (kind == CatalogKind::Album) {
        target.id = variant.album_id; target.name = variant.album.empty() ? track.album : variant.album;
        target.cover = variant.cover_url.empty() ? track.cover_url : variant.cover_url;
        target.subtitle = variant.artist.empty() ? track.artist : variant.artist;
    } else {
        const auto exact = std::find_if(variant.artists.begin(), variant.artists.end(), [&](const auto& a) { return a.name == wanted; });
        if (exact != variant.artists.end()) { target.id = exact->id; target.name = exact->name; }
        else if (variant.source == Source::Local) target.name = wanted;
        else { error = "未能确认这位歌手的资料，未打开其他同名结果"; return false; }
    }
    if (target.name.empty()) { error = kind == CatalogKind::Artist ? "没有歌手信息" : "没有专辑信息"; return false; }
    if (target.source == Source::Local && kind == CatalogKind::Artist) target.id = target.name;
    if (target.source == Source::Local && kind == CatalogKind::Album) target.id = platform::fs::u8path(variant.id).parent_path().u8string();
    if (target.source != Source::Local && !valid_catalog_id(target.id)) { error = "平台没有提供可浏览的歌手或专辑 ID"; return false; }
    return true;
}
inline CatalogTarget catalog_album(Source source, const js::Value& album) {
    CatalogTarget out; out.source = source; out.kind = CatalogKind::Album;
    out.id = source == Source::QQ ? album["albumMid"].str(album["album_mid"].str(album["mid"].str())) :
        catalog_id(album["id"].is_null() ? album["albumid"] : album["id"]);
    out.name = text::decode_entities(album["name"].str(album["albumName"].str(album["album_name"].str())));
    out.cover = album["picUrl"].str(album["pic"].str(album["pic120"].str()));
    if (source == Source::QQ && !out.id.empty()) out.cover = "https://y.gtimg.cn/music/photo_new/T002R300x300M000" + out.id + ".jpg";
    out.subtitle = album["publishTime"].str(album["pubTime"].str(album["releaseDate"].str(album["pub_time"].str())));
    return out;
}
inline CatalogPage fetch_catalog(CatalogTarget target, const Settings& st, bool albums = false, int offset = 0,
                                 const std::string& continuation = {}) {
    CatalogPage out; out.target = std::move(target);
    auto& t = out.target;
    if (!valid_catalog_id(t.id) || offset < 0 || offset > 5000) { out.error = "目录参数无效或超过 5000 条"; return out; }
    js::Value root; const js::Value* list = nullptr;
    if (t.source == Source::Netease) {
        if (!numeric_catalog_id(t.id)) { out.error = "网易云目录 ID 无效"; return out; }
        const auto path = t.kind == CatalogKind::Album ? "album/" : albums ? "artist/albums/" : "artist/";
        if (!providers::json_response(net::get("https://music.163.com/api/" + std::string(path) + t.id +
            "?limit=50&offset=" + std::to_string(offset), {"Referer: https://music.163.com/", "User-Agent: Mozilla/5.0", "Cookie: " + st.netease_cookie}, 10000), root, out.error)) return out;
        if (root["code"].integer() != 200) { out.error = "网易云目录接口拒绝（code " + std::to_string(root["code"].integer()) + "）"; return out; }
        const auto& header = t.kind == CatalogKind::Album ? root["album"] : root["artist"];
        if (!header["name"].str().empty()) t.name = text::decode_entities(header["name"].str());
        if (!header["picUrl"].str().empty()) t.cover = header["picUrl"].str();
        out.description = header["briefDesc"].str(header["description"].str());
        if (albums) { list = &root["hotAlbums"]; out.more = root["more"].truthy(); }
        else list = t.kind == CatalogKind::Album ? &root["songs"] : &root["hotSongs"];
    } else if (t.source == Source::QQ) {
        if (t.kind == CatalogKind::Album) {
            if (!providers::json_response(net::get("https://c.y.qq.com/v8/fcg-bin/fcg_v8_album_info_cp.fcg?format=json&albummid=" + net::url_encode(t.id),
                {"Referer: https://y.qq.com/", "User-Agent: Mozilla/5.0", "Cookie: " + st.qq_cookie}, 10000), root, out.error)) return out;
            if (root["code"].integer(-1) != 0) { out.error = "QQ 专辑接口拒绝"; return out; }
            const auto& data = root["data"];
            if (!data["name"].str().empty()) t.name = text::decode_entities(data["name"].str());
            out.description = data["desc"].str(); list = &data["list"];
        } else {
            const auto params = "{\"singermid\":" + providers::quote(t.id) + (albums ? ",\"begin\":" : ",\"sin\":") +
                std::to_string(offset) + (albums ? ",\"num\":50,\"order\":\"time\",\"exstatus\":1}" : ",\"num\":50,\"sort\":5}");
            if (!catalog_qq_request(st, "music.web_singer_info_svr",
                albums ? "get_singer_album" : "get_singer_detail_info", params, root, out.error)) return out;
            list = albums ? &root["list"] : &root["songlist"];
            const auto total = (albums ? root["total"] : root["total_song"]).integer(-1);
            out.more = total >= 0 ? offset + int(list->size()) < total : list->size() == 50;
            const auto name = albums ? root["singer_name"].str() : root["singer_info"]["name"].str();
            if (!name.empty()) t.name = text::decode_entities(name);
            out.description = root["singer_brief"].str(root["desc"].str());
            t.cover = "https://y.gtimg.cn/music/photo_new/T001R300x300M000" + t.id + ".jpg";
        }
    } else if (t.source == Source::Bilibili && !albums && t.kind == CatalogKind::Artist) {
        if (!numeric_catalog_id(t.id)) { out.error = "B 站 UP 主 ID 无效"; return out; }
        const auto session = providers::bili_session(3000, out.error); if (session.key.empty()) return out;
        const auto query = providers::wbi_query({{"mid", t.id}, {"pn", std::to_string(offset / 30 + 1)}, {"ps", "30"}, {"order", "pubdate"}}, session.key, std::time(nullptr));
        if (!providers::json_response(net::get("https://api.bilibili.com/x/space/wbi/arc/search?" + query,
            {"Referer: https://www.bilibili.com/", "User-Agent: Mozilla/5.0", "Cookie: " + session.cookie}, 10000), root, out.error)) return out;
        if (root["code"].integer(-1) != 0) { out.error = root["message"].str("B 站 UP 主作品暂不可用"); return out; }
        list = &root["data"]["list"]["vlist"];
        for (const auto& item : list->array) {
            Variant v; v.source = t.source; v.id = item["bvid"].str(); v.title = providers::strip_tags(item["title"].str());
            v.artist = t.name; v.artists = {{t.id, t.name}}; v.cover_url = item["pic"].str();
            if (v.cover_url.rfind("//", 0) == 0) v.cover_url = "https:" + v.cover_url;
            v.duration_ms = providers::clock_ms(item["length"].str());
            Track track; track.title = v.title; track.artist = v.artist; track.cover_url = v.cover_url; track.duration_ms = v.duration_ms;
            if (!v.id.empty() && !v.title.empty()) { track.variants = {std::move(v)}; out.tracks.push_back(std::move(track)); }
        }
        out.next_offset = offset + int(list->size()); out.more = out.next_offset < root["data"]["page"]["count"].integer(); return out;
    } else { out.error = "此音源未提供这个目录"; return out; }
    if (!list || !list->is_array()) { out.error = "平台未返回目录列表，请稍后重试"; return out; }
    out.next_offset = offset + int(list->size()); out.more = out.more && !list->array.empty() && out.next_offset < 5000;
    for (const auto& row : list->array) {
        if (albums) {
            auto album = catalog_album(t.source, row);
            if (valid_catalog_id(album.id) && !album.name.empty()) out.albums.push_back(std::move(album));
        } else {
            auto track = catalog_track(t.source, row);
            if (track.variants.empty()) continue;
            if (t.kind == CatalogKind::Album) {
                auto& v = track.variants.front(); v.album_id = t.id; v.album = t.name; track.album = t.name;
                if (v.cover_url.empty()) { v.cover_url = t.cover; track.cover_url = t.cover; }
            }
            out.tracks.push_back(std::move(track));
        }
    }
    return out;
}

}  // namespace music
