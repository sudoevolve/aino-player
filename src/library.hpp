// 本地资料库只保存曲目元数据和在线歌单引用，不保存音频或账号凭据。
#pragma once
#include "sources.hpp"
#include "platform.hpp"
#include <fstream>
#include <sstream>
#include <regex>

namespace library {
inline std::string quote(const std::string& text) {
    std::string out = "\"";
    const char* hex = "0123456789abcdef";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += char(c);
    }
    return out + '"';
}
inline bool same_track(const music::Track& a, const music::Track& b) {
    for (const auto& x : a.variants) for (const auto& y : b.variants)
        if (x.source == y.source && !x.id.empty() && x.id == y.id) return true;
    return false;
}
inline std::string encode_track(const music::Track& t) {
    std::string out = "{\"title\":" + quote(t.title) + ",\"artist\":" + quote(t.artist) +
        ",\"album\":" + quote(t.album) + ",\"duration_ms\":" + std::to_string(t.duration_ms) +
        ",\"cover\":" + quote(t.cover_url) + ",\"chosen\":" + std::to_string(t.chosen) + ",\"variants\":[";
    bool first = true;
    for (const auto& v : t.variants) {
        if (!first) out += ',';
        first = false;
        out += "{\"source\":" + std::to_string(int(v.source)) + ",\"id\":" + quote(v.id) +
            ",\"native_id\":" + quote(v.native_id) + ",\"title\":" + quote(v.title) +
            ",\"artist\":" + quote(v.artist) + ",\"album\":" + quote(v.album) +
            ",\"cover\":" + quote(v.cover_url) + ",\"duration_ms\":" + std::to_string(v.duration_ms) +
            ",\"bitrate\":" + std::to_string(v.bitrate) + ",\"needs_login\":" + (v.needs_login ? "true" : "false") + "}";
    }
    return out + "]}";
}
inline bool decode_track(const js::Value& value, music::Track& t) {
    t.title = value["title"].str(); t.artist = value["artist"].str(); t.album = value["album"].str();
    t.cover_url = value["cover"].str(); t.duration_ms = int(value["duration_ms"].integer());
    const auto& variants = value["variants"];
    if (!variants.is_array() || variants.size() > 12) return false;
    for (const auto& row : variants.array) {
        const int source = int(row["source"].integer(-1));
        const auto id = row["id"].str();
        if (source < 0 || source >= music::kSourceCount || id.empty()) continue;
        music::Variant v;
        v.source = music::Source(source); v.id = id; v.native_id = row["native_id"].str();
        v.title = row["title"].str(t.title); v.artist = row["artist"].str(t.artist);
        v.album = row["album"].str(t.album); v.cover_url = row["cover"].str(t.cover_url);
        v.duration_ms = int(row["duration_ms"].integer(t.duration_ms));
        v.bitrate = int(row["bitrate"].integer()); v.needs_login = row["needs_login"].truthy();
        t.variants.push_back(std::move(v));
    }
    if (t.variants.empty()) return false;
    t.chosen = std::clamp(int(value["chosen"].integer()), 0, int(t.variants.size()) - 1);
    return true;
}
struct Playlist {
    std::string key, name, remote_id, cover;
    music::Source source = music::Source::Netease;
    std::vector<music::Track> tracks;
    std::string remote_owner; // QQ 自建歌单使用 dirid + uin；公开歌单使用 disstid。
    bool online() const { return !remote_id.empty(); }
};
inline bool parse_link(const std::string& input, music::Source& source, std::string& id) {
    // 只接受明确的官方长链接，不把短链接/任意 URL 当成 HTTP 请求。
    const auto text = music::text::trim(input);
    const std::regex netease(R"(^https?://music\.163\.com/(?:#/)?(?:playlist|m/playlist)(?:\?|/\?)[^\s]*\bid=([0-9]+)(?:[&#][^\s]*)?$)");
    const std::regex qq(R"(^https?://(?:y\.qq\.com|i\.y\.qq\.com)/(?:n/ryqq/playlist/|n2/m/share/details/taoge\.html\?(?:[^\s]*&)?id=|n/yqq/playlist/)([0-9]+)(?:\.html)?(?:[?&#][^\s]*)?$)");
    std::smatch match;
    if (std::regex_match(text, match, netease)) source = music::Source::Netease;
    else if (std::regex_match(text, match, qq)) source = music::Source::QQ;
    else return false;
    id = match[1].str();
    return id.size() <= 24 && id.find_first_not_of('0') != std::string::npos;
}
class Library {
public:
    std::vector<Playlist> playlists;
    Library() { reset(); }
    void reset() { playlists = {{"favorites", "本地收藏", {}, {}, music::Source::Netease, {}}}; }
    bool contains(const music::Track& track) const {
        for (const auto& t : playlists.front().tracks) if (same_track(t, track)) return true;
        return false;
    }
    bool toggle(const music::Track& track) {
        auto& tracks = playlists.front().tracks;
        const auto it = std::find_if(tracks.begin(), tracks.end(), [&](const auto& t) { return same_track(t, track); });
        if (it != tracks.end()) { tracks.erase(it); return false; }
        if (!track.variants.empty()) tracks.push_back(track);
        return true;
    }
    void upsert(Playlist playlist) {
        if (playlist.key.empty() || playlist.key == "favorites") return;
        for (auto& p : playlists) if (p.key == playlist.key) { p = std::move(playlist); return; }
        playlists.push_back(std::move(playlist));
    }
    bool save(const std::string& path, std::string& err) const {
        std::string out = "{\"version\":1,\"playlists\":[";
        bool first = true;
        for (const auto& p : playlists) {
            if (!first) out += ',';
            first = false;
            out += "{\"key\":" + quote(p.key) + ",\"name\":" + quote(p.name) +
                ",\"remote_id\":" + quote(p.remote_id) + ",\"cover\":" + quote(p.cover) +
                ",\"source\":" + std::to_string(int(p.source)) + ",\"remote_owner\":" + quote(p.remote_owner) + ",\"tracks\":[";
            bool first_track = true;
            for (const auto& t : p.tracks) {
                if (!first_track) out += ',';
                first_track = false; out += encode_track(t);
            }
            out += "]}";
        }
        out += "]}\n";
        const auto tmp = path + ".tmp";
        auto* f = platform::fopen(tmp, "wb");
        if (!f) { err = "无法创建本地资料库文件"; return false; }
        const auto written = std::fwrite(out.data(), 1, out.size(), f);
        const bool closed = std::fclose(f) == 0;
        if (written != out.size() || !closed || !platform::replace_file(tmp, path)) {
            platform::remove(tmp); err = "本地资料库保存失败，原文件保留"; return false;
        }
        return true;
    }
    bool load(const std::string& path, std::string& err) {
        if (!platform::exists(path)) return true;
        const auto n = platform::file_size(path);
        if (n <= 0 || n > 64 * 1024 * 1024) { err = "本地资料库大小异常，原文件保留"; return false; }
        std::ifstream f(platform::fs::u8path(path), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(f)), {});
        js::Value root;
        if (!js::parse(text, root) || root["version"].integer() != 1 || !root["playlists"].is_array()) {
            err = "本地资料库格式损坏，原文件保留"; return false;
        }
        Library loaded;
        for (const auto& row : root["playlists"].array) {
            Playlist p;
            p.key = row["key"].str(); p.name = row["name"].str(); p.remote_id = row["remote_id"].str(); p.cover = row["cover"].str();
            p.remote_owner = row["remote_owner"].str();
            const int source = int(row["source"].integer(-1));
            if (p.key.empty() || p.name.empty() || source < 0 || source >= music::kSourceCount || !row["tracks"].is_array()) {
                err = "本地资料库条目损坏，原文件保留"; return false;
            }
            p.source = music::Source(source);
            for (const auto& track : row["tracks"].array) {
                music::Track t;
                if (!decode_track(track, t)) { err = "本地曲目数据损坏，原文件保留"; return false; }
                p.tracks.push_back(std::move(t));
            }
            if (p.key == "favorites") loaded.playlists.front() = std::move(p);
            else loaded.upsert(std::move(p));
        }
        playlists = std::move(loaded.playlists); return true;
    }
};
} // namespace library
