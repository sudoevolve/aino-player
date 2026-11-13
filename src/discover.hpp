// 发现页的视图数据与平台目录适配；HTTP 协议由 providers/*.js 统一维护。
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

inline Heats heats(music::Source source, int timeout_ms = 8000) {
    Heats out; js::Value rows;
    prov::ExecutionTime budget(timeout_ms);
    const auto* key = music::provider_key_of(source);
    if (!key) { out.err = "该平台没有热搜目录"; return out; }
    if (!prov::Host::instance().request(key, "heats", "{}", rows, out.err)) return out;
    if (!rows.is_array()) { out.err = "热搜响应格式无效"; return out; }
    for (const auto& row : rows.array) {
        auto text = music::text::decode_entities(row["text"].str());
        if (!text.empty()) out.words.push_back({std::move(text), row["heat"].integer()});
        if (out.words.size() >= 50) break;
    }
    if (out.words.empty()) out.err = "暂无热搜";
    return out;
}
inline std::vector<Card> playlists(music::Source source, int limit, std::string& err, int timeout_ms = 10000) {
    std::vector<Card> out; js::Value rows;
    prov::ExecutionTime budget(timeout_ms);
    const auto* key = music::provider_key_of(source);
    if (!key) { err = "该平台没有发现目录"; return out; }
    if (!prov::Host::instance().request(key, "playlists", "{\"limit\":" + std::to_string(limit) + "}", rows, err)) return out;
    if (!rows.is_array()) { err = "歌单响应格式无效"; return out; }
    for (const auto& row : rows.array) {
        Card c; c.source = source; c.id = row["id"].str();
        c.name = music::text::decode_entities(row["name"].str()); c.cover = row["cover"].str();
        c.creator = music::text::decode_entities(row["creator"].str()); c.official = row["official"].truthy();
        c.plays = row["plays"].integer(); c.count = int(row["count"].integer());
        if (!c.id.empty() && !c.name.empty()) out.push_back(std::move(c));
        if (int(out.size()) >= limit) break;
    }
    if (out.empty()) err = "暂无歌单或榜单";
    return out;
}
} // namespace disc
