// lyrics.hpp —— 歌词抓取与 LRC 解析
//
// 为什么只走网易云：实测三个源里只有它匿名给歌词。
//   * 网易云  /api/song/lyric?id=…           → 完整 LRC（可用）
//   * 酷我    m.kuwo.cn/…/songinfoandlrc      → status 301「音乐查询失败」（已下线）
//   * QQ      c.y.qq.com/…/fcg_query_lyric    → retcode 1101（要 g_tk / 登录）
// 所以酷我和 QQ 的曲目会用「歌名 + 歌手」反查网易云取歌词 —— 同一首歌的歌词
// 在各平台基本一致，这样三个源的曲目都能显示歌词。
#pragma once

#include <algorithm>
#include <cmath>
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

inline std::string find_netease_id(const music::Track& t, const music::Settings& st) {
    for (const auto& v : t.variants)
        if (v.source == music::Source::Netease && !v.id.empty()) return v.id;

    // 曲目不在网易云上（酷我独有 / QQ 独有）→ 按「歌名 + 歌手」反查。
    // 反查结果往往混着一堆翻唱（搜「花海 周杰伦」前两条都是 70 秒的改编版），
    // 所以判据要严格：**原始标题**完全相等才算强命中，时长差是主要过滤器。
    // 只用 normalize_title 不行 —— 它会把「（治愈版）」这类后缀抹掉，翻唱和原曲同分。
    std::string err;
    const auto got = music::search_netease(t.title + " " + t.artist, 10, 6000, err, st);
    const std::string want_raw = trim_copy(t.title);
    const std::string want = music::text::normalize_title(t.title);
    std::string best_id;
    double best_score = -1e9;
    for (const auto& v : got) {
        if (v.id.empty()) continue;
        const std::string have_raw = trim_copy(v.title);
        const std::string have = music::text::normalize_title(v.title);
        double s = 0.0;
        if (have_raw == want_raw) s += 60.0;
        else if (have == want) s += 8.0;   // 只是去括号后相同 —— 多半是某个改编版
        else if (have.find(want) != std::string::npos || want.find(have) != std::string::npos) s += 4.0;
        else continue;  // 标题都对不上，不可能是同一首

        if (t.duration_ms > 0 && v.duration_ms > 0) {
            const double diff = std::abs(double(v.duration_ms - t.duration_ms)) / 1000.0;
            if (diff > 15.0) continue;     // 同名但时长差 15 秒以上 → 不是同一版本
            s += 40.0 - diff * 2.5;
        }
        if (music::text::normalize_title(v.artist).find(music::text::normalize_title(t.artist)) !=
            std::string::npos)
            s += 20.0;
        if (s > best_score) {
            best_score = s;
            best_id = v.id;
        }
    }
    return best_id;
}

// ---------------------------------------------------------------- 各源直取
//
// 三家都有能匿名拿到明文 LRC 的接口，没必要只认一家：
//   网易云  /api/song/lyric?id=…                            → lrc.lyric
//   QQ      fcg_query_lyric_new.fcg?songmid=…&nobase64=1    → lyric（nobase64=1 才是明文）
//   酷我    m.kuwo.cn/newh5/singles/songinfoandlrc?musicId=… → data.lrclist[{time,lineLyric}]
// 酷我这条**必须走 https 且带 httpsStatus=1**：走 http 会回 status 301「音乐查询失败」。

inline Doc from_lrc(const std::string& raw, const char* origin) {
    Doc d;
    bool synced = true;
    d.lines = parse(raw, &synced);
    d.synced = synced;
    d.origin = origin;
    if (d.lines.empty()) d.error = "这首歌没有歌词";
    return d;
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
    return from_lrc(raw, "网易云");
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
    // 少数情况下外层裹着 MusicJsonCallback({...})，把回调名前缀切掉
    std::string body = r.body;
    const size_t brace = body.find('{');
    if (brace != std::string::npos && brace > 0) body = body.substr(brace);

    js::Value root;
    if (!js::parse(body, root, true)) {
        d.error = "歌词返回格式异常";
        return d;
    }
    const long long code = root["code"].integer(root["retcode"].integer(0));
    if (code != 0) {
        d.error = "QQ 歌词返回 code " + std::to_string(code);
        return d;
    }
    std::string raw = music::text::decode_entities(root["lyric"].str());
    return from_lrc(raw, "QQ");
}

