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
    return seed.title;
}
inline std::string query_key(const music::Track& seed) {
    return std::string(music::source_key(seed.current().source)) + "#" + music::text::normalize_title(query(seed));
}
inline std::vector<std::string> artist_keys(const music::Track& track) {
    std::vector<std::string> keys;
    for (const auto& artist : music::track_artists(track)) {
        const auto key = music::text::normalize_title(artist.name);
        if (!key.empty() && std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
    }
    return keys;
}
inline bool shares_artist(const music::Track& a, const music::Track& b) {
    const auto left = artist_keys(a);
    const auto right = artist_keys(b);
    for (const auto& artist : left)
        if (std::find(right.begin(), right.end(), artist) != right.end()) return true;
    return false;
}
inline std::vector<music::Track> different_artist_tracks(const music::Track& seed, std::vector<music::Track> tracks) {
    tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [&](const music::Track& candidate) {
        return candidate.variants.empty() || music::same_variant(seed.current(), candidate.current()) || shares_artist(seed, candidate);
    }), tracks.end());
    return tracks;
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
        result.tracks = different_artist_tracks(seed, std::move(result.tracks));
        if (!result.tracks.empty()) return result;
    } else if (!explore) {
        const char* key = music::provider_key_of(source);
        if (key && prov::Host::instance().has_similar(key)) {
            music::songs_via_provider(source, "similar", "{\"id\":" + lx::detail::js_quote(variant.id) +
                ",\"native_id\":" + lx::detail::js_quote(variant.native_id) + "}", result.tracks, native_error);
            result.method = std::string(music::source_name(source)) + "相似推荐";
            result.tracks = different_artist_tracks(seed, std::move(result.tracks));
            if (!result.tracks.empty()) return result;
        }
    }

    if (!explore && source != music::Source::Netease && settings.enable[int(music::Source::Netease)]) {
        std::string match_error;
        const auto matches = music::search_netease(seed.title + " " + seed.artist, 20, 8000, match_error, settings);
        for (const auto& match : matches) {
            auto reference = seed;
            if (reference.duration_ms <= 0 || match.duration_ms <= 0) reference.duration_ms = match.duration_ms;
            if (match.id.empty() || !music::same_song(reference, match)) continue;
            std::string similar_error;
            music::songs_via_provider(music::Source::Netease, "similar",
                "{\"id\":" + lx::detail::js_quote(match.id) + "}", result.tracks, similar_error);
            result.tracks = different_artist_tracks(seed, std::move(result.tracks));
            result.method = "网易云相似歌曲";
            if (!result.tracks.empty()) return result;
            native_error = similar_error;
            break;
        }
    }

    // Expand by title as a last resort, never by singer.
    result.searched = true;
    result.method = std::string(music::source_name(source)) + "标题拓展";
    const auto keyword = query(seed);
    if (keyword.empty()) { result.error = "歌曲缺少可用于推荐的歌名或歌手"; return result; }
    std::string search_error;
    auto candidates = source == music::Source::Bilibili
        ? music::search_bilibili(keyword, 20, 9000, search_error, page)
        : music::search_platform(source, keyword, 20, 9000, search_error, settings, page);
    result.exhausted = candidates.size() < 20;
    for (auto& candidate : candidates) {
        if (candidate.id.empty() || candidate.title.empty() || music::same_variant(variant, candidate)) continue;
        auto track = track_from_variant(std::move(candidate));
        if (!shares_artist(seed, track)) result.tracks.push_back(std::move(track));
    }
    if (!search_error.empty()) result.error = native_error.empty() ? search_error : native_error + "；" + search_error;
    return result;
}
} // namespace radio
