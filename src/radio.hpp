// 漫游推荐适配；不拥有播放设备、队列或账号持久化。
#pragma once
#include "sources.hpp"

namespace radio {
struct Recommendations {
    std::vector<music::Track> tracks;
    std::string method, error, alias;
    bool searched = false, exhausted = false;
};

inline std::string identity(const music::Track& track) {
    const auto& variant = track.current();
    // 同一视频的分 P 仍属于同一个推荐种子，避免相关推荐重复插入它。
    return variant.source == music::Source::Bilibili ? std::string(music::source_key(variant.source)) + "#" + variant.id
        : music::variant_uid(variant);
}

inline std::string query(const music::Track& seed) {
    const auto artists = music::track_artists(seed);
    return artists.empty() ? seed.title : artists.front().name;
}
inline std::string query_key(const music::Track& seed) {
    return std::string(music::source_key(seed.current().source)) + "#" + music::text::normalize_title(query(seed));
}
inline music::Track track_from_variant(music::Variant variant) {
    music::Track track;
    track.title = variant.title; track.artist = variant.artist; track.album = variant.album;
    track.duration_ms = variant.duration_ms; track.cover_url = variant.cover_url;
    track.variants.push_back(std::move(variant));
    return track;
}

inline Recommendations recommend(const music::Track& seed, const music::Settings& settings, int page, bool explore) {
    Recommendations result;
    if (seed.variants.empty()) { result.error = "歌曲没有平台资料"; return result; }
    const auto& variant = seed.current();
    const auto source = variant.source;
    if (source == music::Source::Local) {
        std::string error;
        const auto candidates = music::search_netease(seed.title + " " + seed.artist, 20, 8000, error, settings);
        for (const auto& candidate : candidates) {
            auto reference = seed;
            if (reference.duration_ms <= 0 || candidate.duration_ms <= 0) reference.duration_ms = candidate.duration_ms;
            if (!candidate.id.empty() && music::same_song(reference, candidate)) {
                result = recommend(track_from_variant(candidate), settings, page, explore);
                result.alias = music::variant_uid(candidate);
                result.method = "本地歌曲 · " + result.method;
                return result;
            }
        }
        result.error = error.empty() ? "未找到歌名、歌手、版本与时长匹配的在线推荐种子" : error;
        return result;
    }
    std::string native_error;
    if (!explore && source == music::Source::Bilibili) {
        js::Value root;
        if (music::providers::json_response(net::get("https://api.bilibili.com/x/web-interface/archive/related?bvid=" +
            net::url_encode(variant.id), {"Referer: https://www.bilibili.com/", "User-Agent: Mozilla/5.0"}, 8000), root, native_error)) {
            if (root["code"].integer(-1) == 0 && root["data"].is_array()) {
                for (const auto& row : root["data"].array) {
                    music::Variant item; item.source = source;
                    item.id = row["bvid"].str(); item.title = music::providers::strip_tags(row["title"].str());
                    item.artist = row["owner"]["name"].str(); item.artists = {{music::catalog_id(row["owner"]["mid"]), item.artist}};
                    item.cover_url = row["pic"].str(); item.album = item.title;
                    if (row["cid"].integer() > 0) item.native_id = std::to_string(row["cid"].integer());
                    item.duration_ms = int(std::clamp<long long>(row["duration"].integer(), 0, 2147483) * 1000);
                    if (!item.id.empty() && item.id != variant.id && !item.title.empty())
                        result.tracks.push_back(track_from_variant(std::move(item)));
                    if (result.tracks.size() == 30) break;
                }
            } else native_error = root["message"].str("B 站相关推荐暂不可用");
        }
        result.method = "B 站相关推荐";
        if (!result.tracks.empty()) return result;
    } else if (!explore) {
        const char* key = music::provider_key_of(source);
        if (key && prov::Host::instance().has_similar(key)) {
            music::songs_via_provider(source, "similar", "{\"id\":" + lx::detail::js_quote(variant.id) +
                ",\"native_id\":" + lx::detail::js_quote(variant.native_id) + "}", result.tracks, native_error);
            result.method = std::string(music::source_name(source)) + "相似推荐";
            if (!result.tracks.empty()) return result;
        }
    }
    // 没有相似接口或推荐图暂时耗尽时，用原平台的歌手检索分页拓展候选。
    result.searched = true;
    result.method = std::string(music::source_name(source)) + "同歌手推荐";
    const auto keyword = query(seed);
    if (keyword.empty()) { result.error = "歌曲缺少可用于推荐的歌名或歌手"; return result; }
    std::string search_error;
    auto candidates = source == music::Source::Bilibili
        ? music::search_bilibili(keyword, 20, 9000, search_error, page)
        : music::search_platform(source, keyword, 20, 9000, search_error, settings, page);
    result.exhausted = candidates.size() < 20;
    const auto artists = music::text::split_artists(seed.artist);
    for (auto& candidate : candidates) {
        if (candidate.id.empty() || candidate.title.empty() || music::same_variant(variant, candidate)) continue;
        bool same_artist = artists.empty();
        for (const auto& artist : music::text::split_artists(candidate.artist))
            if (!artist.empty() && std::find(artists.begin(), artists.end(), artist) != artists.end()) same_artist = true;
        if (same_artist) result.tracks.push_back(track_from_variant(std::move(candidate)));
    }
    if (!search_error.empty()) result.error = native_error.empty() ? search_error : native_error + "；" + search_error;
    return result;
}
} // namespace radio
