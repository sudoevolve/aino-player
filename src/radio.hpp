// 漫游推荐适配；不拥有播放设备、队列或账号持久化。
#pragma once
#include "sources.hpp"

namespace radio {
struct Recommendations {
    std::vector<music::Track> tracks;
    std::string method, error, alias;
};

inline std::string identity(const music::Track &track) {
    const auto &variant = track.current();
    // 同一视频的分 P 仍属于同一个推荐种子，避免相关推荐重复插入它。
    return variant.source == music::Source::Bilibili ? std::string(music::source_key(variant.source)) + "#" + variant.id
                                                     : music::variant_uid(variant);
}

inline std::vector<std::string> artist_keys(const music::Track &track) {
    std::vector<std::string> keys;
    for (const auto &artist : music::track_artists(track)) {
        const auto key = music::text::normalize_title(artist.name);
        if (!key.empty() && std::find(keys.begin(), keys.end(), key) == keys.end())
            keys.push_back(key);
    }
    return keys;
}
inline bool shares_artist(const music::Track &a, const music::Track &b) {
    const auto left = artist_keys(a);
    const auto right = artist_keys(b);
    for (const auto &artist : left)
        if (std::find(right.begin(), right.end(), artist) != right.end())
            return true;
    return false;
}
inline std::vector<music::Track> different_artist_tracks(const music::Track &seed, std::vector<music::Track> tracks) {
    tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                [&](const music::Track &candidate) {
                                    return candidate.variants.empty() ||
                                           music::same_variant(seed.current(), candidate.current()) ||
                                           shares_artist(seed, candidate);
                                }),
                 tracks.end());
    return tracks;
}
inline music::Track track_from_variant(music::Variant variant) {
    music::Track track;
    track.title = variant.title;
    track.artist = variant.artist;
    track.album = variant.album;
    track.duration_ms = variant.duration_ms;
    track.cover_url = variant.cover_url;
    track.variants.push_back(std::move(variant));
    return track;
}

inline Recommendations recommend(const music::Track &seed, const music::Settings &settings,
                                 bool cross_platform_first = false) {
    Recommendations result;
    if (seed.variants.empty()) {
        result.error = "歌曲没有平台资料";
        return result;
    }
    prov::ExecutionTime budget(24000);
    if (net::cancelled()) {
        result.error = "相似推荐已取消";
        return result;
    }
    const auto &variant = seed.current();
    const auto source = variant.source;
    std::string native_error;
    const auto try_native = [&] {
        // 用户明确指定的当前种子仍可使用原平台；平台开关只限制跨平台反查。
        if (!music::is_platform(source) || net::cancelled())
            return false;
        if (source == music::Source::Bilibili) {
            js::Value root;
            if (music::providers::json_response(
                    net::get("https://api.bilibili.com/x/web-interface/archive/related?bvid=" +
                                 net::url_encode(variant.id),
                             {"Referer: https://www.bilibili.com/", "User-Agent: Mozilla/5.0"}, 8000),
                    root, native_error)) {
                if (root["code"].integer(-1) == 0 && root["data"].is_array()) {
                    for (const auto &row : root["data"].array) {
                        music::Variant item;
                        item.source = source;
                        item.id = row["bvid"].str();
                        item.title = music::providers::strip_tags(row["title"].str());
                        item.artist = row["owner"]["name"].str();
                        item.artists = {{music::catalog_id(row["owner"]["mid"]), item.artist}};
                        item.cover_url = row["pic"].str();
                        item.album = item.title;
                        if (row["cid"].integer() > 0)
                            item.native_id = std::to_string(row["cid"].integer());
                        item.duration_ms = int(std::clamp<long long>(row["duration"].integer(), 0, 2147483) * 1000);
                        if (!item.id.empty() && item.id != variant.id && !item.title.empty())
                            result.tracks.push_back(track_from_variant(std::move(item)));
                        if (result.tracks.size() == 30)
                            break;
                    }
                } else
                    native_error = root["message"].str("B 站相关推荐暂不可用");
            }
            result.method = "B 站相关推荐";
            result.tracks = different_artist_tracks(seed, std::move(result.tracks));
            if (!result.tracks.empty())
                return true;
        } else {
            const char *key = music::provider_key_of(source);
            if (key && prov::Host::instance().has_similar(key)) {
                music::songs_via_provider(source, "similar",
                                          "{\"id\":" + lx::detail::js_quote(variant.id) +
                                              ",\"native_id\":" + lx::detail::js_quote(variant.native_id) + "}",
                                          result.tracks, native_error);
                result.method = std::string(music::source_name(source)) + "相似推荐";
                result.tracks = different_artist_tracks(seed, std::move(result.tracks));
                if (!result.tracks.empty())
                    return true;
            }
        }
        return false;
    };
    if (!cross_platform_first && try_native())
        return result;

    // 搜索只用于定位同一录音的推荐种子，搜索结果本身不能代表风格相似。
    const std::array<music::Source, 2> targets{music::Source::Netease, music::Source::QQ};
    for (const auto target : targets) {
        if (net::cancelled())
            break;
        if (!settings.enable[int(target)] || target == source)
            continue;
        const char *key = music::provider_key_of(target);
        if (!key || !prov::Host::instance().has_similar(key))
            continue;
        std::string match_error;
        const auto matches =
            music::search_platform(target, seed.title + " " + seed.artist, 20, 6000, match_error, settings);
        if (!match_error.empty())
            native_error = match_error;
        for (const auto &match : matches) {
            if (net::cancelled())
                break;
            if (!music::matches_recording(seed, match))
                continue;
            std::string similar_error;
            music::songs_via_provider(target, "similar",
                                      "{\"id\":" + lx::detail::js_quote(match.id) +
                                          ",\"native_id\":" + lx::detail::js_quote(match.native_id) + "}",
                                      result.tracks, similar_error);
            result.tracks = different_artist_tracks(seed, std::move(result.tracks));
            result.method = std::string(music::source_name(target)) + "相似歌曲";
            if (!result.tracks.empty()) {
                result.alias = music::variant_uid(match);
                return result;
            }
            if (!similar_error.empty())
                native_error = similar_error;
            break;
        }
    }
    if (cross_platform_first && try_native())
        return result;
    result.error = native_error.empty() ? "暂未取得可用的相似歌曲，将稍后重试" : native_error;
    return result;
}
} // namespace radio
