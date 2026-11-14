// 本地资料库只保存曲目元数据和在线歌单引用，本地文件只保存路径，不复制音频或保存账号凭据。
#pragma once
#include "sources.hpp"
#include "platform.hpp"
#include "tag.hpp"
#include <fstream>
#include <sstream>
#include <regex>
#include <functional>
#include <cctype>
#include <map>
#include <cmath>
#include <unordered_set>

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
        if (music::same_variant(x, y)) return true;
    return false;
}
/** @brief 收藏身份索引；遵循 same_variant 的平台、ID 和 B 站分 P 匹配规则。 */
class FavoriteIndex {
public:
    explicit FavoriteIndex(const std::vector<music::Track>& tracks = {}) {
        for (const auto& track : tracks) add(track);
    }
    bool contains(const music::Track& track) const {
        for (const auto& variant : track.variants)
            if (!variant.id.empty() && variants_.count(key(variant))) return true;
        return false;
    }
    void add(const music::Track& track) {
        for (const auto& variant : track.variants)
            if (!variant.id.empty()) variants_.insert(key(variant));
    }
private:
    static std::string key(const music::Variant& variant) {
        return std::to_string(int(variant.source)) + ":" + std::to_string(variant.id.size()) + ":" +
            variant.id + (variant.source == music::Source::Bilibili ? variant.native_id : "");
    }
    std::unordered_set<std::string> variants_;
};

/**
 * @brief 歌单曲目共享只读存储，只有可写访问才复制。
 *
 * 资料库版本、详情页与历史共享曲目；迭代器和可写引用只用于当前同步修改，
 * 不跨快照保存。UI 使用 const 访问，避免读取时触发复制。
 */
class SharedTracks {
public:
    using Container = std::vector<music::Track>;
    using iterator = Container::iterator;
    using const_iterator = Container::const_iterator;
    SharedTracks() : data_(std::make_shared<Container>()) {}
    SharedTracks(const SharedTracks&) = default;
    SharedTracks& operator=(const SharedTracks&) = default;
    // 移动后也保留可读取的空 / 旧值，避免已移动详情对象留下空指针。
    SharedTracks(SharedTracks&& other) noexcept : data_(other.data_) {}
    SharedTracks& operator=(SharedTracks&& other) noexcept { data_ = other.data_; return *this; }
    SharedTracks(Container tracks) : data_(std::make_shared<Container>(std::move(tracks))) {}
    SharedTracks(std::initializer_list<music::Track> tracks) : SharedTracks(Container(tracks)) {}
    operator const Container&() const { return *data_; }
    size_t size() const { return data_->size(); }
    bool empty() const { return data_->empty(); }
    const music::Track& operator[](size_t i) const { return (*data_)[i]; }
    music::Track& operator[](size_t i) { return edit()[i]; }
    const music::Track& front() const { return data_->front(); }
    music::Track& front() { return edit().front(); }
    const music::Track& back() const { return data_->back(); }
    music::Track& back() { return edit().back(); }
    const music::Track* data() const { return data_->data(); }
    const_iterator begin() const { return data_->begin(); }
    const_iterator end() const { return data_->end(); }
    iterator begin() { return edit().begin(); }
    iterator end() { return edit().end(); }
    void reserve(size_t count) { edit().reserve(count); }
    void clear() { data_ = std::make_shared<Container>(); }
    void resize(size_t count) { edit().resize(count); }
    void push_back(const music::Track& track) { edit().push_back(track); }
    void push_back(music::Track&& track) { edit().push_back(std::move(track)); }
    template<class... Args> void emplace_back(Args&&... args) { edit().emplace_back(std::forward<Args>(args)...); }
    iterator erase(const_iterator first, const_iterator last) {
        const auto a = first - data_->cbegin(), b = last - data_->cbegin();
        auto& tracks = edit();
        return tracks.erase(tracks.cbegin() + a, tracks.cbegin() + b);
    }
    iterator erase(const_iterator item) { return erase(item, item + 1); }
    template<class It> iterator insert(const_iterator at, It first, It last) {
        const auto offset = at - data_->cbegin();
        auto& tracks = edit();
        return tracks.insert(tracks.cbegin() + offset, first, last);
    }
private:
    Container& edit() {
        if (!data_.unique()) data_ = std::make_shared<Container>(*data_);
        return *data_;
    }
    std::shared_ptr<Container> data_;
};
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
            ",\"bitrate\":" + std::to_string(v.bitrate) + ",\"needs_login\":" + (v.needs_login ? "true" : "false") +
            ",\"album_id\":" + quote(v.album_id) + ",\"album_audio_id\":" + quote(v.album_audio_id) + ",\"quality_hashes\":{";
        bool first_hash = true;
        for (const auto& h : v.quality_hashes) {
            if (!first_hash) out += ','; first_hash = false;
            out += quote(h.first) + ":" + quote(h.second);
        }
        out += "},\"artists\":[";
        for (size_t i = 0; i < v.artists.size(); ++i) {
            if (i) out += ',';
            out += "{\"id\":" + quote(v.artists[i].id) + ",\"name\":" + quote(v.artists[i].name) + "}";
        }
        out += "]}";
    }
    return out + "]}";
}
// dropped：变体全都属于已移除的平台，这首直接丢弃。
// 这和"文件损坏"必须区分开 —— 否则一次升级就把用户整个资料库判成损坏。
inline bool decode_track(const js::Value& value, music::Track& t, bool* dropped = nullptr) {
    t.title = value["title"].str(); t.artist = value["artist"].str(); t.album = value["album"].str();
    t.cover_url = value["cover"].str(); t.duration_ms = int(value["duration_ms"].integer());
    const auto& variants = value["variants"];
    if (!variants.is_array() || variants.size() > 12) return false;
    for (const auto& row : variants.array) {
        const int source = int(row["source"].integer(-1));
        const auto id = row["id"].str();
        if (!music::is_platform(music::Source(source)) && music::Source(source) != music::Source::Local) continue;
        if (id.empty()) continue;
        music::Variant v;
        v.source = music::Source(source); v.id = id; v.native_id = row["native_id"].str();
        v.album_id = row["album_id"].str();
        v.album_audio_id = row["album_audio_id"].str();
        if (row["quality_hashes"].size() > 12) return false;
        for (const auto& h : row["quality_hashes"].object) v.quality_hashes[h.first] = h.second.str();
        if (row["artists"].size() > 32) return false;
        for (const auto& a : row["artists"].array) {
            if (!a["name"].str().empty()) v.artists.push_back({a["id"].str(), a["name"].str()});
        }
        v.title = row["title"].str(t.title); v.artist = row["artist"].str(t.artist);
        v.album = row["album"].str(t.album); v.cover_url = row["cover"].str(t.cover_url);
        v.duration_ms = int(row["duration_ms"].integer(t.duration_ms));
        v.bitrate = int(row["bitrate"].integer()); v.needs_login = row["needs_login"].truthy();
        t.variants.push_back(std::move(v));
    }
    if (t.variants.empty()) {
        if (dropped) *dropped = true;
        return false;
    }
    t.chosen = std::clamp(int(value["chosen"].integer()), 0, int(t.variants.size()) - 1);
    return true;
}
struct Playlist {
    std::string key, name, remote_id, cover;
    music::Source source = music::Source::Netease;
    SharedTracks tracks;
    std::string remote_owner; // QQ 自建歌单使用 dirid + uin；公开歌单使用 disstid。
    std::string local_folder;
    bool recursive = true;
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
    else {
        return false;
    }
    id = match[1].str();
    return id.size() <= 24 && id.find_first_not_of('0') != std::string::npos;
}
inline bool local_audio_file(const platform::fs::path& path) {
    auto ext = path.extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".mp3" || ext == ".flac" || ext == ".wav" || ext == ".wave" || ext == ".m4a" ||
        ext == ".aac" || ext == ".ogg" || ext == ".oga" || ext == ".opus" || ext == ".webm";
}
inline std::string local_path_key(const platform::fs::path& path) {
#ifdef _WIN32
    auto value = path.lexically_normal().native();
    const int length = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr, 0);
    if (length > 0) {
        std::wstring folded(size_t(length), L'\0');
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, value.data(), int(value.size()), folded.data(), length, nullptr, nullptr, 0);
        return platform::fs::path(folded).u8string();
    }
