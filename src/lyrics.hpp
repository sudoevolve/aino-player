// lyrics.hpp —— 歌词抓取与 LRC 解析
//
// 优先使用曲目所属平台的歌词；失败后按标题、歌手和时长反查网易云。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "json.hpp"
#include "net.hpp"
#include "sources.hpp"
#include "platform.hpp"
#include <fstream>

namespace lyr {

struct Line {
    long long ms = 0;
    std::string text;
    std::string translation;
};

struct Doc {
    std::vector<Line> lines;
    std::string origin;      // 歌词来自哪个源（展示用）
    std::string error;       // 非空 = 没拿到，界面照原样显示这句话
    bool synced = true;      // false = 纯文本歌词，没有时间轴

    bool empty() const { return lines.empty(); }
};

// ---------------------------------------------------------------- LRC 解析

// 取 [mm:ss.xx] / [mm:ss:xx] / [mm:ss] 的时间，返回毫秒；失败返回 -1
inline long long parse_stamp(const std::string& s, size_t from, size_t to) {
    long long mm = 0, ss = 0, ms = 0;
    int part = 0;
    size_t digits = 0;
    long long* dst[3] = {&mm, &ss, &ms};
    for (size_t i = from; i < to; ++i) {
        const char c = s[i];
        if (c >= '0' && c <= '9') {
            *dst[part] = *dst[part] * 10 + (c - '0');
            ++digits;
        } else if (c == ':' || c == '.') {
            if (part < 2) ++part;
        } else {
            return -1;  // [ti:xxx] 这类元信息标签会走到这里
        }
    }
    if (digits == 0) return -1;
    // 小数位按位数换算：.5 → 500ms，.53 → 530ms，.532 → 532ms
    if (part == 2) {
        int scale = 1;
        for (size_t i = from; i < to; ++i) {
            if (s[i] == '.') {
                size_t n = 0;
                for (size_t k = i + 1; k < to; ++k)
                    if (s[k] >= '0' && s[k] <= '9') ++n;
                for (size_t k = 0; k < n; ++k) scale *= 10;
                break;
            }
        }
        ms = scale > 0 ? (ms * 1000) / scale : ms;
    }
    return mm * 60000 + ss * 1000 + ms;
}

inline std::string trim_copy(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

// 一行可以有多个时间标签（[00:12.00][01:20.00]词），每个标签都要生成一行
inline std::vector<Line> parse(const std::string& raw, bool* synced_out = nullptr) {
    std::vector<Line> out;
    size_t pos = 0;
    bool any_stamp = false;
    while (pos <= raw.size()) {
        size_t eol = raw.find('\n', pos);
        if (eol == std::string::npos) eol = raw.size();
        const std::string line = raw.substr(pos, eol - pos);
        pos = eol + 1;

        // 收集行首连续的时间标签
        size_t i = 0;
        std::vector<long long> stamps;
        while (i < line.size() && line[i] == '[') {
            const size_t close = line.find(']', i);
            if (close == std::string::npos) break;
            const long long t = parse_stamp(line, i + 1, close);
            if (t < 0) break;  // [ti:] / [ar:] 等，不是时间
            stamps.push_back(t);
            i = close + 1;
        }
        const std::string text = trim_copy(line.substr(i));
        if (stamps.empty()) {
            // 没有时间标签但也不是空行 → 当作纯文本歌词（很多中文歌词文件这样）
            if (text.empty()) continue;
            if (text[0] == '[') continue;  // 元信息行，丢掉
            Line l;
            l.ms = -1;
            l.text = text;
            out.push_back(std::move(l));
            continue;
        }
        any_stamp = true;
        for (long long t : stamps) {
            Line l;
            l.ms = t;
            l.text = text;
            out.push_back(std::move(l));
        }
    }

    if (any_stamp) {
        // 多个时间标签会产生乱序（同一行在不同位置重复），按时间排一下
        std::stable_sort(out.begin(), out.end(), [](const Line& a, const Line& b) {
            const long long ka = a.ms < 0 ? 0 : a.ms, kb = b.ms < 0 ? 0 : b.ms;
            return ka < kb;
        });
        // 纯空白的行（间奏）保留，它们给界面留出呼吸感
    }
    if (synced_out) *synced_out = any_stamp;
    return out;
}

// ---------------------------------------------------------------- 取歌词

// ---------------------------------------------------------------- 各源直取
//
// 网易云与 QQ 的歌词接口：
//   网易云  /api/song/lyric?id=…                            → lrc.lyric
//   QQ      fcg_query_lyric_new.fcg?songmid=…&nobase64=1    → lyric（nobase64=1 才是明文）

inline Doc from_lrc(const std::string& raw, const char* origin) {
    Doc d;
    bool synced = true;
    auto text = raw;
    if (text.rfind("\xef\xbb\xbf", 0) == 0) text.erase(0, 3);
    d.lines = parse(text, &synced);
    d.synced = synced;
    d.origin = origin;
    if (d.lines.empty()) d.error = "这首歌没有歌词";
    return d;
}

// 译文只按时间轴关联，不能用行号配对（间奏、多时间标签会改变行数）。
inline void add_translation(Doc& doc, const std::string& raw) {
    const auto translated = parse(raw.rfind("\xef\xbb\xbf", 0) == 0 ? raw.substr(3) : raw);
    for (const auto& line : translated) {
        if (line.ms < 0 || line.text.empty()) continue;
        auto first = std::lower_bound(doc.lines.begin(), doc.lines.end(), line.ms - 50,
            [](const Line& a, long long time) { return std::max(0LL, a.ms) < time; });
        auto best = doc.lines.end();
        for (auto it = first; it != doc.lines.end() && it->ms <= line.ms + 50; ++it)
            if (it->ms >= 0 && !it->text.empty() && (best == doc.lines.end() || std::llabs(it->ms - line.ms) < std::llabs(best->ms - line.ms))) best = it;
        if (best != doc.lines.end() && best->text != line.text) {
            if (!best->translation.empty()) best->translation += " / ";
            best->translation += line.text;
        }
    }
}

inline Doc fetch_netease_lyric(const std::string& id, const music::Settings& st, int timeout_ms) {
    Doc d;
    d.origin = "网易云";
    if (id.empty()) {
        d.error = "没有网易云 id";
        return d;
    }
    std::vector<std::string> headers = {"Referer: https://music.163.com/",
                                        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)"};
    headers.push_back("Cookie: appver=2.0.2; os=pc" +
                      (st.netease_cookie.empty() ? std::string() : "; " + st.netease_cookie));

    const std::string url = "https://music.163.com/api/song/lyric?id=" + id + "&lv=-1&kv=-1&tv=-1&rv=-1";
    const net::Response r = net::get(url, headers, timeout_ms);
    if (!r.ok()) {
        d.error = "歌词请求失败（HTTP " + std::to_string(r.status) + "）";
        return d;
    }
    js::Value root;
    if (!js::parse(r.body, root, true)) {
        d.error = "歌词返回格式异常";
        return d;
    }
    std::string raw = root["lrc"]["lyric"].str();
    if (trim_copy(raw).empty()) raw = root["tlyric"]["lyric"].str();  // 少数条目只在翻译里有时间轴
    auto result = from_lrc(raw, "网易云");
    add_translation(result, root["tlyric"]["lyric"].str());
    return result;
}

// QQ 的原文 / 译文可能分别为明文或 Base64，不能只凭 nobase64 判断两者。
inline std::string decode_lyric_text(const std::string& raw) {
    auto text = music::text::decode_entities(raw);
    if (text.find('[') != std::string::npos) return text;
    auto compact = text;
    compact.erase(std::remove_if(compact.begin(), compact.end(), [](char ch) { return ch == '\r' || ch == '\n'; }), compact.end());
    if (!compact.empty() && compact.size() % 4 == 0 && compact.find_first_not_of(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") == std::string::npos) {
        auto decoded = lx::detail::base64_decode(compact);
        if (decoded.find('[') != std::string::npos || decoded.find('\n') != std::string::npos)
            return music::text::decode_entities(decoded);
    }
    return text;
}
inline Doc qq_lyric_document(const std::string& response) {
    const auto first = response.find('{'), last = response.rfind('}');
    js::Value root;
    if (first == std::string::npos || last == std::string::npos || last < first ||
        !js::parse(response.substr(first, last - first + 1), root, true)) {
        Doc d; d.origin = "QQ"; d.error = "QQ 歌词返回格式异常"; return d;
    }
    const auto code = root["code"].integer(root["retcode"].integer());
    if (code != 0) { Doc d; d.origin = "QQ"; d.error = "QQ 歌词返回 code " + std::to_string(code); return d; }
    auto doc = from_lrc(decode_lyric_text(root["lyric"].str()), "QQ");
    add_translation(doc, decode_lyric_text(root["trans"].str()));
    return doc;
}

inline Doc fetch_qq_lyric(const std::string& mid, const std::string& cookie, int timeout_ms) {
    Doc d;
    d.origin = "QQ";
    if (mid.empty()) {
        d.error = "没有 QQ 歌曲 mid";
        return d;
    }
    // nobase64=1 → 直接给明文 LRC，省掉自己解 base64 的一步
    const std::string url = "https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg?songmid=" + mid +
                            "&format=json&nobase64=1&g_tk=5381&loginUin=0&hostUin=0&platform=yqq"
                            "&needNewCode=0&inCharset=utf8&outCharset=utf-8";
    std::vector<std::string> headers = {"Referer: https://y.qq.com/portal/player.html",
                                        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)"};
    if (!cookie.empty()) headers.push_back("Cookie: " + cookie);

    const net::Response r = net::get(url, headers, timeout_ms);
    if (!r.ok()) {
        d.error = "歌词请求失败（HTTP " + std::to_string(r.status) + "）";
        return d;
    }
    return qq_lyric_document(r.body);
}

inline Doc fetch_provider_lyric(const music::Variant& v, int timeout_ms) {
    Doc doc; doc.origin = music::source_name(v.source);
    const auto* key = music::provider_key_of(v.source);
    if (!key) { doc.error = "此平台未提供歌词接口"; return doc; }
    prov::ExecutionTime budget(timeout_ms);
    js::Value result;
    const auto args = "{\"id\":" + lx::detail::js_quote(v.id) + ",\"native_id\":" + lx::detail::js_quote(v.native_id) +
        ",\"title\":" + lx::detail::js_quote(v.title) + ",\"duration_ms\":" + std::to_string(v.duration_ms) + "}";
    if (!prov::Host::instance().request(key, "lyric", args, result, doc.error)) return doc;
    doc = from_lrc(decode_lyric_text(result["lyric"].str()), music::source_name(v.source));
    add_translation(doc, decode_lyric_text(result["translation"].str()));
    return doc;
}

inline bool has_translation(const Doc& doc) {
    return std::any_of(doc.lines.begin(), doc.lines.end(), [](const Line& line) { return !line.translation.empty(); });
}
inline bool needs_translation(const Doc& doc) {
    int foreign = 0, count = 0;
    for (const auto& line : doc.lines) {
        if (line.text.empty()) continue;
        ++count; int latin = 0, cjk = 0; bool kana_or_hangul = false;
        for (size_t i = 0; i < line.text.size();) {
            const auto lead = static_cast<unsigned char>(line.text[i++]);
            unsigned cp = lead;
            int continuation = 0;
            if (lead >= 0xc2 && lead <= 0xdf) { cp &= 0x1f; continuation = 1; }
            else if (lead >= 0xe0 && lead <= 0xef) { cp &= 0x0f; continuation = 2; }
            else if (lead >= 0xf0 && lead <= 0xf4) { cp &= 7; continuation = 3; }
            else if (lead >= 0x80) continue;
            bool valid = i + continuation <= line.text.size();
            for (int n = 0; valid && n < continuation; ++n) {
                const auto next = static_cast<unsigned char>(line.text[i]);
                if ((next & 0xc0) != 0x80) valid = false;
                else { cp = (cp << 6) | (next & 0x3f); ++i; }
            }
            if (!valid) continue;
            if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) ++latin;
            if (cp >= 0x4e00 && cp <= 0x9fff) ++cjk;
            kana_or_hangul |= (cp >= 0x3040 && cp <= 0x30ff) || (cp >= 0xac00 && cp <= 0xd7af) || (cp >= 0x0400 && cp <= 0x052f);
        }
        if (kana_or_hangul || (latin >= 4 && cjk == 0)) ++foreign;
    }
    return !has_translation(doc) && foreign >= 3 && foreign * 3 >= count;
}
// 歌词匹配保留括号内的正文，不能使用会删除版本注释的歌曲标题规范化。
inline std::string line_key(const std::string& text) {
    const auto folded = music::text::lower_ascii(music::text::fold_width(text));
    std::string key;
    for (unsigned char ch : folded)
        if (ch >= 0x80 || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) key += char(ch);
    return key;
}
// 跨平台时间戳可能不同：只给原文一致的行补译文，不把别的平台时间轴强套过来。
inline bool supplement_translation(Doc& primary, const Doc& translated) {
    struct Match { size_t target; const Line* source; };
    std::vector<Match> matches;
    std::set<size_t> used;
    int eligible = 0;
    std::vector<std::string> keys;
    keys.reserve(translated.lines.size());
    for (const auto& line : translated.lines) keys.push_back(line_key(line.text));
    for (size_t i = 0; i < primary.lines.size(); ++i) {
        const auto& line = primary.lines[i];
        if (line.text.empty() || !line.translation.empty()) continue;
        ++eligible;
        const auto key = line_key(line.text);
        if (key.empty()) continue;
        size_t best = translated.lines.size(); long long distance = 5001;
        for (size_t j = 0; j < translated.lines.size(); ++j) {
            const auto& other = translated.lines[j];
            if (used.count(j) || other.translation.empty() || key != keys[j]) continue;
            const auto delta = std::llabs(line.ms - other.ms);
            if (delta < distance) { best = j; distance = delta; }
        }
        if (best != translated.lines.size()) { used.insert(best); matches.push_back({i, &translated.lines[best]}); }
    }
    if (matches.size() < 3 || matches.size() * 3 < size_t(eligible)) return false;
    for (const auto& match : matches) primary.lines[match.target].translation = match.source->translation;
    primary.origin += " · 译文：" + translated.origin;
    return true;
}

// ---------------------------------------------------------------- 按名反查
//
// 平台没有返回这首歌的歌词时，去网易云按
// 「歌名 + 歌手」反查一条 —— 同一首歌的歌词在各平台基本一致。
// 反查结果常混着翻唱（搜「花海 周杰伦」前两条是 70 秒改编版），所以判据要严格：
// **原始标题**完全相等才算强命中，时长差是主要过滤器。
inline std::string search_netease_id(const music::Track& t, const music::Settings& st) {
    const auto title = music::text::normalize_title(t.title);
    const auto artists = music::text::split_artists(t.artist);
    if (title.empty() || artists.empty()) return {};
    std::string err;
    const auto got = music::search_netease(t.title + " " + t.artist, 16, 6000, err, st);
    std::string best_id; double best_score = -1;
    for (const auto& v : got) {
        if (v.id.empty() || music::text::normalize_title(v.title) != title) continue;
        const auto candidates = music::text::split_artists(v.artist);
        bool same_artist = false;
        for (const auto& a : artists) for (const auto& b : candidates) if (!a.empty() && a == b) same_artist = true;
        if (!same_artist) continue;
        double score = 100;
        if (t.duration_ms > 0 && v.duration_ms > 0) {
            const double delta = std::abs(double(t.duration_ms) - v.duration_ms);
            if (delta > 5000) continue;
            score -= delta / 1000;
        }
        if (score > best_score) { best_score = score; best_id = v.id; }
    }
    return best_id;
}

inline Doc fetch(const music::Track& t, const music::Settings& st, int timeout_ms = 8000,
                 const std::function<void(const Doc&)>& publish = {}) {
    prov::ExecutionTime total_budget(std::max(1000, timeout_ms) * 3);
    prov::Host::instance().set_credentials(st.netease_cookie, st.qq_cookie);
    if (!t.variants.empty() && t.current().source == music::Source::Local) {
        Doc d; d.origin = "本地 LRC";
        auto path = platform::fs::u8path(t.current().id); path.replace_extension(".lrc");
        const auto size = platform::file_size(path.u8string());
        if (size > 0 && size < 4 * 1024 * 1024) {
            std::ifstream f(path, std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(f)), {});
            if (text.rfind("\xef\xbb\xbf", 0) == 0) text.erase(0, 3);
            d.lines = parse(text, &d.synced);
        }
        if (d.lines.empty()) d.error = "未找到同名 UTF-8 .lrc 歌词";
        return d;
    }
    // 顺序：当前选中的源 → 其它源。每家用自己那条曲目的 id，不去猜。
    std::vector<int> order;
    if (!t.variants.empty()) {
        const int first = std::clamp<int>(t.chosen, 0, int(t.variants.size()) - 1);
        order.push_back(first);
        for (int i = 0; i < int(t.variants.size()); ++i)
            if (i != first) order.push_back(i);
    }

    std::string last_err; Doc primary; std::set<std::string> tried_netease;
    auto take = [&](Doc doc) {
        if (doc.empty()) { if (!doc.error.empty()) last_err = doc.origin + "：" + doc.error; return false; }
        if (primary.empty()) { primary = std::move(doc); if (publish) publish(primary); }
        else if (has_translation(doc)) supplement_translation(primary, doc);
        return !needs_translation(primary);
    };
    for (int i : order) {
        if (net::cancelled()) break;
        const auto& v = t.variants[size_t(i)];
        if (v.id.empty()) continue;
        Doc doc;
        switch (v.source) {
            case music::Source::Netease: tried_netease.insert(v.id); doc = fetch_netease_lyric(v.id, st, timeout_ms); break;
            case music::Source::QQ: doc = fetch_qq_lyric(v.id, st.qq_cookie, timeout_ms); break;
            case music::Source::Kuwo: case music::Source::Kugou: case music::Source::Migu:
                doc = fetch_provider_lyric(v, timeout_ms); break;
            default: continue;
        }
        if (take(std::move(doc))) return primary;
    }
    if (!net::cancelled()) {
        const auto id = search_netease_id(t, st);
        if (!id.empty() && !tried_netease.count(id)) {
            auto doc = fetch_netease_lyric(id, st, timeout_ms);
            doc.origin = "网易云（同曲匹配）";
            take(std::move(doc));
        }
    }
    if (!primary.empty()) return primary;
    primary.error = net::cancelled() ? "歌词加载已取消" : last_err.empty() ? "没有找到可用的歌词" : last_err;
    return primary;
}


// 把解析后的行还原成一份 LRC 文本（内嵌歌词要用）：
// 有时间的行写 [mm:ss.xx]，纯文本行原样输出。
inline std::string to_lrc(const Doc& d) {
    std::string out;
    char buf[32];
    for (const auto& l : d.lines) {
        if (l.ms >= 0) {
            const long long m = l.ms / 60000;
            const long long sec = (l.ms / 1000) % 60;
            const long long cs = (l.ms % 1000) / 10;
            std::snprintf(buf, sizeof buf, "[%02lld:%02lld.%02lld]", m, sec, cs);
            out += buf;
            out += l.text;
        } else {
            out += l.text;
        }
        out += "\r\n";
        if (!l.translation.empty()) {
            if (l.ms >= 0) out += buf;
            out += l.translation + "\r\n";
        }
    }
    return out;
}

// 给定播放位置，返回当前行（最后一个 time <= pos 的行）；没有就 -1
inline int current_line(const std::vector<Line>& lines, long long pos_ms) {
    int idx = -1;
    for (int i = 0; i < int(lines.size()); ++i) {
        if (lines[size_t(i)].ms < 0) continue;
        if (lines[size_t(i)].ms <= pos_ms) idx = i;
        else break;
    }
    return idx;
}

}  // namespace lyr