inline Doc fetch_kuwo_lyric(const std::string& rid, int timeout_ms) {
    Doc d;
    d.origin = "酷我";
    if (rid.empty()) {
        d.error = "没有酷我 rid";
        return d;
    }
    const std::string url = "https://m.kuwo.cn/newh5/singles/songinfoandlrc?musicId=" + rid + "&httpsStatus=1";
    const net::Response r = net::get(url,
                                    {"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
                                     "Referer: https://www.kuwo.cn/"}, timeout_ms);
    if (!r.ok()) {
        d.error = "歌词请求失败（HTTP " + std::to_string(r.status) + "）";
        return d;
    }
    js::Value root;
    if (!js::parse(r.body, root, true)) {
        d.error = "歌词返回格式异常";
        return d;
    }
    const js::Value& list = root["data"]["lrclist"];
    if (!list.is_array() || list.size() == 0) {
        d.error = "这首歌没有歌词";
        return d;
    }
    for (size_t i = 0; i < list.size(); ++i) {
        Line l;
        l.ms = (long long)(std::atof(list[i]["time"].str().c_str()) * 1000.0 + 0.5);  // 秒（小数）→ 毫秒
        l.text = music::text::decode_entities(list[i]["lineLyric"].str());
        d.lines.push_back(std::move(l));
    }
    d.synced = true;
    return d;
}

// ---------------------------------------------------------------- 按名反查
//
// 三家都没有这首歌的歌词时（多见于酷我独有 / QQ 独有曲目），去网易云按
// 「歌名 + 歌手」反查一条 —— 同一首歌的歌词在各平台基本一致。
// 反查结果常混着翻唱（搜「花海 周杰伦」前两条是 70 秒改编版），所以判据要严格：
// **原始标题**完全相等才算强命中，时长差是主要过滤器。
inline std::string search_netease_id(const music::Track& t, const music::Settings& st) {
    std::string err;
    const auto got = music::search_netease(t.title + " " + t.artist, 10, 6000, err, st);
    const std::string want_raw = trim_copy(t.title);
    const std::string want = music::text::normalize_title(t.title);
    std::string best_id;
    double best_score = -1e9;
    for (const auto& v : got) {
        if (v.id.empty()) continue;
        const std::string have_raw = trim_copy(v.title);
        const std::string have = music::text::normalize_title(v.title);
        double s = 0.0;
        if (have_raw == want_raw) s += 60.0;
        else if (have == want) s += 8.0;
        else if (have.find(want) != std::string::npos || want.find(have) != std::string::npos) s += 4.0;
        else continue;

        if (t.duration_ms > 0 && v.duration_ms > 0) {
            const double diff = std::abs(double(v.duration_ms - t.duration_ms)) / 1000.0;
            if (diff > 15.0) continue;
            s += 40.0 - diff * 2.5;
        }
        if (music::text::normalize_title(v.artist).find(music::text::normalize_title(t.artist)) !=
            std::string::npos)
            s += 20.0;
        if (s > best_score) {
            best_score = s;
            best_id = v.id;
        }
    }
    return best_id;
}

inline Doc fetch(const music::Track& t, const music::Settings& st, int timeout_ms = 8000) {
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
        const int first = std::min<int>(t.chosen, int(t.variants.size()) - 1);
        order.push_back(first);
        for (int i = 0; i < int(t.variants.size()); ++i)
            if (i != first) order.push_back(i);
    }

    std::string last_err;
    for (int i : order) {
        const music::Variant& v = t.variants[size_t(i)];
        if (v.id.empty()) continue;
        Doc d;
        switch (v.source) {
            case music::Source::Netease: d = fetch_netease_lyric(v.id, st, timeout_ms); break;
            case music::Source::QQ: d = fetch_qq_lyric(v.id, st.qq_cookie, timeout_ms); break;
            default: continue;
        }
        if (!d.lines.empty()) return d;
        if (!d.error.empty()) last_err = std::string(music::source_name(v.source)) + "：" + d.error;
    }

    const std::string id = search_netease_id(t, st);
    if (!id.empty()) {
        Doc d = fetch_netease_lyric(id, st, timeout_ms);
        if (!d.lines.empty()) {
            d.origin = "网易云（按名反查）";
            return d;
        }
        if (!d.error.empty()) last_err = d.error;
    }

    Doc d;
    d.error = last_err.empty() ? "没有找到可用的歌词" : last_err;
    return d;
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