#endif
    return path.lexically_normal().u8string();
}
// 文件夹导入、系统打开、拖放共用标签规则，路径规范化后也能识别已有队列歌曲。
inline music::Track local_track(const platform::fs::path& file) {
    const auto path = file.u8string();
    music::Variant v; v.source = music::Source::Local; v.id = path;
    v.title = file.stem().u8string();
    const auto split = v.title.find(" - ");
    if (split != std::string::npos) { v.artist = v.title.substr(0, split); v.title.erase(0, split + 3); }
    v.album = file.parent_path().filename().u8string();
    const auto metadata = tag::read_metadata(path);
    if (!metadata.title.empty()) v.title = metadata.title;
    if (!metadata.artist.empty()) v.artist = metadata.artist;
    if (!metadata.album.empty()) v.album = metadata.album;
    v.duration_ms = int(std::min(metadata.duration_ms, 2147483647LL));
    if (!metadata.cover.empty()) v.cover_url = "local-cover:" + path;
    music::Track t; t.title = v.title; t.artist = v.artist; t.album = v.album; t.cover_url = v.cover_url; t.duration_ms = v.duration_ms;
    t.variants.push_back(std::move(v)); return t;
}
// 扫描路径与有界标签；不跟随目录链接、不修改原音乐，失败时调用方保留旧歌单。
inline Playlist scan_folder(const std::string& input, bool recursive, std::string& error,
                            const std::function<bool()>& cancelled = {}) {
    error.clear();
    Playlist out;
    std::error_code ec;
    auto folder = platform::fs::canonical(platform::fs::u8path(music::text::trim(input)), ec);
    if (ec || !platform::fs::is_directory(folder, ec)) { error = "文件夹不存在或无法读取"; return out; }
    out.local_folder = folder.u8string(); out.recursive = recursive;
    out.source = music::Source::Local; out.key = "folder#" + out.local_folder;
    out.name = folder.filename().u8string();
    if (out.name.empty()) out.name = out.local_folder;
    std::vector<std::string> files;
    auto add = [&](const platform::fs::directory_entry& entry) {
        if (entry.is_symlink(ec) || !entry.is_regular_file(ec) || ec) return;
        if (local_audio_file(entry.path())) files.push_back(entry.path().u8string());
    };
    if (recursive) {
        for (platform::fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            if (cancelled && cancelled()) { error = "扫描已取消"; return out; }
            add(*it);
            if (ec) break;
        }
    } else {
        for (platform::fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            if (cancelled && cancelled()) { error = "扫描已取消"; return out; }
            add(*it);
            if (ec) break;
        }
    }
    if (ec) { error = "部分目录无法读取，原歌单已保留：" + ec.message(); return out; }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    for (const auto& path : files) {
        if (cancelled && cancelled()) { error = "扫描已取消"; return out; }
        out.tracks.push_back(local_track(platform::fs::u8path(path)));
    }
    if (!out.tracks.empty()) out.cover = out.tracks.front().cover_url;
    if (out.tracks.empty()) error = "文件夹内没有支持的音频文件，原歌单已保留";
    return out;
}
class Library {
public:
    std::vector<Playlist> playlists;
    struct LyricPreference { int offset_ms = 0; std::string path; };
    std::vector<music::Track> recent;
    std::map<std::string, LyricPreference> lyric_preferences;
    Library() { reset(); }
    void reset() { playlists = {{"favorites", "本地收藏", {}, {}, music::Source::Netease, {}}}; recent.clear(); lyric_preferences.clear(); }
    void remember(const music::Track& track) {
        recent.erase(std::remove_if(recent.begin(), recent.end(), [&](const auto& t) { return same_track(t, track); }), recent.end());
        recent.insert(recent.begin(), track);
        if (recent.size() > 200) recent.resize(200);
    }
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
                ",\"source\":" + std::to_string(int(p.source)) + ",\"remote_owner\":" + quote(p.remote_owner) + ",\"local_folder\":" + quote(p.local_folder) +
                ",\"recursive\":" + (p.recursive ? "true" : "false") + ",\"tracks\":[";
            bool first_track = true;
            for (const auto& t : p.tracks) {
                if (!first_track) out += ',';
                first_track = false; out += encode_track(t);
            }
            out += "]}";
        }
        out += "],\"recent\":[";
        first = true;
        for (const auto& track : recent) { if (!first) out += ','; first = false; out += encode_track(track); }
        out += "],\"lyrics\":["; first = true;
        for (const auto& entry : lyric_preferences) {
            if (!first) out += ','; first = false;
            out += "{\"uid\":" + quote(entry.first) + ",\"offset\":" + std::to_string(entry.second.offset_ms) + ",\"path\":" + quote(entry.second.path) + "}";
        }
        out += "]}\n";
        if (out.size() > 64 * 1024 * 1024) { err = "本地资料库超过 64 MiB，原文件已保留"; return false; }
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
            p.local_folder = row["local_folder"].str();
            p.recursive = row["recursive"].is_null() || row["recursive"].truthy();
            const int source = int(row["source"].integer(-1));
            if (p.key.empty() || p.name.empty() || !row["tracks"].is_array() ||
                (!music::is_platform(music::Source(source)) && music::Source(source) != music::Source::Local)) {
                err = "本地资料库条目损坏，原文件保留"; return false;
            }
            p.source = music::Source(source);
            for (const auto& track : row["tracks"].array) {
                music::Track t;
                bool dropped = false;
                if (!decode_track(track, t, &dropped)) {
                    if (dropped) continue;   // 来源平台已移除，跳过这首歌
                    err = "本地曲目数据损坏，原文件保留"; return false;
                }
                p.tracks.push_back(std::move(t));
            }
            if (p.key == "favorites") loaded.playlists.front() = std::move(p);
            else loaded.upsert(std::move(p));
        }
        if ((!root["recent"].is_null() && !root["recent"].is_array()) ||
            (!root["lyrics"].is_null() && !root["lyrics"].is_array())) { err = "本地记录格式损坏"; return false; }
        if (root["recent"].size() > 200 || root["lyrics"].size() > 4096) { err = "本地记录超过容量限制"; return false; }
        for (const auto& row : root["recent"].array) {
            music::Track track; bool dropped = false;
            if (!decode_track(row, track, &dropped)) {
                if (dropped) continue;
                err = "最近播放记录损坏"; return false;
            }
            loaded.recent.push_back(std::move(track));
        }
        for (const auto& row : root["lyrics"].array) {
            const auto uid = row["uid"].str(), path = row["path"].str();
            if (uid.empty() || uid.size() > 8192 || path.size() > 8192 || !std::isfinite(row["offset"].num())) { err = "歌词配置损坏"; return false; }
            loaded.lyric_preferences[uid] = {int(std::clamp(row["offset"].num(), -60000.0, 60000.0)), path};
        }
        *this = std::move(loaded); return true;
    }
};
} // namespace library
