// 业务：搜索、播放、登录、下载、歌单、持久化与后台状态；不依赖 aino。
#pragma once
#include "audio.hpp"
#include "background.hpp"
#include "cache.hpp"
#include "cover.hpp"
#include "discover.hpp"
#include "library.hpp"
#include "login.h"
#include "lyrics.hpp"
#include "net.hpp"
#include "netease_api.hpp"
#include "platform.hpp"
#include "qqapi.hpp"
#include "sources.hpp"
#include "tag.hpp"
#include "../assets/aino_icon.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <numeric>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

// 业务：搜索、播放、登录、下载、歌单、持久化与后台状态；不依赖 aino。
// UI 可编辑交互状态，业务结果通过只读快照展示；不暴露锁或播放设备。
namespace player {
enum class PlayState { Idle, Resolving, Buffering, Ready, Playing, Paused, Failed };
enum class PlayMode { Sequential, Shuffle, RepeatAll, RepeatOne };
inline const char* play_mode_name(PlayMode mode) {
    switch (mode) {
        case PlayMode::Sequential: return "顺序播放";
        case PlayMode::Shuffle: return "随机播放";
        case PlayMode::RepeatOne: return "单曲循环";
        default: return "列表循环";
    }
}
enum class ClearAction { None, Cookies, Library, Queue, Cache };
#ifndef AINO_PLAYER_VERSION
#define AINO_PLAYER_VERSION "0.0.0"
#endif
inline constexpr const char* kVersion = AINO_PLAYER_VERSION;
namespace project {
inline constexpr const char* author = "sudoevolve";
inline constexpr const char* github = "https://github.com/sudoevolve/aino-player";
inline constexpr const char* license = "Apache-2.0";
}
enum class ProjectLink { Repository, Issues, License, Dependencies };
enum class UpdateStatus { Idle, Checking, Current, Available, Downloading, Installing, Installed, Failed, NoRelease };
struct UpdateView {
    UpdateStatus status = UpdateStatus::Idle;
    std::string version, release_url, asset_url, message, digest, release_notes;
    long long asset_size = 0;
    bool installer = false;
    int progress = 0;
};

// 只比较正式版，并只接受本仓库的 HTTPS 发布页面和安装包。
namespace updates {
inline constexpr const char* kReleases = "https://github.com/sudoevolve/aino-player/releases";
inline constexpr const char* kApi = "https://api.github.com/repos/sudoevolve/aino-player/releases/latest";
inline bool version_parts(std::string text, std::array<unsigned, 3>& parts) {
    if (!text.empty() && text.front() == 'v') text.erase(0, 1);
    size_t at = 0;
    for (size_t i = 0; i < parts.size(); ++i) {
        const auto start = at;
        unsigned value = 0;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
            if (value > 99999) return false;
            value = value * 10 + unsigned(text[at++] - '0');
        }
        if (at == start || (at - start > 1 && text[start] == '0')) return false;
        parts[i] = value;
        if (i + 1 < parts.size() && (at == text.size() || text[at++] != '.')) return false;
    }
    return at == text.size();
}
inline UpdateView parse_release(const net::Response& response, const std::string& current, const std::string& suffix) {
    UpdateView view;
    view.status = UpdateStatus::Failed;
    if (!response.error.empty()) { view.message = "无法连接 GitHub，请检查网络后重试"; return view; }
    if (response.status == 404) { view.status = UpdateStatus::NoRelease; view.message = "GitHub 暂无正式版本"; return view; }
    if (response.status == 403 || response.status == 429) { view.message = "GitHub 请求受限，请稍后重试"; return view; }
    if (!response.ok()) { view.message = "GitHub 检查失败（HTTP " + std::to_string(response.status) + "）"; return view; }
    js::Value release;
    if (response.body.size() > 1024 * 1024 || !js::parse(response.body, release) || !release.is_object()) {
        view.message = "GitHub 返回的版本数据无效"; return view;
    }
    const auto tag = release["tag_name"].str();
    std::array<unsigned, 3> installed{}, latest{};
    const auto release_url = std::string(kReleases) + "/tag/" + tag;
    if (release["draft"].truthy() || release["prerelease"].truthy() ||
        !version_parts(current, installed) || !version_parts(tag, latest) || release["html_url"].str() != release_url) {
        view.message = "GitHub 正式版本信息无效"; return view;
    }
    view.version = tag; view.release_url = release_url;
    view.release_notes = release["body"].str();
    if (view.release_notes.size() > 65536) {
        size_t end = 65536;
        while (end > 0 && (static_cast<unsigned char>(view.release_notes[end]) & 0xc0) == 0x80) --end;
        view.release_notes.resize(end);
        view.release_notes += "\n\n更新说明较长，完整内容请查看发布页。";
    }
    view.status = latest > installed ? UpdateStatus::Available : UpdateStatus::Current;
    view.message = latest > installed ? "发现新版，可下载后更新" : "当前已是最新正式版";
    const auto prefix = std::string(kReleases) + "/download/" + tag + "/";
    for (const auto& asset : release["assets"].array) {
        const auto name = asset["name"].str(), url = asset["browser_download_url"].str();
        if (!suffix.empty() && name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0 &&
            name.rfind("aino-player", 0) == 0 && name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == std::string::npos &&
            url == prefix + name) {
            const auto version = tag.front() == 'v' ? tag.substr(1) : tag;
            const bool installer = name == "aino-player-" + tag + "-setup-windows-x64.exe" ||
                                   name == "aino-player_" + version + "_amd64.deb";
            if (!view.asset_url.empty() && (!installer || view.installer)) continue;
            view.asset_url = url; view.installer = installer;
            view.digest = asset["digest"].str();
            const double size = asset["size"].num();
            view.asset_size = std::isfinite(size) && size > 0 && size <= 64 * 1024 * 1024 ? (long long)size : 0;
        }
    }
    return view;
}
inline const char* asset_suffix() {
#ifdef _WIN32
    return "windows-x64.exe";
#elif defined(__x86_64__)
    return "_amd64.deb";
#else
    return "";
#endif
}
} // namespace updates
struct Snapshot {
    bool searching = false;
    std::vector<music::Track> tracks; // 只保存播放队列
    std::vector<music::Track> results; // 在线搜索结果独立于队列、歌单详情
    int results_version = -1;
    std::vector<music::SourceReport> reports;
    int elapsed_ms = 0;
    int tracks_version = -1;
    int playing = -1;
    PlayState ps = PlayState::Idle;
    bool pending_paused = false;
    std::string now_uid, now_cover, now_title, now_artist, now_quality, now_short, error;
    music::Track now_track;
    int now_source = -1;
    int now_lane = -1;   // 这条流由哪条渠道给出：0 音源 / 1 自带 / -1 未知
    // 详细来源：谁给的链接（音源脚本名 / providers/xx.js / 内置），
    // 以及"原曲在哪"与"实际从哪放"—— 跨源补位时必须说清楚，否则界面上平台会乱跳。
    std::string now_engine;
    int now_origin = -1;      // 原曲所属平台（列表里显示的那个）
    bool now_swapped = false; // true = 换了别的平台才取到流
    float buffer = 0.0f;
    long long cached_bytes = 0;
    std::string pending_path;
    long long pending_duration_ms = 0;
    int gen = 0;
};

struct UiState {
    std::string query = "周杰伦";
    music::Settings settings;
    uint32_t src_mask = 0x3;   // 默认只开网易云 + QQ
    int tab = 1;
    float volume = 70.0f;
    float pos_ms = 0.0f;
    float dur_ms = 0.0f;
    float frame_ms = 0.0f;
    int seek_to = -1;
    bool scroll_to_top = false;
    bool config_dirty = false;   // 音量 / 音源开关 / Cookie 变了，等合适时机落盘
    double last_save_s = 0.0;

    // 外观
    bool dark = false;
    bool close_to_tray = false, tray_enabled = true, window_hidden = false;
    int close_behavior = 0; // 0 询问，1 到托盘，2 退出；旧版 bool 仅保留配置兼容。
    bool close_dialog = false, close_remember = false;
    bool media_available = false, tray_available = false;
    std::string output_device, lyric_file_path;
    int download_filter = 0;
    bool lyric_tools = false, device_loading = false;
    int search_kind = 0, playlist_search_site = 0;
    std::vector<audio::OutputDevice> output_devices;
    std::string device_error;
    // 歌词面板（Apple Music 那种右侧抽屉）
    bool lyrics_open = false;
    bool queue_open = false;
    // 账号登录态（cookie 存在 settings 里，uin/uid 单独存）
    std::string qq_uin;
    std::string ne_uid;
    // 歌单页当前看的是哪个账号（0=网易云 1=QQ）
    int pl_site = 2;
    int sources_tab = 0;   // 音源页：0 = 第三方音源，1 = 自带接口
    bool sources_show_failed = false;  // 是否展开"没能加载"的脚本
    std::string playlist_link;
    std::string local_name;
    std::string local_folder;
    bool scan_recursive = true;
    std::string playlist_filter, playlist_rename, library_transfer_path;
    int playlist_sort = 0;
    bool playlist_manage = false;
    bool importing_playlist = false;
    bool opening_local = false, choosing_local = false, setting_default_player = false;
    int catalog_section = 0;
    int library_add_mode = -1; // 收起 / 文件夹 / 在线链接 / 当前队列
    std::string remove_playlist_key;
    int settings_tab = 0;
    bool update_settings = false;
    ClearAction clear_action = ClearAction::None;
    // 发现页平台（0=网易云，1=QQ）
    int disc_site = 0;   // kDiscoveryPlatforms 的界面索引
    PlayMode play_mode = PlayMode::RepeatAll;
    unsigned long long sleep_deadline = 0;
    unsigned sleep_remaining_seconds = 0;
    bool sleep_after_track = false;
    // 下载
    std::string download_dir;    // 空 = 用默认 %USERPROFILE%\Music\aino player
    // 提示条：底栏不再放长错误文本，一次性消息统一走 toast
    std::string toast;
};

struct DlItem {
    int percent = 0;
    bool done = false;
    bool failed = false;
    bool publishing = false;
    bool queued = true;
    std::string path;
    music::Track track;
    std::string name;   // 保存的文件名
    std::string err;
};

struct QqSnap {
    bool logged = false;
    bool loading = false;
    std::string nickname, err, note;
    std::vector<qq::Playlist> playlists;
};

struct NeSnap {
    bool logged = false;
    bool loading = false;
    std::string nickname, err, uid;
    std::vector<ne::Playlist> playlists;
};
struct DiscSnap {
    bool loading = false, more = true;
    std::string query;
    int site = 0;
    int version = -1;
    std::vector<disc::Card> cards;
    disc::Heats heats;
    std::string err;
};

struct LoginView { bool connecting = false; std::string note; };
struct LyricView { std::shared_ptr<const lyr::Doc> doc; std::string uid; bool loading = false; };
struct StoragePaths { std::string config, cache, current_cache; };
struct PlaylistView {
    bool open = false, loading = false, saved = false;
    int return_tab = 3;
    unsigned library_revision = 0;
    library::Playlist playlist;
    std::string error;
    std::vector<int> rows;
    bool catalog_open = false, albums_loaded = false, loading_more = false;
    music::CatalogTarget catalog;
    music::Track catalog_seed;
    int catalog_artist_index = 0;
    std::vector<music::CatalogTarget> albums;
    std::vector<int> album_rows;
    std::string description, albums_error;
    bool tracks_more = false, albums_more = false;
    bool tracks_retry_append = false, albums_retry_append = false;
    int tracks_offset = 0, albums_offset = 0;
    std::string tracks_continuation, albums_continuation;
};
struct ViewModel {
    UiState& ui;
    const Snapshot& playback;
    const library::Library& library;
    const QqSnap& qq;
    const NeSnap& netease;
    const DiscSnap& discovery;
    const DiscSnap& playlist_search;
    const std::map<std::string, DlItem>& downloads;
    const std::array<LoginView, 3>& login;
    const LyricView& lyrics;
    const StoragePaths& paths;
    const UpdateView& update;
    const PlaylistView& playlist;
    int sync_site, sync_index;
    unsigned long long toast_until;
    std::shared_ptr<const library::Library> library_storage;
    unsigned library_revision = 0;
    const library::FavoriteIndex& favorites;
};
inline std::string track_uid(const music::Track& track) {
    const auto& variant = track.current();
    return music::variant_uid(variant);
}
} // namespace player

namespace player {
inline std::string fmt_time(long long ms) {
    if (ms < 0) ms = 0;
    const long long total = ms / 1000;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld:%02lld", total / 60, total % 60);
    return buf;
}

inline std::string fmt_size(long long bytes) {
    char buf[32];
    if (bytes >= 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f MB", double(bytes) / (1024.0 * 1024.0));
    else if (bytes >= 1024) std::snprintf(buf, sizeof buf, "%.0f KB", double(bytes) / 1024.0);
    else std::snprintf(buf, sizeof buf, "%lld B", bytes);
    return buf;
}

} // namespace player

// UI 动作与应用生命周期接口；后台资源和互斥锁留在控制器实现中。
namespace player {
struct HostCallbacks { std::function<void()> wake; std::function<void()> activate; std::function<void()> close; std::function<void*()> native_window; std::function<bool()> hide; };
ViewModel view_model();
bool initialize(bool smoke_test);
void start(HostCallbacks host);
void tick();
void bootstrap(bool initial_search, bool smoke_test);
void finish_frame(float frame_ms);
void shutdown(bool smoke_test);
void cancel_login(int site);
void seek_playback(int milliseconds);
void choose_source(int index, int variant, bool replay);
void library_remove(size_t index);
void library_choose_folder();
void open_local_files(std::vector<std::string> paths);
void choose_local_audio();
void configure_default_player();
void playlist_filter_rows();
void playlist_rename_local();
void library_export(bool playlist_only);
void library_restore();
void queue_move(int from, int to);
void download_current();
void request_current_lyrics(bool force = false);
void open_data_directory(const std::string& path);
std::shared_ptr<const cover::Image> cover_image(const std::string& url);
void start_search();
void search_more();
void retry_search_source(int source);
void start_play(int index);
void toggle_pause();
void media_action(const platform::MediaEvent& event);
void play_step(int delta, bool automatic = false);
void set_play_mode(PlayMode mode);
void set_sleep_timer(int minutes, bool after_track = false);
void queue_play_next(const music::Track& track);
void set_volume(float v);
void toast(const std::string& text, int ms = 3600);
void start_download(music::Track track, music::Settings st);
void cancel_download(const std::string& uid);
void cancel_all_downloads();
void retry_failed_downloads();
void clear_finished_downloads();
void retry_download(const std::string& uid);
void reveal_download(const std::string& uid);
void reveal_local_track(const music::Track& track);
void recent_play(size_t index);
void recent_clear();
int lyric_offset();
void change_lyric_offset(int delta, bool reset = false);
void use_local_lyrics(bool reset = false);
void choose_local_lyrics();
void resolve_window_close(bool to_tray);
void refresh_output_devices();
void select_output_device(const std::string& id);
void qq_refresh();
void ne_refresh();
void qq_open_playlist(int idx);
void ne_open_playlist(int idx);
void playlist_sync_to(int site, int idx);
bool queue_push(const music::Track& t);
void queue_clear();
void queue_remove(int idx);
void disc_refresh(int site);
void disc_more();
void search_playlists(bool append = false);
void open_playlist_card(const disc::Card& card);
void disc_open(int idx);
void favorite_toggle(const music::Track& track);
enum class BatchAction { Enqueue, Favorite, Download, RemovePlaylist, RemoveQueue };
void batch_tracks(std::vector<music::Track> tracks, BatchAction action, std::string playlist_key = {});
void library_open(size_t index);
void library_save_current();
void playlist_open_remote(library::Playlist playlist);
void bilibili_open_parts(const music::Track& track);
void open_track_artist(const music::Track& track, int artist_index = 0);
void begin_login(int site);
void clear_data(ClearAction action);
void check_updates(bool notify = true);
void open_update(bool download);
void open_project(ProjectLink link);
const cover::Image& brand_icon();
std::string default_download_dir();
} // namespace player

namespace player::config { void load(UiState& state); bool save(UiState& state); }
namespace player::config {
static std::string config_path() {
    const auto dir = platform::config_dir();
    return dir.empty() ? std::string() : platform::join(dir, "config.ini");
}

static std::string read_line_value(const std::string& line, const char* key) {
    const std::string k(key);
    if (line.rfind(k + "=", 0) != 0) return std::string();
    return line.substr(k.size() + 1);
}

void load(UiState& state) {
    const std::string path = config_path();
    if (path.empty()) return;
    std::FILE* f = platform::fopen(path, "rb");  // 用户名可能是中文
    if (!f) return;
    char buf[65536];
    bool has_version = false, has_play_mode = false;
    while (std::fgets(buf, sizeof buf, f)) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::string v = read_line_value(line, "volume");
        if (!v.empty()) state.volume = float(std::atof(v.c_str()));
        v = read_line_value(line, "sources");
        if (!v.empty()) {
            const uint32_t m = uint32_t(std::strtoul(v.c_str(), nullptr, 10));
            // 老配置里可能还开着已删除的平台，这里直接清掉
            uint32_t allowed = 0;
            for (music::Source src : music::kPlatforms) allowed |= 1u << int(src);
            state.src_mask = m & allowed;
            if (!state.src_mask && m) state.src_mask = 0x3u; // 旧配置只启用历史平台时，恢复默认平台。
            if (m != state.src_mask) state.config_dirty = true;
        }
        v = read_line_value(line, "netease_cookie");
        if (!v.empty()) state.settings.netease_cookie = v;
        v = read_line_value(line, "qq_cookie");
        if (!v.empty()) state.settings.qq_cookie = v;
        v = read_line_value(line, "download_dir");
        if (!v.empty()) state.download_dir = v;
        v = read_line_value(line, "qq_uin");
        if (!v.empty()) state.qq_uin = v;
        v = read_line_value(line, "netease_uid");
        if (!v.empty()) state.ne_uid = v;
        v = read_line_value(line, "dark");
        if (!v.empty()) state.dark = (v != "0");
        v = read_line_value(line, "quality");
        if (!v.empty()) state.settings.quality = std::atoi(v.c_str());
        v = read_line_value(line, "lane_pref");
        if (!v.empty()) state.settings.lane_pref = std::atoi(v.c_str()) == 1 ? 1 : 0;
        v = read_line_value(line, "lane_enable");
        if (!v.empty()) {
            const uint32_t m = uint32_t(std::strtoul(v.c_str(), nullptr, 10));
            for (int i = 0; i < music::kLaneCount; ++i) state.settings.lane_enable[i] = (m >> i) & 1u;
            // 两条都关掉等于听不了歌，至少留一条
            if (!state.settings.lane_enable[0] && !state.settings.lane_enable[1])
                state.settings.lane_enable[1] = true;
        }
        v = read_line_value(line, "embed_cover");
        if (!v.empty()) state.settings.embed_cover = (v != "0");
        v = read_line_value(line, "embed_lyrics");
        if (!v.empty()) state.settings.embed_lyrics = (v != "0");
        v = read_line_value(line, "loop_list");
        if (!v.empty() && !has_play_mode) state.play_mode = v == "0" ? PlayMode::Sequential : PlayMode::RepeatAll;
        v = read_line_value(line, "play_mode");
        if (v == "0" || v == "1" || v == "2" || v == "3") {
            state.play_mode = PlayMode(std::atoi(v.c_str())); has_play_mode = true;
        }
        v = read_line_value(line, "config_version");
        if (!v.empty()) has_version = std::atoi(v.c_str()) >= 2;
    }
    std::fclose(f);

    // 保留旧配置版本迁移；平台位始终按当前支持列表过滤。
    if (!has_version) state.config_dirty = true;
}

bool save(UiState& state) {
    const std::string path = config_path();
    if (path.empty()) return false;
    const auto temporary = path + ".tmp";
    std::FILE* f = platform::fopen(temporary, "wb");
    if (!f) return false;
    std::fprintf(f, "# aino player 配置（Cookie 明文放在本机，介意就别填）\n");
    std::fprintf(f, "config_version=2\n");
    std::fprintf(f, "volume=%d\n", int(state.volume));
    std::fprintf(f, "sources=%u\n", state.src_mask);
    std::fprintf(f, "netease_cookie=%s\n", state.settings.netease_cookie.c_str());
    std::fprintf(f, "qq_cookie=%s\n", state.settings.qq_cookie.c_str());
    std::fprintf(f, "download_dir=%s\n", state.download_dir.c_str());
    std::fprintf(f, "qq_uin=%s\n", state.qq_uin.c_str());
    std::fprintf(f, "netease_uid=%s\n", state.ne_uid.c_str());
    std::fprintf(f, "close_to_tray=%d\ntray_enabled=%d\noutput_device=%s\n", state.close_behavior == 1, state.tray_enabled, state.output_device.c_str());
    std::fprintf(f, "close_behavior=%d\n", state.close_behavior);
    std::fprintf(f, "dark=%d\n", state.dark ? 1 : 0);
    std::fprintf(f, "quality=%d\n", state.settings.quality);
    std::fprintf(f, "lane_pref=%d\n", state.settings.lane_pref);
    std::fprintf(f, "lane_enable=%u\n", unsigned(state.settings.lane_enable[0] ? 1 : 0) |
                                        unsigned(state.settings.lane_enable[1] ? 2 : 0));
    std::fprintf(f, "embed_cover=%d\n", state.settings.embed_cover ? 1 : 0);
    std::fprintf(f, "embed_lyrics=%d\n", state.settings.embed_lyrics ? 1 : 0);
    std::fprintf(f, "play_mode=%d\n", int(state.play_mode));
    std::fprintf(f, "loop_list=%d\n", state.play_mode == PlayMode::Sequential ? 0 : 1);
    const bool written = !std::ferror(f);
    const bool closed = std::fclose(f) == 0;
    if (!written || !closed || !platform::replace_file(temporary, path)) { platform::remove(temporary); return false; }
    state.config_dirty = false;
    return true;
}

} // namespace player::config

namespace player {

// 异步区：这些字段由后台线程写，主线程每帧拷快照读
struct AsyncState {
    bool searching = false;
    std::vector<music::Track> tracks; // 只保存播放队列
    std::vector<music::Track> results; // 在线搜索结果独立于队列、歌单详情
    int results_version = -1;
    std::vector<music::SourceReport> reports;
    int elapsed_ms = 0;
    int tracks_version = 0;

    int gen = 0;
    int playing = -1;
    PlayState ps = PlayState::Idle;
    std::string now_uid;    // 正在播放的曲目身份（源#id），不随列表重排漂移
    music::Track now_track; // 切换搜索/歌单后仍能收藏当前歌曲。
    std::string now_cover;  // 当前曲目的封面 URL（可能为空 → 用程序化渐变）
    std::string now_title;
    std::string now_artist;
    std::string now_quality = "—";
    std::string now_short;   // 精简音质（"128 kbps"），底栏只放得下这么多
    int now_source = -1;
    int now_lane = -1;   // 这条流由哪条渠道给出：0 音源 / 1 自带 / -1 未知（本地）
    std::string now_engine;   // 具体实现：音源脚本名 / providers 文件名 / 内置
    int now_origin = -1;      // 原曲所属平台；与 now_source 不同即"换源播放"
    bool now_swapped = false;
    std::string error;
    float buffer = 0.0f;
    long long cached_bytes = 0;
    std::string pending_path;
    long long pending_duration_ms = 0;
    long long position_ms = 0;
};

static AsyncState g_as;
static std::atomic<int> g_play_generation{0};
static std::shared_ptr<audio::Stream> g_pending_stream; // g_mu；仅用于向主线程交接设备数据源。
static audio::StreamSpec g_active_stream_spec; // g_mu；seek 重建使用，不保存 URL 到磁盘。
static bool g_play_ready_paused = false; // g_mu
static int g_track_failure_gen = -1; // g_mu；仅取流/文件失败允许跳歌，设备故障保留现场。
static std::set<std::string> g_failed_tracks; // 主线程；一次连续失败链，手动点播时清空。
static std::mutex g_mu;
static std::atomic<bool> g_shutdown{false};
static std::atomic<int> g_search_gen{0};
struct SearchRequest {
    std::string keyword; music::Settings settings; int generation = 0; bool append = false;
    std::array<int, music::kSourceCount> pages{};
    std::array<std::string, music::kSourceCount> continuation;
};
static std::string g_search_keyword;
static std::array<int, music::kSourceCount> g_search_pages{};
static std::array<std::string, music::kSourceCount> g_search_continuation, g_search_page_input;
static SearchRequest g_pending_search; // g_mu；只保留最新的待执行搜索。
static bool g_search_worker = false;
// 界面希望的刷新节奏（帧/秒）：0 = 不用动，30 = 有进度要跟，60 = 歌词在滚动
static std::atomic<int> g_fps_hint{0};
static std::atomic<int> g_frame_ms_x10{100};
static std::atomic<unsigned long long> g_sleep_deadline{0};
// 与队列共用 g_mu；显式指定的下一首优先于随机和单曲循环。
static std::deque<std::string> g_play_next;
static std::vector<std::string> g_shuffle_remaining;
static std::vector<std::string> g_shuffle_history;
static int g_shuffle_cursor = -1;
static int g_shuffle_version = -1;
static std::mt19937 g_shuffle_random{std::random_device{}()};

static std::string g_temp_dir;
static std::shared_mutex g_play_cache_mutex;
static HostCallbacks g_host;
static HostCallbacks* g_win = nullptr;

// UI 自己的状态：只有主线程碰，不需要锁

static UiState g_ui;
static Snapshot g_snapshot;
// 只支持网易云(0) / QQ(1)
static login::Site login_site(int site) { return site == 1 ? login::Site::QQ : login::Site::NetEase; }
static UpdateView g_update_view;
static std::atomic<bool> g_update_cancel{false};
static PlaylistView g_playlist_view;
static std::atomic<unsigned> g_playlist_generation{0};
static unsigned g_library_revision = 0;
struct DetailHistory {
    PlaylistView view;
    std::string filter, rename;
    int sort = 0, section = 0;
    bool manage = false;
};
static std::vector<DetailHistory> g_detail_history;
static std::atomic<bool> g_folder_cancel{false};
static bool g_folder_selecting = false, g_lyric_selecting = false;
static std::atomic<bool> g_lyric_cancel{false};
static std::mutex g_ui_mail_mutex;
static std::deque<std::function<void()>> g_ui_mail;
static void post_ui(std::function<void()> action) {
    if (g_shutdown.load()) return;
    { std::lock_guard<std::mutex> lk(g_ui_mail_mutex); g_ui_mail.push_back(std::move(action)); }
    if (g_win) g_win->wake();
}
static void drain_ui_mail() {
    std::deque<std::function<void()>> actions;
    { std::lock_guard<std::mutex> lk(g_ui_mail_mutex); actions.swap(g_ui_mail); }
    for (auto& action : actions) action();
}
static std::shared_ptr<library::Library> g_library = std::make_shared<library::Library>();
struct LibraryEdit { std::function<void(library::Library&)> apply; std::function<void()> saved; };
static std::mutex g_library_write_mutex;
static std::deque<LibraryEdit> g_library_edits;
static std::shared_ptr<library::Library> g_library_durable;
static bool g_library_writer = false;
static bool g_library_writable = true;
static unsigned g_library_epoch = 0;
static std::string library_path() { return platform::join(platform::config_dir(), "library.json"); }

static audio::Player g_player;
static cover::Store g_covers;

// 歌词：后台线程取，主线程只拿 shared_ptr 读，不加锁看内容
struct LyricState {
    unsigned long long generation = 0;
    std::mutex m;
    std::string for_uid;
    std::shared_ptr<const lyr::Doc> doc;
    bool loading = false;
};
static LyricState g_lyr;

// 下载表：uid → 状态。仅修订号变化时发布 UI 快照。

static std::mutex g_dlm;
static std::map<std::string, DlItem> g_dls;
static std::map<std::string, std::shared_ptr<std::atomic<bool>>> g_download_cancel;
static std::deque<std::function<void()>> g_download_pending;
static int g_download_workers = 0;
static unsigned long long g_download_revision = 0, g_download_snapshot_revision = ~0ULL;
static std::map<std::string, DlItem> g_dls_snap;   // 主线程快照

// toast 的到期时间放在原子里，后台刷新线程要读它（决定还要不要叫醒主循环）
static std::atomic<unsigned long long> g_toast_until{0};

// ---- 账号 + 歌单（QQ 音乐 / 网易云音乐 各一套）----
struct QqState {
    std::mutex m;
    bool logged = false;
    bool loading = false;
    std::string cookie, uin, nickname, err;
    std::vector<qq::Playlist> playlists;
    unsigned playlists_revision = 1;
};
static QqState g_qq;

// 主线程绘制用的不可变快照（不持锁画界面）

static QqSnap g_qq_snap;

struct NeState {
    std::mutex m;
    bool logged = false;
    bool loading = false;
    std::string cookie, uid, nickname, err;
    std::vector<ne::Playlist> playlists;
    unsigned playlists_revision = 1;
};
static NeState g_ne;

static NeSnap g_ne_snap;

// 同步进行中的目标：(站点, 下标)。站点 0=网易云 1=QQ，-1 = 空闲
static std::atomic<int> g_sync_site{-1};
static std::atomic<int> g_sync_index{-1};
static std::atomic<int> g_wait_login_site{-1};
static std::atomic<int> g_goto_tab{-1};

// 内部后台任务的前向声明；UI 只使用上方的动作接口。

void start_search();
void start_play(int index);
// 队列相关（定义在下面的"后台任务"之后，这里先声明给 do_search / 打开歌单用）
static void queue_mark_dirty();
static bool queue_save();
static void queue_load();
void toggle_pause();
void play_step(int delta, bool automatic);
void set_volume(float v);
static bool save_config() { return config::save(g_ui); }

static void do_search(HostCallbacks* win, SearchRequest request) {
    auto st = request.settings;
    const int gen = request.generation;
    st.cancelled = [gen] { return g_shutdown.load() || gen != g_search_gen.load(); };
    net::CancelScope cancellation(st.cancelled);
    if (st.cancelled()) return;
    std::vector<music::Track> base;
    std::vector<music::SourceReport> old_reports;
    { std::lock_guard<std::mutex> lock(g_mu); if (request.append) base = g_as.results; old_reports = g_as.reports; }
    auto publish = [&](const music::SearchResult& result, bool done) {
        if (st.cancelled()) return;
        std::vector<music::Track> merged;
        std::map<std::string, size_t> index;
        for (const auto* batch : std::array<const std::vector<music::Track>*, 2>{&base, &result.tracks})
            for (const auto& track : *batch) for (const auto& variant : track.variants)
                music::detail::merge_into(merged, index, variant);
        std::lock_guard<std::mutex> lock(g_mu);
        if (gen != g_search_gen.load()) return;
        g_as.reports = result.reports;
        if (request.append) for (auto& report : g_as.reports) if (!st.enable[int(report.source)])
            for (const auto& old : old_reports) if (old.source == report.source) { report = old; break; }
        g_as.elapsed_ms = result.elapsed_ms; g_as.searching = !done;
        // 每种变体都可找到其选择和封面，分批结果重排不会重置换源选择。
        std::map<std::string, std::string> selected, covers;
        for (const auto& track : g_as.results) for (const auto& variant : track.variants)
            selected[music::variant_uid(variant)] = music::variant_uid(track.current());
        for (const auto* batch : std::array<const std::vector<music::Track>*, 2>{&base, &result.tracks})
            for (const auto& track : *batch) for (const auto& variant : track.variants)
                if (!track.cover_url.empty()) covers[music::variant_uid(variant)] = track.cover_url;
        for (auto& track : merged) {
            track.score = music::detail::relevance(track, music::core_title(request.keyword));
            for (int i = 0; i < int(track.variants.size()); ++i) if (!track.variants[size_t(i)].needs_login) { track.chosen = i; break; }
            for (const auto& variant : track.variants) {
                const auto key = music::variant_uid(variant);
                if (track.cover_url.empty()) {
                    const auto cover = covers.find(key);
                    if (cover != covers.end()) track.cover_url = cover->second;
                    else if (!variant.cover_url.empty()) track.cover_url = variant.cover_url;
                }
                const auto old = selected.find(key);
                if (old != selected.end()) for (int i = 0; i < int(track.variants.size()); ++i)
                    if (music::variant_uid(track.variants[size_t(i)]) == old->second) { track.chosen = i; break; }
            }
        }
        std::stable_sort(merged.begin(), merged.end(), [](const auto& a, const auto& b) {
            if (std::abs(a.score - b.score) > 0.01) return a.score > b.score;
            if (a.variants.size() != b.variants.size()) return a.variants.size() > b.variants.size();
            return a.duration_ms > b.duration_ms;
        });
        g_as.results = std::move(merged);
        if (done) for (int i = 0; i < music::kSourceCount; ++i) if (st.enable[i]) {
            g_search_pages[size_t(i)] = request.pages[size_t(i)];
            g_search_page_input[size_t(i)] = request.continuation[size_t(i)];
            g_search_continuation[size_t(i)] = result.continuation[size_t(i)];
        }
        ++g_as.results_version;
        if (win) win->wake();
    };
    auto result = music::run_search(request.keyword, st, 12, 9000, 0, request.continuation,
        [&](const music::SearchResult& partial) { publish(partial, false); }, &request.pages);
    if (st.cancelled()) return;
    publish(result, true);
    music::fill_covers(result.tracks, st);
    if (!st.cancelled()) publish(result, true);
}
static void dispatch_search(SearchRequest request) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        request.generation = ++g_search_gen;
        g_as.searching = true; g_pending_search = std::move(request);
        if (g_search_worker) return;
        g_search_worker = true;
    }
    background::start([] {
        for (;;) {
            SearchRequest request;
            {
                std::lock_guard<std::mutex> lock(g_mu);
                if (g_shutdown.load() || !g_pending_search.generation) { g_search_worker = false; return; }
                request = std::move(g_pending_search); g_pending_search = {};
            }
            do_search(g_win, std::move(request));
        }
    });
}
void start_search() {
    if (music::text::trim(g_ui.query).empty()) { toast("请输入搜索关键词"); return; }
    if (!g_ui.src_mask) { toast("请至少开启一个音源"); return; }
    g_ui.scroll_to_top = true;
    SearchRequest request; request.keyword = g_ui.query; request.settings = g_ui.settings;
    for (int i = 0; i < music::kSourceCount; ++i) request.settings.enable[i] = (g_ui.src_mask >> i) & 1u;
    { std::lock_guard<std::mutex> lock(g_mu); g_search_keyword = request.keyword; g_search_pages = {}; g_search_continuation = {}; g_search_page_input = {}; }
    dispatch_search(std::move(request));
}
void search_more() {
    SearchRequest request;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_as.searching || g_search_keyword.empty()) return;
        request.keyword = g_search_keyword; request.settings = g_ui.settings; request.append = true;
        request.continuation = g_search_continuation;
        bool any = false;
        for (int i = 0; i < music::kSourceCount; ++i) {
            bool more = false;
            for (const auto& report : g_as.reports) if (int(report.source) == i) more = report.has_more && report.ok;
            request.settings.enable[i] = more && ((g_ui.src_mask >> i) & 1u); any |= request.settings.enable[i];
            request.pages[size_t(i)] = g_search_pages[size_t(i)] + 1;
        }
        if (!any) { toast("已经显示全部可用结果"); return; }
    }
    dispatch_search(std::move(request));
}
void retry_search_source(int source) {
    if (source < 0 || source >= music::kSourceCount) return;
    SearchRequest request;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_as.searching || g_search_keyword.empty()) return;
        request.keyword = g_search_keyword; request.settings = g_ui.settings; request.append = true;
        request.pages = g_search_pages; request.continuation = g_search_page_input;
        for (int i = 0; i < music::kSourceCount; ++i) request.settings.enable[i] = i == source;
    }
    dispatch_search(std::move(request));
}

// 按文件头判容器：兼容 MP3、M4A 等响应，不依赖下载地址的扩展名。
// 扩展名与内容一致，供缓存、下载和 AAC 转码选择正确的容器。
static std::string sniff_ext(const std::string& path) {
    std::FILE* f = platform::fopen(path, "rb");
    if (!f) return "mp3";
    unsigned char h[12] = {0};
    const size_t n = std::fread(h, 1, sizeof h, f);
    std::fclose(f);
    if (n >= 8 && std::memcmp(h + 4, "ftyp", 4) == 0) return "m4a";
    if (n >= 4 && std::memcmp(h, "fLaC", 4) == 0) return "flac";
    if (n >= 4 && std::memcmp(h, "RIFF", 4) == 0) return "wav";
    if (n >= 4 && std::memcmp(h, "OggS", 4) == 0) return "ogg";
    if (n >= 4 && h[0] == 0x1a && h[1] == 0x45 && h[2] == 0xdf && h[3] == 0xa3) return "webm";
    if (n >= 3 && std::memcmp(h, "ID3", 3) == 0) return "mp3";
    if (n >= 2 && h[0] == 0xFF && (h[1] & 0xF6) == 0xF0) return "aac";
    if (n >= 2 && h[0] == 0xFF && (h[1] & 0xE0) == 0xE0) return "mp3";
    return "mp3";
}

// 预加载仅拥有下一首的磁盘文件；不打开设备、不发布正在播放状态。
struct PreparedMedia {
    music::Variant variant;
    music::ResolveResult resolved;
    std::string original, playable, converted;
    bool owns_original = false;
    ~PreparedMedia() {
        if (owns_original && !original.empty()) platform::remove(original);
        if (!converted.empty()) platform::remove(converted);
    }
};
struct PreloadRequest {
    music::Track track;
    music::Settings settings;
    std::string key;
    std::atomic<bool> cancelled{false};
    unsigned serial = 0;
};
static std::mutex g_preload_mutex;
static std::shared_ptr<PreloadRequest> g_preload_request, g_preload_pending;
static std::shared_ptr<PreparedMedia> g_preload_ready, g_current_media;
static bool g_preload_worker = false;
static unsigned g_preload_serial = 0;

static std::string preload_key(const music::Track& track, const music::Settings& st) {
    std::string key = library::encode_track(track) + ":" + std::to_string(st.quality);
    key += ":" + std::to_string(st.lane_pref) + ":" + std::to_string(lx::Engine::instance().revision());
    for (bool enabled : st.lane_enable) key += enabled ? '1' : '0';
    for (bool enabled : st.enable) key += enabled ? '1' : '0';
    for (const auto* value : {&st.netease_cookie, &st.qq_cookie}) key += library::quote(*value);
    return key;
}
static void cancel_preload() {
    std::lock_guard<std::mutex> lock(g_preload_mutex);
    if (g_preload_request) g_preload_request->cancelled.store(true);
    g_preload_request.reset(); g_preload_pending.reset(); g_preload_ready.reset();
}
static std::shared_ptr<PreparedMedia> prepare_next(const std::shared_ptr<PreloadRequest>& request) {
    auto st = request->settings;
    st.cancelled = [request] { return g_shutdown.load() || request->cancelled.load(); };
    net::CancelScope network(st.cancelled);
    std::shared_lock<std::shared_mutex> cache_lock(g_play_cache_mutex);
    const auto& track = request->track;
    std::vector<int> order;
    if (track.variants.empty() || st.cancelled()) return {};
    order.push_back(std::clamp(track.chosen, 0, int(track.variants.size()) - 1));
    for (int i = 0; i < int(track.variants.size()); ++i) if (i != order[0]) order.push_back(i);
    for (int index : order) {
        if (st.cancelled()) return {};
        auto media = std::make_shared<PreparedMedia>();
        media->variant = track.variants[size_t(index)];
        const auto& variant = media->variant;
        const auto base = g_temp_dir + "preload-" + std::to_string(request->serial);
        std::string error;
        if (variant.source == music::Source::Local) {
            media->original = variant.id;
            if (!platform::exists(media->original)) continue;
        } else {
            const auto result = music::resolve(variant, st, [&](const music::ResolveResult& resolved, std::string& reason) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
                net::CancelScope candidate([parent = net::cancel_current, deadline] {
                    return (parent && parent()) || std::chrono::steady_clock::now() >= deadline;
                });
                const auto probe = net::probe_audio(resolved.url, resolved.headers, 6000);
                if (!probe.playable) { reason = probe.reason; return false; }
                if ((variant.source == music::Source::Netease || variant.source == music::Source::QQ) &&
                    probe.total_bytes > 0 && variant.duration_ms > 0 && probe.total_bytes * 5 < variant.duration_ms * 16LL * 3)
                    return false;
                if (net::cancelled()) return false;
                media->original = base + ".mp3"; media->owns_original = true;
                if (!net::download(resolved.url, media->original, resolved.headers, {}, request->cancelled, reason)) return false;
                const auto extension = sniff_ext(media->original);
                if (extension != "mp3") {
                    const auto renamed = base + "." + extension;
                    if (!platform::replace_file(media->original, renamed)) return false;
                    media->original = renamed;
                }
                media->converted = base + ".pcm.wav";
                if (!audio::prepare_file(media->original, media->playable, reason, media->converted,
                    [] { return net::cancelled(); }) || net::cancelled()) return false;
                media->resolved = resolved;
                return true;
            });
            if (result.ok && !st.cancelled()) return media;
            continue;
        }
        media->converted = base + ".pcm.wav";
        if (audio::prepare_file(media->original, media->playable, error, media->converted, st.cancelled) && !st.cancelled()) return media;
    }
    return {};
}
static void request_preload(const music::Track& track, const music::Settings& settings) {
    const auto key = preload_key(track, settings);
    {
        std::lock_guard<std::mutex> lock(g_preload_mutex);
        if (g_preload_request && g_preload_request->key == key) return;
        if (g_preload_request) g_preload_request->cancelled.store(true);
        auto request = std::make_shared<PreloadRequest>();
        request->track = track; request->settings = settings; request->key = key; request->serial = ++g_preload_serial;
        g_preload_request = request; g_preload_pending = request; g_preload_ready.reset();
        if (g_preload_worker) return;
        g_preload_worker = true;
    }
    background::start([] {
        for (;;) {
            std::shared_ptr<PreloadRequest> request;
            {
                std::lock_guard<std::mutex> lock(g_preload_mutex);
                if (!g_preload_pending || g_shutdown.load()) { g_preload_worker = false; return; }
                request = std::move(g_preload_pending);
            }
            auto ready = prepare_next(request);
            {
                std::lock_guard<std::mutex> lock(g_preload_mutex);
                if (g_preload_request == request && !request->cancelled.load()) g_preload_ready = std::move(ready);
            }
        }
    });
}
static std::shared_ptr<PreparedMedia> take_preloaded(const music::Track& track, const music::Settings& settings) {
    const auto key = preload_key(track, settings);
    std::lock_guard<std::mutex> lock(g_preload_mutex);
    std::shared_ptr<PreparedMedia> result;
    if (g_preload_request && g_preload_request->key == key) result = std::move(g_preload_ready);
    if (g_preload_request) g_preload_request->cancelled.store(true);
    g_preload_request.reset(); g_preload_pending.reset(); g_preload_ready.reset();
    return result;
}

// 下载完成 → 交给主线程开 miniaudio
static void finish_play(HostCallbacks* win, const music::Variant& v, const std::string& path, int gen,
                        bool is_preview, const std::string& playable = {}) {
    const audio::MediaInfo mi = audio::inspect_mp3(path);
    int kbps = mi.bitrate_kbps;
    if (!mi.valid && v.duration_ms > 0) {
        // m4a 没帧头可读，用 文件大小 / 时长 估平均码率（比"码率未解析"有用）
        const auto sz = platform::file_size(path);
        if (sz > 0) kbps = int(double(sz) * 8.0 / double(v.duration_ms));
    }
    char qbuf[160];
    if (mi.valid)
        std::snprintf(qbuf, sizeof qbuf, "%s · %d kbps · %d Hz · %s", music::source_name(v.source),
                      mi.bitrate_kbps, mi.sample_rate, mi.channels == 1 ? "单声道" : "立体声");
    else if (kbps > 0)
        std::snprintf(qbuf, sizeof qbuf, "%s · %d kbps · %s", music::source_name(v.source), kbps,
                      sniff_ext(path) == "m4a" ? "AAC/m4a" : "未知容器");
    else
        std::snprintf(qbuf, sizeof qbuf, "%s · 码率未能解析", music::source_name(v.source));
    std::string quality = qbuf;
    if (is_preview) quality += " · 试听片段";
    char sbuf[64];
    if (kbps > 0) std::snprintf(sbuf, sizeof sbuf, "%d kbps%s", kbps, is_preview ? " 试听" : "");
    else std::snprintf(sbuf, sizeof sbuf, "%s", music::source_name(v.source));

    std::lock_guard<std::mutex> lk(g_mu);
    if (gen != g_as.gen) return;
    g_as.pending_path = playable.empty() ? path : playable;
    g_as.now_source = int(v.source);
    // 补上"这条流是谁给的"：渠道 · 实现（音源脚本名 / providers 文件名 / 内置）
    std::string via;
    if (g_as.now_lane >= 0) {
        via = std::string(" · ") + music::lane_name(music::Lane(g_as.now_lane));
        if (!g_as.now_engine.empty()) via += " · " + g_as.now_engine;
    }
    g_as.now_quality = quality + via;
    g_as.now_short = sbuf;
    g_as.pending_duration_ms = v.duration_ms;
    g_as.buffer = 1.0f;
    g_as.ps = PlayState::Ready;
    g_as.error = is_preview ? "该曲目可用音源只给试听片段，放的是最长的一段" : std::string();
    win->wake();
}

static void finish_stream(HostCallbacks* win, const music::Variant& variant, const audio::StreamSpec& spec,
                          std::shared_ptr<audio::Stream> stream, int gen, const std::string& detail = {}) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_shutdown.load() || gen != g_as.gen) return;
    g_pending_stream = std::move(stream); g_active_stream_spec = spec;
    g_as.pending_path = "stream:"; g_as.pending_duration_ms = spec.duration_ms;
    g_as.now_source = int(variant.source);
    // detail 为空 = 拖动进度条后重开流，此时沿用上一次的音质说明，别把它抹成只剩平台名
    if (!detail.empty()) g_as.now_quality = std::string(music::source_name(variant.source)) + " · " + detail;
    else if (g_as.now_quality.empty()) g_as.now_quality = music::source_name(variant.source);
    g_as.now_short = "边下边播"; g_as.buffer = 1; g_as.ps = PlayState::Ready; g_as.error.clear();
    if (win) win->wake();
}

// 取流 + 下载；失败沿其它源换源（跨源自动换源）
//
static void do_play(HostCallbacks* win, music::Track track, music::Settings st, int gen, bool stream_allowed = true) {
    const auto queue_uid = track_uid(track);
    st.cancelled = [gen] { return g_shutdown.load() || gen != g_play_generation.load(); };
    net::CancelScope network(st.cancelled);
    std::shared_lock<std::shared_mutex> cache_write(g_play_cache_mutex);
    if (!track.variants.empty() && track.current().source == music::Source::Local) {
        const auto& v = track.current();
        std::string playable, error;
        const auto cache = g_temp_dir + "track-" + std::to_string(gen) + ".pcm.wav";
        if (stream_allowed && platform::exists(v.id) && !audio::inspect_mp3(v.id).valid) {
            audio::StreamSpec spec; spec.local_path = v.id; spec.duration_ms = v.duration_ms > 0 ? v.duration_ms : track.duration_ms;
            { std::lock_guard<std::mutex> lock(g_mu); spec.start_ms = g_as.position_ms; }
            auto stream = audio::Stream::open(spec, st.cancelled, error);
            if (stream) { finish_stream(win, v, spec, std::move(stream), gen); return; }
            if (st.cancelled()) return;
        }
        if (platform::exists(v.id) && audio::prepare_file(v.id, playable, error, cache, st.cancelled)) {
            finish_play(win, v, v.id, gen, false, playable); return;
        }
        std::lock_guard<std::mutex> lock(g_mu);
        if (gen != g_as.gen) return;
        g_as.ps = PlayState::Failed; g_track_failure_gen = gen; g_as.now_quality = "本地文件不可用";
        g_as.error = error.empty() ? "本地文件已移动或删除：" + v.id : error;
        win->wake(); return;
    }
    std::vector<int> order;
    if (!track.variants.empty()) {
        const int first = std::min<int>(track.chosen, int(track.variants.size()) - 1);
        order.push_back(first);
        for (int i = 0; i < int(track.variants.size()); ++i)
            if (i != first) order.push_back(i);
    }

    std::string reasons;
    auto add_reason = [&](const std::string& s) {
        if (s.empty()) return;
        if (!reasons.empty()) reasons += "；";
        reasons += s;
    };

    // 试听片段兜底：所有源都只有片段时，至少放最长的那一段，并如实标注
    bool have_preview = false;
    music::Variant preview_variant;
    music::ResolveResult preview_rr;
    long long preview_bytes = 0;

    // 补位成功时把候选写回列表：药丸上就能看到它，下次直接命中，不用再搜一遍
    auto publish_variant = [&](const music::Variant& v) {
        std::lock_guard<std::mutex> lk(g_mu);
        if (gen != g_as.gen) return;
        // 解析期间可能通过右键重排队列，不能再写回启动任务时的旧行号。
        const auto found = std::find_if(g_as.tracks.begin(), g_as.tracks.end(), [&](const auto& item) { return track_uid(item) == queue_uid; });
        if (found == g_as.tracks.end()) return;
        music::Track& t = *found;
        for (const auto& x : t.variants)
            if (music::same_variant(x, v)) return;
        t.variants.push_back(v);
        ++g_as.tracks_version;
    };

    // 一个候选的完整尝试：取流 → 探测完整性 → 缓冲或下载 → 交给播放器。
    // 返回 true = 不必再试别的候选（成功了，或者用户已经切歌）。
    auto attempt = [&](const music::Variant& v) -> bool {
        if (g_shutdown.load()) return true;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (gen != g_as.gen) return true;  // 用户点了别的歌，别抢
            g_as.ps = PlayState::Buffering;
            g_as.buffer = 0.0f;
            g_as.error.clear();
        }
        win->wake();

        const auto result = music::resolve(v, st, [&](const music::ResolveResult& rr, std::string& candidate_error) {
            const auto loading_begin = std::chrono::steady_clock::now();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
            net::CancelScope candidate([parent = net::cancel_current, deadline] {
                return (parent && parent()) || std::chrono::steady_clock::now() >= deadline;
            });
            {
                std::lock_guard<std::mutex> lk(g_mu);
                if (gen != g_as.gen) return true;
                g_as.now_source = int(v.source);
                g_as.now_lane = int(rr.lane);
                g_as.now_engine = rr.engine;
                // 原曲在哪个平台 vs 这条流实际从哪来：跨源补位时两者不同，界面要如实分开写
                g_as.now_origin = int(track.current().source);
                g_as.now_swapped = v.source != track.current().source;
                g_as.now_quality = rr.detail.empty() ? std::string("取流中") : rr.detail;
            }

            // 探体积：只取 2 字节，代价极小
            const net::AudioProbe probe = net::probe_audio(rr.url, rr.headers, 6000);
            if (!probe.playable) {
                candidate_error = probe.reason.empty() ? "音频地址不可用" : probe.reason;
                return false;
            }
            const long long expected = v.duration_ms > 0 ? v.duration_ms * 16LL : 0;  // ms × 128kbps/8/1000
            const bool preview =
                (v.source == music::Source::Netease || v.source == music::Source::QQ) && probe.total_bytes > 0 && expected > 0 && probe.total_bytes * 5 < expected * 3;  // < 60%
            if (preview) {
                if (probe.total_bytes > preview_bytes) {
                    preview_bytes = probe.total_bytes;
                    preview_variant = v;
                    preview_rr = rr;
                    have_preview = true;
                }
                add_reason(std::string(music::source_name(v.source)) + " 仅试听片段");
                return false;
            }

            if (stream_allowed) {
                audio::StreamSpec spec; spec.url = rr.url; spec.headers = rr.headers; spec.duration_ms = v.duration_ms > 0 ? v.duration_ms : track.duration_ms;
                { std::lock_guard<std::mutex> lock(g_mu); spec.start_ms = g_as.position_ms; }
                std::string error;
                auto stream = audio::Stream::open(spec, st.cancelled, error);
                if (stream && !net::cancelled()) {
                    if (rr.load_timing) rr.load_timing->ready_ms = int(std::clamp<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - loading_begin).count(), 1, 300000));
                    finish_stream(win, v, spec, std::move(stream), gen, rr.detail); return true;
                }
                if (st.cancelled()) return true;
                if (net::cancelled()) { candidate_error = "音源已移除或缓冲超时"; return false; }
                // 尾部 moov 等需要随机访问的容器，退回原有完整下载与文件解码。
            }

            const std::string path = g_temp_dir + "track-" + std::to_string(gen) + ".mp3";
            std::atomic<bool> cancel{false};
            auto last_wake = std::chrono::steady_clock::now();
            std::string err;
            const bool ok = net::download(
                rr.url, path, rr.headers,
                [&](long long got, long long total) {
                    std::lock_guard<std::mutex> lk(g_mu);
                    if (g_shutdown.load() || gen != g_as.gen) { cancel.store(true); return; }
                    g_as.cached_bytes = got;
                    g_as.buffer = total > 0 ? float(double(got) / double(total)) : 0.0f;
                    const auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_wake).count() > 80) {
                        last_wake = now;
                        win->wake();
                    }
                },
                cancel, err);

            if (g_shutdown.load()) return true;
            if (!ok) {
                candidate_error = err.empty() ? "下载超时或失败" : err;
                add_reason(std::string(music::source_name(v.source)) + " 下载失败（" + err + "）");
                return false;
            }
            // 缓存保留真实扩展名，AAC 转码和下载共用同一份格式识别结果。
            std::string final_path = path;
            if (const std::string ext = sniff_ext(path); ext != "mp3") {
                final_path = g_temp_dir + "track-" + std::to_string(gen) + "." + ext;
                platform::replace_file(path, final_path);
            }
            std::string playable;
            if (!audio::prepare_file(final_path, playable, err, {}, [] { return net::cancelled(); }) || net::cancelled()) {
                candidate_error = err;
                add_reason(err);
                return false;
            }
            finish_play(win, v, final_path, gen, /*is_preview=*/false, playable);
            return true;
        });
        if (!result.ok && !st.cancelled())
            add_reason(std::string(music::source_name(v.source)) + " " + result.reason);
        return result.ok || st.cancelled();
    };

    // 一、列表里已有的候选，按"选中的源优先"逐个试
    for (int idx : order) {
        if (attempt(track.variants[size_t(idx)])) return;
    }

    // 二、列表里一条能播的都没有 → 按「歌名 + 歌手」去别的源补位。
    //     歌单里的曲目只有本源那一条 id，本源不可用时只在列表内换源是死路，
    //     必须真的去搜 —— 这就是"自动换源"。
    const bool selected_part = track.current().source == music::Source::Bilibili && !track.current().native_id.empty();
    if (!track.title.empty() && !selected_part) {
        for (int si = 0; si < music::kSourceCount; ++si) {
            if (g_shutdown.load()) return;
            if (!st.enable[si]) continue;
            bool already = false;
            for (const auto& v : track.variants)
                if (int(v.source) == si) { already = true; break; }
            if (already) continue;

            music::Variant got;
            std::string e;
            if (!music::find_on_source(music::Source(si), track, st, got, e)) {
                add_reason(e);
                continue;
            }
            const bool done = attempt(got);
            if (done) {
                publish_variant(got);
                return;
            }
            track.variants.push_back(got);  // 记下来，后面的源不再重复搜同一个
        }
    }

    // 三、只有试听片段：放最长的那段，并在音质栏写明
    if (have_preview) {
        const std::string path = g_temp_dir + "track-" + std::to_string(gen) + ".mp3";
        std::atomic<bool> cancel{false};
        std::string err;
        if (net::download(
                preview_rr.url, path, preview_rr.headers,
                [&](long long got, long long total) {
                    std::lock_guard<std::mutex> lk(g_mu);
                    if (g_shutdown.load() || gen != g_as.gen) { cancel.store(true); return; }
                    g_as.cached_bytes = got;
                    g_as.buffer = total > 0 ? float(double(got) / double(total)) : 0.0f;
                },
                cancel, err)) {
            std::string playable;
            if (audio::prepare_file(path, playable, err, {}, st.cancelled)) {
                finish_play(win, preview_variant, path, gen, /*is_preview=*/true, playable);
                return;
            }
        }
        add_reason(err.empty() ? "片段下载失败" : err);
    }

    if (g_shutdown.load()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (gen != g_as.gen) return;
    g_as.ps = PlayState::Failed;
    g_track_failure_gen = gen;
    g_as.now_quality = "不可用";
    g_as.error = reasons.empty() ? "没有可用音源"
                                 : ("取流失败 — " + reasons);
    win->wake();
}

// =============================================================================
//  三、歌词 / 下载（都在后台线程跑）
// =============================================================================

void toast(const std::string& text, int ms) {
    post_ui([text, ms] {
        g_ui.toast = text;
        g_toast_until.store(platform::ticks() + static_cast<unsigned long long>(ms));
    });
}

// ---------------- 歌词 ----------------
// 取一次就缓存住：同一个 uid 不重复请求。切歌后旧结果会被丢弃（比对 for_uid）。
static void request_lyrics(const music::Track& track, bool force = false) {
    unsigned long long generation = 0;
    const std::string uid = track_uid(track);
    if (uid.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_lyr.m);
        if (!force && g_lyr.for_uid == uid && (g_lyr.doc || g_lyr.loading)) return;
        generation = ++g_lyr.generation;
        g_lyr.for_uid = uid;
        g_lyr.doc.reset();
        g_lyr.loading = true;
    }
    music::Track t = track;
    const auto found = g_library->lyric_preferences.find(uid);
    const std::string local = found == g_library->lyric_preferences.end() ? "" : found->second.path;
    music::Settings st = g_ui.settings;
    background::start([t, st, uid, local, generation] {
        net::CancelScope signal([uid, generation] {
            if (g_shutdown.load()) return true;
            std::lock_guard<std::mutex> lock(g_lyr.m);
            return g_lyr.for_uid != uid || g_lyr.generation != generation;
        });
        lyr::Doc d;
        if (local.empty()) d = lyr::fetch(t, st, 8000, [uid, generation](const lyr::Doc& partial) {
            auto doc = std::make_shared<const lyr::Doc>(partial);
            {
                std::lock_guard<std::mutex> lock(g_lyr.m);
                if (g_lyr.for_uid != uid || g_lyr.generation != generation) return;
                g_lyr.doc = std::move(doc);
            }
            if (g_win) g_win->wake();
        });
        else {
            d.origin = "自选本地 LRC";
            const auto size = platform::file_size(local);
            if (size <= 0 || size > 4 * 1024 * 1024) d.error = "本地歌词不存在、为空或超过 4 MiB，可重新选择或恢复默认";
            else {
                std::ifstream file(platform::fs::u8path(local), std::ios::binary);
                std::string text(4 * 1024 * 1024 + 1, '\0');
                file.read(text.data(), std::streamsize(text.size())); text.resize(size_t(file.gcount()));
                if (text.size() > 4 * 1024 * 1024) { d.error = "本地歌词超过 4 MiB"; text.clear(); }
                if (text.rfind("\xef\xbb\xbf", 0) == 0) text.erase(0, 3);
                d.lines = lyr::parse(text, &d.synced);
                if (d.lines.empty()) d.error = "本地歌词没有可显示内容";
            }
        }
        if (g_shutdown.load()) return;
        auto shared = std::make_shared<const lyr::Doc>(std::move(d));
        {
            std::lock_guard<std::mutex> lk(g_lyr.m);
            if (g_lyr.for_uid != uid || g_lyr.generation != generation) return; // 切歌或重选歌词后丢弃旧请求。
            g_lyr.doc = shared;
            g_lyr.loading = false;
        }
        if (g_win) g_win->wake();
    });
}

// ---------------- 下载 ----------------
static std::string sanitize_filename(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        const bool bad = c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
                         c == '>' || c == '|' || c < 0x20;
        out += bad ? '_' : char(c);
    }
    // Windows 会吃掉结尾的点和空格，不清掉的话"保存了却找不到同名文件"
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    // 按 UTF-8 字符边界截断到 100 字节，别把一个汉字切成两半
    if (out.size() > 100) {
        size_t cut = 100;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out = out.substr(0, cut);
    }
    return out.empty() ? std::string("track") : out;
}

static long long file_size_of(const std::string& path) { return platform::file_size(path); }
std::string default_download_dir() { return platform::download_dir(); }
static std::string effective_download_dir() {
    return g_ui.download_dir.empty() ? default_download_dir() : g_ui.download_dir;
}
static void make_dirs(const std::string& dir) { platform::make_dirs(dir); }

static void dl_set(const std::string& uid, int percent, int state, const std::string& err, const std::shared_ptr<std::atomic<bool>>& token) {
    // state: 0 进行中 / 1 完成 / 2 失败
    std::lock_guard<std::mutex> lk(g_dlm);
    const auto active = g_download_cancel.find(uid);
    if (active == g_download_cancel.end() || active->second != token || (token->load() && state != 1)) return;
    auto it = g_dls.find(uid);
    if (it == g_dls.end()) return;
    if (percent >= 0) it->second.percent = percent;
    it->second.done = (state == 1);
    it->second.failed = (state == 2);
    if (!err.empty()) it->second.err = err;
    ++g_download_revision;
}

static std::string publish_download(const std::string& part, const std::string& dir,
                                    const std::string& stem, const std::string& extension, std::string& error) {
    for (unsigned suffix = 0; suffix < 100000; ++suffix) {
        const auto name = stem + (suffix ? " (" + std::to_string(suffix + 1) + ")" : "") + "." + extension;
        const auto target = platform::join(dir, name);
        if (platform::move_file_no_replace(part, target)) return target;
        if (!platform::exists(target)) { error = "无法保存下载文件，请检查目录权限或剩余空间"; return {}; }
    }
    error = "同名下载文件过多，请更换目录或歌曲名称";
    return {};
}


void cancel_download(const std::string& uid) {
    std::lock_guard<std::mutex> lock(g_dlm);
    const auto found = g_download_cancel.find(uid);
    if (found == g_download_cancel.end()) return;
    found->second->store(true);
    auto& item = g_dls[uid];
    // 排他发布一旦开始就完成提交；取消只跳过之后的标签，不开放同曲目重试。
    if (!item.done && !item.publishing) { item.failed = true; item.err = "已取消"; ++g_download_revision; }
}
// 沿所有源试一遍，第一个"完整曲目"落盘；三源都只有试听片段就把最长的那段存下来并标注
void start_download(music::Track track, music::Settings st) {
    if (track.variants.empty()) return;
    if (track.current().source == music::Source::Local) { toast("该歌曲已在本机：" + track.current().id, 6000); return; }
    const std::string uid = track_uid(track);
    if (uid.empty()) return;
    auto token = std::make_shared<std::atomic<bool>>(false);
    st.cancelled = [token] { return g_shutdown.load() || token->load(); };

    {
        std::lock_guard<std::mutex> lk(g_dlm);
        auto it = g_dls.find(uid);
        if (it != g_dls.end() && it->second.publishing) {
            toast("正在保存文件，请稍候");
            return;
        }
        if (it != g_dls.end() && !it->second.failed) {
            // 已完成 / 正在下载：都不该再起一个线程。失败过的允许重试。
            if (it->second.done) toast("这首已经下载过了：" + it->second.name);
            else {
                const auto cancel = g_download_cancel.find(uid);
                if (cancel != g_download_cancel.end()) cancel->second->store(true);
                it->second.failed = true; it->second.err = "已取消"; ++g_download_revision;
                toast("已取消下载：" + track.title);
            }
            return;
        }
        if (g_download_pending.size() >= 32) { toast("下载等待队列已满，请稍后再添加"); return; }
        // 已结束记录最多保留 200 条，运行/保存中的任务不淘汰。
        if (g_dls.size() >= 200) for (auto old = g_dls.begin(); old != g_dls.end();) {
            if ((old->second.done || old->second.failed) && !g_download_cancel.count(old->first)) { old = g_dls.erase(old); if (g_dls.size() < 200) break; }
            else ++old;
        }
        DlItem item; item.track = track; ++g_download_revision;
        item.name = sanitize_filename(track.artist.empty() ? track.title : track.artist + " - " + track.title) + ".mp3";
        g_dls[uid] = item;
        g_download_cancel[uid] = token;
    }

    const std::string dir = effective_download_dir();
    make_dirs(dir);
    toast("开始下载：" + track.title);

    auto action = [track, st, uid, dir, token] {
        struct Finished {
            const std::string& uid; const std::shared_ptr<std::atomic<bool>>& token;
            ~Finished() {
                std::lock_guard<std::mutex> lock(g_dlm);
                const auto found = g_download_cancel.find(uid);
                if (found != g_download_cancel.end() && found->second == token) g_download_cancel.erase(found);
            }
        } finished{uid, token};
        net::CancelScope network(st.cancelled);
        if (st.cancelled()) return;
        std::string name;
        {
            std::lock_guard<std::mutex> lk(g_dlm);
            auto it = g_dls.find(uid);
            if (it == g_dls.end()) return;
            name = it->second.name; it->second.queued = false; ++g_download_revision;
        }
        const std::string stem = name.size() > 4 ? name.substr(0, name.size() - 4) : name;
        static std::atomic<unsigned long long> serial{0};
        const std::string part = platform::join(dir, ".aino-download-" + std::to_string(platform::process_id()) +
            "-" + std::to_string(platform::ticks()) + "-" + std::to_string(++serial) + ".part");
        struct PartialFile { std::string path; ~PartialFile() { platform::remove(path); } } partial{part};

        std::string reasons;
        std::string best_url;
        std::vector<std::string> best_headers;
        long long best_bytes = 0;
        bool saved = false;
        bool saved_preview = false;

        auto try_one = [&](const music::Variant& v, bool allow_preview) -> bool {
            const auto result = music::resolve(v, st, [&](const music::ResolveResult& rr, std::string& candidate_error) {
                if (net::cancelled()) { candidate_error = "下载已取消"; return false; }
                if (!allow_preview) {
                    const net::AudioProbe probe = net::probe_audio(rr.url, rr.headers, 8000);
                    if (!probe.playable) { candidate_error = probe.reason; return false; }
                    const long long expected = v.duration_ms > 0 ? v.duration_ms * 16LL : 0;
                    const bool preview =
                        (v.source == music::Source::Netease || v.source == music::Source::QQ) && probe.total_bytes > 0 && expected > 0 && probe.total_bytes * 5 < expected * 3;
                    if (preview) {
                        if (probe.total_bytes > best_bytes) {
                            best_bytes = probe.total_bytes;
                            best_url = rr.url;
                            best_headers = rr.headers;
                        }
                        if (!reasons.empty()) reasons += "；";
                        reasons += std::string(music::source_name(v.source)) + " 仅试听片段";
                        return false;
                    }
                }
                auto& cancel = *token;
                std::string err;
                auto last = std::chrono::steady_clock::now();
                const bool ok = net::download(
                    rr.url, part, rr.headers,
                    [&](long long got, long long total) {
                        if (total <= 0) return;
                        const auto now = std::chrono::steady_clock::now();
                        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() < 120) return;
                        last = now;
                        dl_set(uid, int(double(got) / double(total) * 100.0), 0, std::string(), token);
                        if (g_win) g_win->wake();
                    },
                    cancel, err, 60000);
                if (!ok) {
                    candidate_error = err;
                    if (!reasons.empty()) reasons += "；";
                    reasons += std::string(music::source_name(v.source)) + " 下载失败（" + err + "）";
                    return false;
                }
                // 落盘后按**实际字节数**再复核一次：有些 CDN 用 chunked 编码不给
                // Content-Length，下载前的探测拿不到 total，会漏判成"完整曲目"，
                // 结果用户下到的是一个 11 秒片段还以为是整首。
                const long long got_bytes = file_size_of(part);
                const long long expected2 = (v.duration_ms > 0 ? v.duration_ms : track.duration_ms) * 16LL;
                if ((v.source == music::Source::Netease || v.source == music::Source::QQ) && got_bytes > 0 && expected2 > 0 && got_bytes * 5 < expected2 * 3) {
                    platform::remove(part);
                    if (got_bytes > best_bytes) {
                        best_bytes = got_bytes;
                        best_url = rr.url;
                        best_headers = rr.headers;
                    }
                    if (!reasons.empty()) reasons += "；";
                    reasons += std::string(music::source_name(v.source)) + " 仅试听片段";
                    return false;
                }
                return true;
            });
            if (!result.ok && !st.cancelled()) {
                if (!reasons.empty()) reasons += "; ";
                reasons += result.reason;
            }
            return result.ok;
        };

        for (const auto& v : track.variants) {
            if (st.cancelled()) return;
            if (try_one(v, false)) {
                saved = true;
                break;
            }
        }
        // 没有网易云变体时，按标题和歌手寻找时长匹配的同曲候选。
        if (!saved) {
            const bool has_netease =
                std::any_of(track.variants.begin(), track.variants.end(), [](const music::Variant& v) {
                    return v.source == music::Source::Netease;
                });
            if (!has_netease) {
                std::string serr;
                const auto cands =
                    music::search_netease(track.title + " " + track.artist, 6, 6000, serr, st);
                const std::string want = music::text::normalize_title(track.title);
                for (const auto& v : cands) {
                    if (music::text::normalize_title(v.title) != want) continue;
                    if (track.duration_ms > 0 && v.duration_ms > 0 &&
                        std::abs(v.duration_ms - track.duration_ms) > 10000)
                        continue;
                    if (try_one(v, false)) {
                        saved = true;
                        break;
                    }
                }
            }
        }

        if (!saved && !best_url.empty()) {
            auto& cancel = *token;
            std::string err;
            if (net::download(best_url, part, best_headers,
                              [&](long long got, long long total) {
                                  if (total <= 0) return;
                                  dl_set(uid, int(double(got) / double(total) * 100.0), 0, std::string(), token);
                                  if (g_win) g_win->wake();
                              },
                              cancel, err, 60000)) {
                saved = true;
                saved_preview = true;
            }
        }

        if (st.cancelled()) return;
        if (!saved) {
            platform::remove(part);
            const std::string why = reasons.empty() ? "没有可用音源" : reasons;
            dl_set(uid, 0, 2, why, token);
            if (g_win) {
                // 失败原因必须露出来：行内只有一个警告图标，不说清就只能靠猜
                toast("下载失败：" + why, 7000);
                g_win->wake();
            }
            return;
        }
        if (st.cancelled()) return;
        std::string publish_error;
        std::string final_path;
        {
            std::lock_guard<std::mutex> lock(g_dlm);
            const auto found = g_download_cancel.find(uid);
            if (st.cancelled() || found == g_download_cancel.end() || found->second != token) return;
            g_dls[uid].publishing = true; ++g_download_revision;
        }
        // rename 在部分文件系统会退回整文件复制，不能持有界面也会读取的状态锁。
        try {
            final_path = publish_download(part, dir, stem + (saved_preview ? " (试听)" : ""), sniff_ext(part), publish_error);
        } catch (const std::exception& error) {
            publish_error = std::string("无法保存下载文件：") + error.what();
        } catch (...) {
            publish_error = "无法保存下载文件";
        }
        const auto final_name = final_path.empty() ? std::string() : platform::fs::u8path(final_path).filename().u8string();
        {
            std::lock_guard<std::mutex> lock(g_dlm);
            auto& item = g_dls[uid];
            item.publishing = false; ++g_download_revision;
            if (!final_path.empty()) {
                item.name = final_name; item.path = final_path; item.err.clear();
                item.percent = 100; item.done = true; item.failed = false;
            } else {
                item.done = false; item.failed = true; item.err = publish_error;
            }
        }
        if (final_path.empty()) {
            toast(publish_error, 7000);
            if (g_win) g_win->wake();
            return;
        }
        // 内嵌封面 / 歌词：先拿素材，再写标签。写失败不影响"文件已经下好了"这个事实，
        // 只额外提示一句，别把一次成功的下载报成失败。
        std::string tag_note;
        if (!st.cancelled() && (st.embed_cover || st.embed_lyrics)) {
            std::string cover_bytes;
            bool is_png = false;
            if (st.embed_cover && !track.cover_url.empty()) {
                const net::Response cr =
                    net::get(track.cover_url, {"User-Agent: Mozilla/5.0", "Referer: https://music.163.com/"},
                             15000);
                if (cr.status == 200 && cr.body.size() > 256) {
                    cover_bytes.assign(cr.body.begin(), cr.body.end());
                    is_png = cover_bytes.size() > 8 && uint8_t(cover_bytes[0]) == 0x89 &&
                             cover_bytes[1] == 'P';
                }
            }
            std::string lrc_text;
            if (st.embed_lyrics && !st.cancelled()) {
                const lyr::Doc d = lyr::fetch(track, st);
                if (!d.lines.empty()) lrc_text = lyr::to_lrc(d);
            }
            std::string terr;
            if (!st.cancelled() && !tag::embed(final_path, cover_bytes, is_png, lrc_text, terr) && !terr.empty())
                tag_note = "（内嵌失败：" + terr + "）";
        }

        dl_set(uid, 100, 1, std::string(), token);
        if (g_win) {
            std::string shown = final_path;
            const size_t slash = shown.find_last_of("\\/");
            if (slash != std::string::npos) shown = shown.substr(slash + 1);
            toast((saved_preview ? "只拿到试听片段，已另存为：" : "已下载：") + shown + tag_note, 5000);
            g_win->wake();
        }
    };
    int launch = 0;
    {
        std::lock_guard<std::mutex> lock(g_dlm);
        g_download_pending.push_back(std::move(action));
        launch = std::min(2 - g_download_workers, int(g_download_pending.size()));
        g_download_workers += launch;
    }
    for (int i = 0; i < launch; ++i) background::start([] {
        for (;;) {
            std::function<void()> next;
            {
                std::lock_guard<std::mutex> lock(g_dlm);
                if (g_shutdown.load() || g_download_pending.empty()) { --g_download_workers; return; }
                next = std::move(g_download_pending.front()); g_download_pending.pop_front();
            }
            next();
        }
    });
}

// =============================================================================
//  四、QQ 音乐（登录 / 歌单 / 同步）
// =============================================================================

static void qq_snapshot() {
    static unsigned revision = 0;
    std::lock_guard<std::mutex> lk(g_qq.m);
    g_qq_snap.logged = g_qq.logged;
    g_qq_snap.loading = g_qq.loading;
    g_qq_snap.nickname = g_qq.nickname;
    g_qq_snap.err = g_qq.err;
    if (revision != g_qq.playlists_revision) {
        g_qq_snap.playlists = g_qq.playlists; revision = g_qq.playlists_revision;
    }
}

void qq_refresh() {
    std::string cookie, uin;
    {
        std::lock_guard<std::mutex> lk(g_qq.m);
        g_qq.loading = true;
        g_qq.err.clear();
        cookie = g_qq.cookie;
        uin = g_qq.uin;
    }
    if (g_win) g_win->wake();
    background::start([cookie, uin] {
        qq::Profile p = qq::fetch_profile(cookie, uin);
        if (g_shutdown.load()) return;
        std::lock_guard<std::mutex> lk(g_qq.m);
        if (g_qq.cookie != cookie || g_qq.uin != uin) return;
        g_qq.loading = false;
        if (!p.ok()) {
            g_qq.err = p.error;
        } else {
            g_qq.nickname = p.nickname;
            g_qq.playlists = std::move(p.playlists);
            ++g_qq.playlists_revision;
            g_qq.err.clear();
        }
        if (g_win) g_win->wake();
    });
}

// 歌单打开只进入详情；用户点播时才建立播放队列。
void qq_open_playlist(int idx) {
    if (idx < 0 || idx >= int(g_qq_snap.playlists.size())) return;
    const auto& p = g_qq_snap.playlists[size_t(idx)];
    library::Playlist playlist;
    playlist.name = p.name; playlist.cover = p.cover; playlist.remote_id = p.dirid;
    playlist.remote_owner = g_ui.qq_uin; playlist.source = music::Source::QQ;
    playlist_open_remote(std::move(playlist));
}

// 把当前列表里属于某个源的歌加进指定歌单。
// site：0 = 网易云，1 = QQ —— 两个站点的写入接口完全不同，但外层流程一模一样
// （取目标 → 挑同源的 id → 后台写 → toast 回显），所以合并成一个函数。
void playlist_sync_to(int site, int idx) {
    if (site != 0 && site != 1) return;
    if (g_sync_site.load() >= 0) { toast("正在同步歌单，请稍候"); return; }
    std::string cookie, uid, pid, name;
    if (site == 1) {
        std::lock_guard<std::mutex> lk(g_qq.m);
        if (idx < 0 || idx >= int(g_qq_snap.playlists.size())) return;
        cookie = g_qq.cookie;
        uid = g_qq.uin;
        pid = g_qq_snap.playlists[size_t(idx)].dirid;
        name = g_qq_snap.playlists[size_t(idx)].name;
    } else {
        std::lock_guard<std::mutex> lk(g_ne.m);
        if (idx < 0 || idx >= int(g_ne_snap.playlists.size())) return;
        cookie = g_ne.cookie;
        uid = g_ne.uid;
        pid = g_ne_snap.playlists[size_t(idx)].id;
        name = g_ne_snap.playlists[size_t(idx)].name;
    }

    const music::Source want = site == 1 ? music::Source::QQ : music::Source::Netease;
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (const auto& t : g_as.tracks) {
            for (const auto& v : t.variants) {
                if (v.source != want) continue;
                const std::string id = v.native_id.empty() ? v.id : v.native_id;
                if (!id.empty()) ids.push_back(id);
                break;
            }
        }
    }
    if (ids.empty()) {
        toast(std::string("当前列表里没有") + music::source_name(want) + "音源的歌（先搜一次，再点药丸换源）", 6000);
        return;
    }
    g_sync_site.store(site);
    g_sync_index.store(idx);
    toast("正在同步 " + std::to_string(ids.size()) + " 首到「" + name + "」…");

    background::start([site, cookie, uid, pid, ids, name] {
        std::string err;
        const bool ok = site == 1 ? qq::add_to_playlist(cookie, uid, pid, ids, err)
                                  : ne::add_tracks(cookie, pid, ids, err);
        g_sync_site.store(-1);
        g_sync_index.store(-1);
        if (g_win) {
            toast(ok ? ("已同步 " + std::to_string(ids.size()) + " 首到「" + name + "」")
                     : ("同步失败：" + err),
                  ok ? 5000 : 9000);
            g_win->wake();
        }
    });
}

// ---------------- 网易云音乐（和 QQ 那套对称） ----------------

static void ne_snapshot() {
    static unsigned revision = 0;
    std::lock_guard<std::mutex> lk(g_ne.m);
    g_ne_snap.logged = g_ne.logged;
    g_ne_snap.loading = g_ne.loading;
    g_ne_snap.nickname = g_ne.nickname;
    g_ne_snap.err = g_ne.err;
    g_ne_snap.uid = g_ne.uid;
    if (revision != g_ne.playlists_revision) {
        g_ne_snap.playlists = g_ne.playlists; revision = g_ne.playlists_revision;
    }
}

void ne_refresh() {
    std::string cookie, uid;
    {
        std::lock_guard<std::mutex> lk(g_ne.m);
        g_ne.loading = true;
        g_ne.err.clear();
        cookie = g_ne.cookie;
        uid = g_ne.uid;
    }
    if (g_win) g_win->wake();
    background::start([cookie, uid] {
        ne::Profile p = ne::fetch_profile(cookie, uid);
        if (g_shutdown.load()) return;
        std::lock_guard<std::mutex> lk(g_ne.m);
        if (g_ne.cookie != cookie || g_ne.uid != uid) return;
        g_ne.loading = false;
        if (!p.ok()) {
            g_ne.err = p.error;
        } else {
            g_ne.nickname = p.nickname;
            g_ne.uid = p.uid;
            g_ne.playlists = std::move(p.playlists);
            ++g_ne.playlists_revision;
            g_ne.err.clear();
        }
        if (g_win) g_win->wake();
    });
}

void ne_open_playlist(int idx) {
    if (idx < 0 || idx >= int(g_ne_snap.playlists.size())) return;
    const auto& p = g_ne_snap.playlists[size_t(idx)];
    library::Playlist playlist;
    playlist.name = p.name; playlist.cover = p.cover; playlist.remote_id = p.id;
    playlist.source = music::Source::Netease;
    playlist_open_remote(std::move(playlist));
}

// ---- 队列操作（全局函数：后台线程与 UI 都会调）----
void queue_remove(int idx) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (idx < 0 || idx >= int(g_as.tracks.size())) return;
        const auto uid = track_uid(g_as.tracks[size_t(idx)]);
        g_play_next.erase(std::remove(g_play_next.begin(), g_play_next.end(), uid), g_play_next.end());
        g_as.tracks.erase(g_as.tracks.begin() + idx);
        if (g_as.playing >= idx) g_as.playing--; // 当前曲目被移除时，自然结束继续原来的后一首。
        ++g_as.tracks_version;
    }
    queue_mark_dirty();
}

void queue_clear() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_as.tracks.clear();
        g_as.playing = -1;
        g_play_next.clear();
        g_shuffle_remaining.clear();
        g_shuffle_history.clear(); g_shuffle_cursor = -1;
        ++g_as.tracks_version;
    }
    queue_mark_dirty();
}

// 加进队列末尾：搜索结果的「＋」走这里。同一首（源 + id 相同）不重复加。
bool queue_push(const music::Track& t) {
    if (t.variants.empty()) return false;
    bool dup = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (const auto& x : g_as.tracks) {
            for (const auto& a : x.variants) {
                for (const auto& b : t.variants) {
                    if (music::same_variant(a, b)) {
                        dup = true;
                        break;
                    }
                }
                if (dup) break;
            }
            if (dup) break;
        }
        if (!dup) {
            g_as.tracks.push_back(t);
            ++g_as.tracks_version;
        }
    }
    if (!dup) queue_mark_dirty();
    return !dup;
}

void queue_play_next(const music::Track& track) {
    if (track.variants.empty()) return;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto same_song = [&](const music::Track& item) {
            for (const auto& a : item.variants) for (const auto& b : track.variants)
                if (music::same_variant(a, b)) return true;
            return false;
        };
        auto found = std::find_if(g_as.tracks.begin(), g_as.tracks.end(), same_song);
        if (found != g_as.tracks.end() && track_uid(*found) == g_as.now_uid) {
            toast("这首歌正在播放"); return;
        }
        music::Track next = found == g_as.tracks.end() ? track : *found;
        if (found != g_as.tracks.end()) {
            const int removed = int(found - g_as.tracks.begin());
            g_as.tracks.erase(found);
            if (g_as.playing >= removed) --g_as.playing;
        }
        int current = g_as.playing;
        for (int i = 0; i < int(g_as.tracks.size()); ++i)
            if (track_uid(g_as.tracks[size_t(i)]) == g_as.now_uid) { current = i; break; }
        const int insertion = std::clamp(current + 1, 0, int(g_as.tracks.size()));
        const auto uid = track_uid(next);
        g_as.tracks.insert(g_as.tracks.begin() + insertion, std::move(next));
        g_as.playing = current;
        g_play_next.erase(std::remove(g_play_next.begin(), g_play_next.end(), uid), g_play_next.end());
        g_play_next.push_front(uid);
        ++g_as.tracks_version;
    }
    queue_mark_dirty();
    toast("下一首播放：" + track.title);
}

// =============================================================================
//  发现页（热搜 + 热门歌单）
// =============================================================================
// 发现页使用网易云 / QQ 的热搜与热门歌单。
struct DiscState {
    std::mutex m;
    bool loading = false, more = true;
    int site = 0, request = 0, version = 0, page = 0;
    std::string query, err;
    std::vector<disc::Card> cards;
    disc::Heats heats;
};
static DiscState g_disc, g_playlist_search;
static DiscSnap g_disc_snap, g_playlist_search_snap;

static void request_cards(DiscState& state, int site, std::string query, bool append, bool with_heats) {
    if (site < 0 || site >= music::kDiscoveryCount) return;
    int request, page;
    {
        std::lock_guard<std::mutex> lock(state.m);
        const bool same = state.site == site && state.query == query;
        if (same && (state.loading || (append && !state.more))) return;
        if (!same) { state.cards.clear(); state.heats = {}; }
        page = append && same ? state.page : 0;
        state.loading = true; state.site = site; state.query = query; state.err.clear();
        request = ++state.request; ++state.version;
    }
    auto* owner = &state;
    const auto settings = g_ui.settings;
    background::start([owner, site, query, append, with_heats, request, page, settings] {
        prov::Host::instance().set_credentials(settings.netease_cookie, settings.qq_cookie);
        net::CancelScope cancellation([owner, request] {
            if (g_shutdown.load()) return true;
            std::lock_guard<std::mutex> lock(owner->m); return owner->request != request;
        });
        const auto platform = music::kDiscoveryPlatforms[site];
        if (with_heats && !append) {
            auto h = disc::heats(platform);
            { std::lock_guard<std::mutex> lock(owner->m);
              if (owner->request != request || g_shutdown.load()) return;
              if (h.err.empty() || owner->heats.words.empty()) owner->heats = std::move(h);
              else owner->heats.err = std::move(h.err);
              ++owner->version; }
            if (g_win) g_win->wake();
        }
        std::string error;
        auto got = disc::playlists(platform, page, 30, query, error);
        if (g_shutdown.load()) return;
        { std::lock_guard<std::mutex> lock(owner->m);
          if (owner->request != request) return;
          if (error.empty()) {
              if (!append) owner->cards.clear();
              size_t added = 0;
              for (auto& card : got.cards) {
                  const bool duplicate = std::any_of(owner->cards.begin(), owner->cards.end(), [&](const auto& old) { return old.id == card.id && old.source == card.source; });
                  if (!duplicate) { owner->cards.push_back(std::move(card)); ++added; }
              }
              owner->page = page + 1; owner->more = got.more && added != 0;
          }
          owner->err = error; owner->loading = false; ++owner->version;
        }
        if (g_win) g_win->wake();
    });
}
void disc_refresh(int site) { request_cards(g_disc, site, {}, false, true); }
void disc_more() { request_cards(g_disc, g_ui.disc_site, {}, true, true); }
void search_playlists(bool append) {
    auto query = music::text::trim(g_ui.query);
    int site = g_ui.playlist_search_site;
    if (append) { std::lock_guard<std::mutex> lock(g_playlist_search.m); query = g_playlist_search.query; site = g_playlist_search.site; }
    if (query.empty()) { toast("请输入歌单关键词"); return; }
    request_cards(g_playlist_search, site, query, append, false);
}
static void disc_snapshot() {
    auto copy = [](DiscState& state, DiscSnap& snap) {
        std::lock_guard<std::mutex> lock(state.m);
        if (snap.version == state.version) return;
        snap.version = state.version; snap.site = state.site; snap.query = state.query;
        snap.loading = state.loading; snap.more = state.more;
        snap.cards = state.cards; snap.heats = state.heats; snap.err = state.err;
    };
    copy(g_disc, g_disc_snap); copy(g_playlist_search, g_playlist_search_snap);
}

static void queue_mark_dirty();

// 打开发现页里的歌单：热门歌单 / 官方榜都是公开歌单，不依赖登录态
void open_playlist_card(const disc::Card& p) {
    library::Playlist playlist;
    playlist.name = p.name; playlist.cover = p.cover; playlist.remote_id = p.id; playlist.source = p.source;
    playlist_open_remote(std::move(playlist));
}
void disc_open(int idx) {
    if (idx >= 0 && idx < int(g_disc_snap.cards.size())) open_playlist_card(g_disc_snap.cards[size_t(idx)]);
}

// ---- 落雪音源（界面用） ----
enum class LxTask { None, Import, Fetch, Reload };
static std::atomic<LxTask> g_lx_task{LxTask::None};
static void lx_check_sources(bool force = false) {
    background::start([force] {
        std::vector<lx::Engine::Check> cases;
        for (const auto source : {music::Source::Netease, music::Source::QQ}) {
            music::Variant v; v.source = source;
            if (source == music::Source::Netease) {
                v.id = "347230"; v.title = "海阔天空"; v.artist = "Beyond"; v.duration_ms = 326000;
            } else {
                v.id = "0039MnYb0qxYhV"; v.title = "晴天"; v.artist = "周杰伦"; v.duration_ms = 269000;
            }
            const auto* key = music::lx_platform_key(source);
            cases.push_back({key, music::lx_music_info(v, key)});
        }
        lx::Engine::instance().check_sources(cases, force, [] { return g_shutdown.load(); }, [] { if (g_win) g_win->wake(); });
    });
}
static void lx_source_task(LxTask kind, std::function<void()> action) {
    if (g_shutdown.load()) return;
    LxTask expected = LxTask::None;
    if (!g_lx_task.compare_exchange_strong(expected, kind)) { toast("音源任务正在进行，请稍候"); return; }
    background::start([action = std::move(action)] {
        struct Done {
            ~Done() { g_lx_task.store(LxTask::None); if (g_win) g_win->wake(); }
        } done;
        try { action(); lx_check_sources(); }
        catch (const std::exception& error) { const auto message = std::string("音源操作失败：") + error.what(); post_ui([message] { toast(message, 6000); }); }
        catch (...) { post_ui([] { toast("音源操作失败，请重试", 6000); }); }
    });
}
struct LxSourceView {
    std::string name, version, error, file;   // file 是磁盘上的完整路径，删除时用它
    bool ok = false;
    bool removed = false;
    int health_rank = 1;
    int latency_ms = 300001;
    std::string health;
};

// 音源引擎是否正在后台加载（界面上显示"加载中"，而不是等锁卡住）
bool lx_sources_loading() { return lx::Engine::instance().loading(); }

const std::vector<LxSourceView>& lx_source_status() {
    static std::vector<LxSourceView> cached;
    static unsigned cached_revision = ~0u;
    const auto revision = lx::Engine::instance().revision();
    if (revision == cached_revision) return cached;
    std::vector<LxSourceView> out;
    const auto errors = lx::Engine::instance().runtime_errors();
    const auto checks = lx::Engine::instance().health();
    const auto timings = lx::Engine::instance().timings();
    for (const auto& e : lx::Engine::instance().entries()) {
        LxSourceView v;
        v.name = e.first; v.file = e.second; v.ok = true;
        auto key = platform::fs::u8path(e.second).filename().u8string();
#ifdef _WIN32
        key = music::text::lower_ascii(key);
#endif
        const auto error = errors.find(key);
        if (error != errors.end()) {
            for (const auto& item : error->second) {
                if (!v.error.empty()) v.error += "\n";
                std::string name = item.first;
                for (const auto& p : music::kPlatformInfo) if (p.lx_key && item.first == p.lx_key) { name = p.name; break; }
                v.error += name + " 最近一次取流失败（不代表整个音源失效）：" + item.second;
            }
        }
        const auto health = checks.find(key);
        int passed = 0, failed = 0, measured = 0, total_latency = 0;
        for (const auto& platform : {std::string("tx"), std::string("wy")}) {
            const lx::Engine::Health* h = nullptr;
            if (health != checks.end()) {
                const auto found = health->second.find(platform);
                if (found != health->second.end()) h = &found->second;
            }
            if (!v.health.empty()) v.health += " · ";
            v.health += platform == "tx" ? "QQ " : "网易云 ";
            v.health += !h || h->state == 0 ? "待检测" : h->state == 1 ? "检测中" : h->state == 2 ? "样本通过" : h->state == 4 ? "不支持" : h->state == 5 ? "检测未完成" : "样本失败";
            if (h && h->state == 2) {
                ++passed;
                if (h->load_ms > 0) {
                    v.health += " " + std::to_string(h->load_ms) + "ms";
                    total_latency += h->load_ms; ++measured;
                }
            }
            if (h && !h->reason.empty()) {
                if (!v.error.empty()) v.error += "\n";
                v.error += (platform == "tx" ? "QQ 样本：" : "网易云样本：") + h->reason;
                if (h->state == 2) v.error += "；音质 " + h->quality + "；取址 " + std::to_string(h->resolve_ms) + "ms；取址与样本加载共 " + std::to_string(h->load_ms) + "ms";
            }
            if (h && h->state == 3) ++failed;
        }
        const auto metrics = timings.find(key);
        if (metrics != timings.end()) for (const auto& platform : metrics->second) for (const auto& quality : platform.second) {
            const auto& value = quality.second;
            if (!value.samples && !value.failures) continue;
            std::string name = platform.first;
            for (const auto& p : music::kPlatformInfo) if (p.lx_key && platform.first == p.lx_key) { name = p.name; break; }
            if (!v.error.empty()) v.error += "\n";
            v.error += name + " · " + quality.first + "：平滑取址 " + std::to_string(value.resolve_ms) + "ms";
            if (value.load_ms) v.error += "，样本加载 " + std::to_string(value.load_ms) + "ms";
            if (value.ready_ms) v.error += "，实际首段就绪 " + std::to_string(value.ready_ms) + "ms";
            v.error += "，有效测速 " + std::to_string(value.samples) + " 次";
            if (value.failures) v.error += "，近期失败 " + std::to_string(value.failures) + " 次";
        }
        v.health_rank = passed ? -passed : failed ? 2 : 1;
        if (measured) v.latency_ms = total_latency / measured;
        out.push_back(std::move(v));
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return std::tie(a.health_rank, a.latency_ms, a.file) < std::tie(b.health_rank, b.latency_ms, b.file);
    });
    const auto dir = lx::default_dir();
    for (const auto& f : lx::Engine::instance().failures()) {
        LxSourceView v;
        v.file = platform::join(dir, f.first);
        v.name = f.first;   // 失败时引擎只给文件名
        if (v.name.size() > 3 && v.name.compare(v.name.size() - 3, 3, ".js") == 0)
            v.name.erase(v.name.size() - 3);
        v.error = f.second;
        v.ok = false;
        out.push_back(std::move(v));
    }
    for (const auto& file : lx::Engine::instance().removed_files()) {
        LxSourceView v; v.name = file; v.file = platform::join(dir, file); v.removed = true;
        v.error = "已从应用移除 · 源文件保留"; out.push_back(std::move(v));
    }
    cached = std::move(out); cached_revision = revision;
    return cached;
}

void lx_reload_sources() {
    lx_source_task(LxTask::Reload, [] {
        const auto dir = lx::default_dir();
        const int n = lx::Engine::instance().load_dir(dir);
        post_ui([n] { toast(n < 0 ? lx::Engine::instance().preferences_error() :
            n > 0 ? ("已加载 " + std::to_string(n) + " 个音源") : "没有可用的音源脚本"); });
    });
}

// 从应用中移除；磁盘文件保留，重新导入该文件即可恢复。
void lx_remove_source(const std::string& file) {
    const std::string name = platform::fs::u8path(file).filename().u8string();
    background::start([file, name] {
        std::string error;
        const bool removed = lx::Engine::instance().remove_source(file, error);
        post_ui([removed, error, name] { toast(removed ? "已从应用移除 " + name + "，源文件保留，可重新导入" : error, 5000); });
    });
}
void lx_restore_source(const std::string& file) {
    lx_source_task(LxTask::Import, [file] {
        const auto name = platform::fs::u8path(file).filename().u8string();
        const int n = lx::Engine::instance().load_files(lx::default_dir(), {name});
        post_ui([n] { toast(n > 0 ? "音源已恢复，正在后台静默检测" : "恢复失败，请查看初始化原因"); });
    });
}

// 下载与脚本初始化分阶段显示；失败不覆盖本地文件，主动获取恢复成功下载的音源。
std::string g_lx_fetch_summary;
std::string g_lx_fetch_progress; // 仅在 UI 线程读写。
void lx_fetch_online_sources() {
    lx_source_task(LxTask::Fetch, [] {
        post_ui([] { g_lx_fetch_progress = "正在选择下载通路…"; });
        static const char* kSlugs[] = {"changqing", "feichangdao", "fish", "flower", "grass",
            "helloworld", "huanyin", "huibq", "hyw", "ikun", "jiexiang", "juhe", "lx", "qdy",
            "qingliang", "sixyin", "suyin", "xinghai", "xinglan", "yuningxi"};
        constexpr int kCount = int(sizeof(kSlugs) / sizeof(kSlugs[0]));
        constexpr int kProbeIndex = 7;
        static const char* kBases[] = {
            "https://raw.githubusercontent.com/",
            "https://gh-proxy.org/raw.githubusercontent.com/",
            "https://ghproxy.net/raw.githubusercontent.com/",
        };
        constexpr int kBaseCount = int(sizeof(kBases) / sizeof(kBases[0]));
        const auto dir = lx::default_dir();
        if (dir.empty()) throw std::runtime_error("无法访问用户音源目录");
        platform::fs::create_directories(platform::fs::u8path(dir));
        const auto started = std::chrono::steady_clock::now();
        const auto deadline = started + std::chrono::seconds(35);
        const auto valid = [](const net::Response& r) {
            const auto& body = r.body;
            const auto begin = body.find_first_not_of(" \t\r\n");
            if (!r.ok() || r.status != 200 || body.size() < 200 || begin == std::string::npos || body[begin] == '<') return false;
            return body.find("globalThis.lx") != std::string::npos ||
                body.find("lx.request") != std::string::npos || body.find("lx.on") != std::string::npos ||
                body.find("EVENT_NAMES") != std::string::npos || body.find("musicUrl") != std::string::npos;
        };
        const auto url = [&](int base, int file) {
            return std::string(kBases[base]) + "pdone/lx-music-source/main/" + kSlugs[file] + "/latest.js";
        };
        const auto parallel = [](int count, const auto& work) {
            std::vector<std::thread> threads;
            struct Join {
                std::vector<std::thread>& threads;
                ~Join() { for (auto& thread : threads) if (thread.joinable()) thread.join(); }
            } join{threads};
            for (int i = 0; i < count; ++i) threads.emplace_back(work, i);
        };

        // 三条通路同时获取同一个小脚本，首个有效响应获胜，其余请求取消；复用探测内容。
        std::atomic<int> winner{-1};
        std::vector<net::Response> probes(size_t(kBaseCount), net::Response{});
        parallel(kBaseCount, [&](int base) {
            try {
                net::CancelScope network([&] { return winner.load() >= 0 || g_shutdown.load(); });
                auto response = net::get(url(base, kProbeIndex), {"User-Agent: Aino-Player"}, 5000);
                if (!valid(response)) return;
                probes[size_t(base)] = std::move(response);
                int expected = -1;
                winner.compare_exchange_strong(expected, base);
            } catch (...) { /* 单条通路失败不影响其余通路。 */ }
        });
        if (g_shutdown.load()) return;
        if (winner.load() < 0) {
            post_ui([] {
                g_lx_fetch_progress.clear();
                g_lx_fetch_summary = "无法连接音源仓库，已有脚本已保留，请检查网络后重试";
                toast(g_lx_fetch_summary, 6000);
            });
            return;
        }
        std::atomic<int> preferred{winner.load()};
        std::set<std::string> loaded;
        for (const auto& entry : lx::Engine::instance().entries())
            loaded.insert(platform::fs::u8path(entry.second).filename().u8string());
        std::atomic<int> next{0};
        int completed = 0, changed = 0, unchanged = 0, failed = 0;
        std::vector<std::string> refresh;
        std::string failed_names;
        std::mutex result_mutex;
        const auto fetch_one = [&](int file, bool& modified) {
            net::CancelScope network([&] {
                return g_shutdown.load() || std::chrono::steady_clock::now() >= deadline;
            });
            std::string body;
            if (file == kProbeIndex) body = std::move(probes[size_t(winner.load())].body);
            else {
                const int first = preferred.load();
                for (int attempt = 0; attempt < kBaseCount && !net::cancelled(); ++attempt) {
                    const int base = (first + attempt) % kBaseCount;
                    auto response = net::get(url(base, file), {"User-Agent: Aino-Player"}, 6000);
                    if (!valid(response)) continue;
                    body = std::move(response.body);
                    preferred.store(base);
                    break;
                }
            }
            if (body.empty() || net::cancelled()) return false;
            const auto dest = platform::join(dir, std::string("lx_") + kSlugs[file] + ".js");
            if (platform::file_size(dest) == static_cast<long long>(body.size())) {
                std::ifstream input(platform::fs::u8path(dest), std::ios::binary);
                const std::string old((std::istreambuf_iterator<char>(input)), {});
                if (input.is_open() && !input.bad() && old == body) return true;
            }
            const auto temporary = dest + ".download";
            auto* output = platform::fopen(temporary, "wb");
            if (!output) return false;
            const bool written = std::fwrite(body.data(), 1, body.size(), output) == body.size() && !std::ferror(output);
            const bool closed = std::fclose(output) == 0;
            const bool saved = written && closed && platform::replace_file(temporary, dest);
            if (!saved) { platform::remove(temporary); return false; }
            modified = true;
            return true;
        };
        const auto worker = [&](int) {
            for (;;) {
                const int file = next.fetch_add(1);
                if (file >= kCount || g_shutdown.load()) return;
                bool modified = false, ok = false;
                try { ok = fetch_one(file, modified); } catch (...) { /* 保留原文件，纳入失败汇总。 */ }
                const std::string name = std::string("lx_") + kSlugs[file] + ".js";
                std::lock_guard<std::mutex> lock(result_mutex);
                if (ok) {
                    if (modified) ++changed; else ++unchanged;
                    if (modified || !loaded.count(name)) refresh.push_back(name);
                } else {
                    ++failed;
                    if (failed_names.size() < 120) failed_names += (failed_names.empty() ? "" : " ") + std::string(kSlugs[file]);
                }
                const auto progress = "正在下载音源 " + std::to_string(++completed) + "/" + std::to_string(kCount) +
                    " · 更新 " + std::to_string(changed) + " · 未变 " + std::to_string(unchanged) + " · 失败 " + std::to_string(failed);
                post_ui([progress] { g_lx_fetch_progress = progress; });
            }
        };
        parallel(6, worker);
        if (g_shutdown.load()) return;
        const auto count = refresh.size();
        post_ui([count] { g_lx_fetch_progress = "下载完成，正在加载脚本（" + std::to_string(count) + " 个）…"; });
        // 主动在线获取与导入含义一致：恢复这次成功下载的文件。普通重载仍尊重移除记录。
        const int refreshed = refresh.empty() ? 0 : lx::Engine::instance().load_files(dir, refresh);
        const int available = int(lx::Engine::instance().entries().size());
        const auto failures = lx::Engine::instance().failures();
        int init_failed = 0;
        for (const auto& failure : failures)
            if (std::find(refresh.begin(), refresh.end(), failure.first) != refresh.end()) ++init_failed;
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
        post_ui([changed, unchanged, failed, failed_names, refreshed, available, init_failed, seconds] {
            g_lx_fetch_progress.clear();
            g_lx_fetch_summary = "在线获取：更新 " + std::to_string(changed) + " · 未变 " + std::to_string(unchanged);
            if (failed) g_lx_fetch_summary += " · 失败 " + std::to_string(failed) + "（" + failed_names + "），原文件保留";
            if (refreshed < 0) g_lx_fetch_summary += " · " + lx::Engine::instance().preferences_error();
            g_lx_fetch_summary += "；已加载 " + std::to_string(available) + " 个";
            if (init_failed) g_lx_fetch_summary += " · 初始化失败 " + std::to_string(init_failed) + "（查看原因）";
            g_lx_fetch_summary += " · " + std::to_string(seconds) + " 秒";
            toast(g_lx_fetch_summary, 9000);
        });
    });
}

void lx_import_source() {
    void* owner = g_host.native_window ? g_host.native_window() : nullptr;
    lx_source_task(LxTask::Import, [owner] {
        std::string error;
        // 多选：一次可以框选任意多个 .js，不用一个一个点
        const auto paths = platform::choose_js_files(owner, g_shutdown, error);
        if (!error.empty()) { post_ui([error] { toast(error); }); return; }
        if (paths.empty()) return;
        const auto dir = lx::default_dir();
        if (dir.empty()) throw std::runtime_error("无法访问用户音源目录");
        if (!platform::fs::exists(dir)) platform::fs::create_directories(dir);
        int copied = 0;
        std::vector<std::string> imported_files;
        std::string names;
        for (const auto& path : paths) {
            const auto name = platform::fs::u8path(path).filename().u8string();
            if (!lx::script_file(platform::fs::u8path(name))) continue;
            const auto dest = platform::join(dir, name);
            std::error_code ec;
            const bool already_in_directory = platform::fs::equivalent(
                platform::fs::u8path(path), platform::fs::u8path(dest), ec);
            ec.clear();
            if (!already_in_directory)
                platform::fs::copy_file(platform::fs::u8path(path), platform::fs::u8path(dest),
                                        platform::fs::copy_options::overwrite_existing, ec);
            if (ec) continue;
            ++copied;
            imported_files.push_back(name);
            if (names.size() < 60) names += (names.empty() ? "" : " ") + name;
        }
        // 只初始化本次导入的脚本，避免所有旧源重复联网。
        const int n = copied > 0 ? lx::Engine::instance().load_files(dir, std::move(imported_files)) : 0;
        post_ui([copied, names, n] {
            if (copied == 0) { toast("导入失败"); return; }
            if (n < 0) { toast(lx::Engine::instance().preferences_error()); return; }
            std::string msg = "已导入 " + std::to_string(copied) + " 个";
            if (copied > 1 || names.size() < 40) msg += "（" + names + "）";
            msg += "，其中可用 " + std::to_string(n) + " 个";
            toast(msg, 6000);
        });
    });
}

// =============================================================================
//  播放队列持久化
// =============================================================================
// 队列独立持久化；浏览歌单和执行在线搜索均不修改它，点播或加入队列才修改。
// 文件格式：每行一首，制表符分隔，字段全转义（标题里真出现过制表符）。
static std::string queue_path() {
    const auto dir = platform::config_dir();
    return dir.empty() ? std::string() : platform::join(dir, "queue.txt");
}

static bool g_queue_writable = true; // 损坏的旧文件在用户修改队列前保留。
static long long g_resume_on_start = 0, g_pending_seek = 0; // g_mu
static constexpr size_t kQueueFileLimit = 64u * 1024u * 1024u;

static bool queue_save() {
    static std::mutex file_mutex;
    std::lock_guard<std::mutex> file_lock(file_mutex);
    const std::string path = queue_path();
    if (path.empty()) return false;
    std::string current;
    long long position = 0;
    std::vector<music::Track> tracks;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_queue_writable) return false;
        current = g_as.now_uid;
        position = g_as.position_ms;
        tracks = g_as.tracks;
    }
    // JSON 编码和文件 I/O 都在播放锁外，避免长歌单阻挡界面和切歌。
    std::string encoded = "{\"version\":2,\"current\":" + library::quote(current) +
        ",\"position_ms\":" + std::to_string(position) + ",\"tracks\":[";
    bool first = true;
    for (const auto& track : tracks) {
        if (track.variants.empty() || track.variants.size() > 12) return false;
        if (!first) encoded += ',';
        first = false;
        encoded += library::encode_track(track);
        if (encoded.size() > kQueueFileLimit - 2) return false;
    }
    encoded += "]}";
    const auto temporary = path + ".tmp";
    std::FILE* f = platform::fopen(temporary, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(encoded.data(), 1, encoded.size(), f) == encoded.size() && !std::ferror(f);
    const bool closed = std::fclose(f) == 0;
    const bool saved = ok && closed && platform::replace_file(temporary, path);
    if (!saved) platform::remove(temporary);
    return saved;
}

// 队列改了（搜索覆盖 / 打开歌单 / 加队列 / 删除）就标脏，主循环里延时落盘，
// 避免一次批量操作写几十遍文件。
static std::atomic<bool> g_queue_dirty{false};
static std::atomic<int> g_queue_gen{0};
static std::mutex g_queue_writer_mutex;
static std::condition_variable g_queue_writer_changed;
static bool g_queue_writer_started = false; // g_queue_writer_mutex
// 单写入任务合并连续编辑；静止窗口不依赖新帧，退出时可立即唤醒。
static void queue_mark_dirty() {
    if (g_shutdown.load()) return;
    { std::lock_guard<std::mutex> lock(g_mu); g_queue_writable = true; }
    std::lock_guard<std::mutex> lock(g_queue_writer_mutex);
    ++g_queue_gen;
    g_queue_dirty.store(true);
    g_queue_writer_changed.notify_one();
    if (g_queue_writer_started) return;
    g_queue_writer_started = true;
    background::start([] {
        std::unique_lock<std::mutex> lock(g_queue_writer_mutex);
        while (!g_shutdown.load()) {
            g_queue_writer_changed.wait(lock, [] { return g_shutdown.load() || g_queue_dirty.load(); });
            if (g_shutdown.load()) break;
            const int revision = g_queue_gen.load();
            if (g_queue_writer_changed.wait_for(lock, std::chrono::milliseconds(1200), [revision] {
                return g_shutdown.load() || revision != g_queue_gen.load();
            })) continue;
            if (!g_queue_dirty.exchange(false)) continue;
            lock.unlock();
            try {
                if (!queue_save()) toast("播放队列保存失败，原文件已保留；请检查目录权限或队列是否超过 64 MiB", 7000);
            } catch (const std::exception& error) { background::report_error(error.what()); }
            catch (...) { background::report_error("Queue persistence failed"); }
            lock.lock();
        }
        g_queue_writer_started = false;
    });
}

static std::vector<std::string> split_tab(const std::string& line) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (true) {
        const size_t end = line.find('\t', pos);
        if (end == std::string::npos) {
            out.push_back(line.substr(pos));
            break;
        }
        out.push_back(line.substr(pos, end - pos));
        pos = end + 1;
    }
    return out;
}

static void queue_load() {
    const std::string path = queue_path();
    if (path.empty()) return;
    std::ifstream file(platform::fs::u8path(path), std::ios::binary);
    if (!file) return;
    const auto bytes = platform::file_size(path);
    auto corrupt = [] {
        { std::lock_guard<std::mutex> lock(g_mu); g_queue_writable = false; }
        toast("播放队列文件损坏或过大，已保留原文件", 7000);
    };
    if (bytes < 0 || bytes > static_cast<long long>(kQueueFileLimit)) { corrupt(); return; }
    const std::string text((std::istreambuf_iterator<char>(file)), {});
    std::vector<music::Track> tracks;
    std::string current;
    long long position = 0;
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin != std::string::npos && text[begin] == '{') {
        js::Value root;
        if (!js::parse(text, root) || root["version"].integer() != 2 || !root["tracks"].is_array()) { corrupt(); return; }
        for (const auto& row : root["tracks"].array) {
            music::Track track;
            if (!library::decode_track(row, track) || track.variants.size() != row["variants"].size()) { corrupt(); return; }
            tracks.push_back(std::move(track));
        }
        current = root["current"].str();
        position = std::clamp(root["position_ms"].integer(), 0LL, 7LL * 24 * 3600 * 1000);
    } else {
    if (text.rfind("# aino queue v1", 0) != 0) { corrupt(); return; }
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const auto f8 = split_tab(line);
        if (f8.size() < 8) { corrupt(); return; }
        music::Variant v;
        v.source = music::Source(std::atoi(f8[0].c_str()));
        v.id = f8[1];
        v.native_id = f8[2];
        if (v.id.empty() || int(v.source) < 0 || int(v.source) > int(music::Source::Local)) { corrupt(); return; }
        music::Track t;
        t.title = f8[3];
        t.artist = f8[4];
        t.album = f8[5];
        t.duration_ms = std::atoi(f8[6].c_str());
        t.cover_url = f8[7];
        v.title = t.title;
        v.artist = t.artist;
        v.album = t.album;
        v.duration_ms = t.duration_ms;
        v.cover_url = t.cover_url;
        t.variants.push_back(v);
        t.chosen = 0;
        tracks.push_back(std::move(t));
    }
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_as.tracks = std::move(tracks);
        g_queue_writable = true;
        g_as.playing = -1;
        g_as.ps = PlayState::Idle;
        g_as.now_uid.clear(); g_as.now_track = {};
        g_resume_on_start = 0; g_as.position_ms = 0;
        for (int i = 0; i < int(g_as.tracks.size()); ++i) if (track_uid(g_as.tracks[size_t(i)]) == current) {
            const auto& track = g_as.tracks[size_t(i)];
            g_as.playing = i; g_as.now_uid = current; g_as.now_track = track;
            g_as.now_title = track.title; g_as.now_artist = track.artist; g_as.now_cover = track.cover_url;
            g_as.now_source = int(track.current().source); g_as.now_quality = "待续播";
            g_as.position_ms = position; g_resume_on_start = position;
            g_ui.pos_ms = float(position); g_ui.dur_ms = float(track.duration_ms);
            break;
        }
        g_as.reports.clear();
        ++g_as.results_version;
        g_as.elapsed_ms = 0;
        g_as.searching = false;
        ++g_as.tracks_version;
    }
}

// 清掉上一首的缓存文件：每首歌一个 track-<gen>.mp3，不清理的话临时目录会一直涨
static void cleanup_temp(int keep_gen) {
    std::unique_lock<std::shared_mutex> cache_write(g_play_cache_mutex, std::try_to_lock);
    if (!cache_write.owns_lock()) return;
    std::error_code ec;
    for (platform::fs::directory_iterator it(platform::fs::u8path(g_temp_dir), ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().u8string();
        if (name.rfind("track-", 0) != 0 || name.rfind("track-" + std::to_string(keep_gen) + ".", 0) == 0) continue;
        platform::remove(it->path().u8string());
    }
}

static void start_play_attempt(int index, bool keep_failures) {
    music::Track track;
    music::Settings st;
    int gen = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (index < 0 || index >= int(g_as.tracks.size())) return;
        track = g_as.tracks[size_t(index)];
        if (track.variants.empty()) return;
        if (!keep_failures) g_failed_tracks.clear();
        st = g_ui.settings;
        for (int i = 0; i < music::kSourceCount; ++i) st.enable[i] = (g_ui.src_mask >> i) & 1u;
        gen = ++g_as.gen; g_play_generation.store(gen);
        g_pending_seek = g_as.now_uid == track_uid(track) ? g_resume_on_start : 0;
        g_resume_on_start = 0;
        g_as.position_ms = g_pending_seek;
        g_as.playing = index;
        g_as.ps = PlayState::Resolving;
        g_as.error.clear();
        g_as.buffer = 0.0f;
        g_as.now_uid = track_uid(track);
        if (g_ui.play_mode == PlayMode::Shuffle &&
            (g_shuffle_cursor < 0 || g_shuffle_history[size_t(g_shuffle_cursor)] != g_as.now_uid)) {
            g_shuffle_history.resize(size_t(g_shuffle_cursor + 1));
            if (g_shuffle_history.size() == 256) g_shuffle_history.erase(g_shuffle_history.begin());
            g_shuffle_history.push_back(g_as.now_uid);
            g_shuffle_cursor = int(g_shuffle_history.size()) - 1;
        }
        g_play_next.erase(std::remove(g_play_next.begin(), g_play_next.end(), g_as.now_uid), g_play_next.end());
        g_shuffle_remaining.erase(std::remove(g_shuffle_remaining.begin(), g_shuffle_remaining.end(), g_as.now_uid), g_shuffle_remaining.end());
        g_as.now_track = track;
        g_as.now_cover = track.cover_url;
        g_as.now_title = track.title;
        g_as.now_artist = track.artist;
        g_as.now_source = int(track.current().source);
        g_as.now_quality = "解析中";
        g_as.cached_bytes = 0;
        g_as.pending_path.clear(); g_pending_stream.reset(); g_active_stream_spec = {}; g_play_ready_paused = false;
    }
    g_ui.pos_ms = float(g_pending_seek);
    g_ui.dur_ms = float(track.duration_ms);
    g_player.close();  // 播放设备只由主线程操作。
    g_current_media = take_preloaded(track, st);
    cleanup_temp(gen);
    if (g_ui.lyrics_open) request_lyrics(track);
    if (g_current_media) {
        {
            std::lock_guard<std::mutex> lock(g_mu);
            const auto& resolved = g_current_media->resolved;
            g_as.now_lane = resolved.ok ? int(resolved.lane) : -1;
            g_as.now_engine = resolved.engine;
            g_as.now_origin = int(track.current().source);
            g_as.now_swapped = g_current_media->variant.source != track.current().source;
        }
        finish_play(g_win, g_current_media->variant, g_current_media->original, gen, false, g_current_media->playable);
        return;
    }
    background::start(do_play, g_win, track, st, gen, true);
}

void start_play(int index) { start_play_attempt(index, false); }

// 请求队列仅由 UI 线程持有；一次只扫描一批，标签读取始终留在后台。
struct LocalOpenRequest { std::vector<std::string> paths; unsigned serial; int play_generation; };
static std::deque<LocalOpenRequest> g_local_open_requests;
static unsigned g_local_open_serial = 0;
static void process_local_open() {
    if (g_local_open_requests.empty() || g_shutdown.load()) { g_ui.opening_local = false; return; }
    auto request = std::move(g_local_open_requests.front()); g_local_open_requests.pop_front();
    g_ui.opening_local = true;
    background::start([request = std::move(request)]() mutable {
        std::vector<music::Track> tracks;
        size_t skipped = 0, repeated = 0;
        bool limited = false;
        std::string error;
        std::unordered_set<std::string> seen;
        constexpr size_t limit = 10000;
        auto add = [&](const platform::fs::path& input, bool explicit_file) {
            if (tracks.size() >= limit) { limited = true; return; }
            std::error_code ec;
            auto file = platform::fs::canonical(input, ec);
            if (ec || !platform::fs::is_regular_file(file, ec) || ec) { ++skipped; return; }
            if (!library::local_audio_file(file)) { if (explicit_file) ++skipped; return; }
            if (!seen.insert(library::local_path_key(file)).second) { ++repeated; return; }
            if (platform::fs::file_size(file, ec) == 0 || ec) { ++skipped; return; }
            auto* check = platform::fopen(file.u8string(), "rb");
            if (!check) { ++skipped; return; }
            std::fclose(check);
            try { tracks.push_back(library::local_track(file)); }
            catch (const std::exception&) { ++skipped; }
        };
        for (const auto& value : request.paths) {
            if (g_shutdown.load() || limited) break;
            try {
                const auto decoded = platform::local_file_path(value);
                if (decoded.empty()) { ++skipped; continue; }
                const auto path = platform::fs::u8path(decoded);
                std::error_code ec;
                if (!platform::fs::is_directory(path, ec)) { add(path, true); continue; }
                // 每个文件夹内部按路径排序；跳过无权限目录及目录链接，避免循环遍历。
                std::vector<platform::fs::path> files;
                const auto options = platform::fs::directory_options::skip_permission_denied;
                for (platform::fs::recursive_directory_iterator it(path, options, ec), end; !ec && it != end; it.increment(ec)) {
                    if (g_shutdown.load()) return;
                    std::error_code entry_error;
                    if (!it->is_symlink(entry_error) && it->is_regular_file(entry_error) && !entry_error && library::local_audio_file(it->path())) {
                        files.push_back(it->path());
                        if (files.size() >= limit) { limited = true; break; }
                    }
                }
                if (ec) ++skipped;
                std::sort(files.begin(), files.end());
                for (const auto& file : files) { if (g_shutdown.load()) return; add(file, false); }
            } catch (const std::exception&) { ++skipped; error = "部分路径无法读取"; }
        }
        if (g_shutdown.load()) return;
        post_ui([serial = request.serial, generation = request.play_generation, tracks = std::move(tracks), skipped, repeated, limited, error]() mutable {
            int first = -1;
            size_t added = 0, duplicates = repeated;
            {
                std::lock_guard<std::mutex> lock(g_mu);
                std::unordered_map<std::string, int> existing;
                for (size_t i = 0; i < g_as.tracks.size(); ++i)
                    for (const auto& variant : g_as.tracks[i].variants)
                        if (variant.source == music::Source::Local) existing.emplace(library::local_path_key(platform::fs::u8path(variant.id)), int(i));
                for (auto& track : tracks) {
                    const auto key = library::local_path_key(platform::fs::u8path(track.current().id));
                    const auto found = existing.find(key);
                    int index = found == existing.end() ? int(g_as.tracks.size()) : found->second;
                    if (found == existing.end()) { existing.emplace(key, index); g_as.tracks.push_back(std::move(track)); ++added; }
                    else ++duplicates;
                    if (first < 0) first = index;
                }
                if (added) ++g_as.tracks_version;
            }
            if (added) queue_mark_dirty();
            if (first >= 0) {
                g_ui.queue_open = true; g_ui.lyrics_open = false;
                // 后台扫描不能覆盖用户刚切换的歌曲；多批拖入只让最新一批自动起播。
                if (serial == g_local_open_serial && generation == g_play_generation.load()) {
                    g_resume_on_start = 0; start_play(first);
                }
            }
            std::string message = first < 0 ? "没有可播放的本地音频" : "已加入 " + std::to_string(added) + " 首本地歌曲";
            if (duplicates) message += "，复用 / 忽略重复 " + std::to_string(duplicates) + " 首";
            if (skipped) message += "，跳过失效或不支持的文件 " + std::to_string(skipped) + " 项";
            if (limited) message += "（每批最多 10000 首，请分批打开）";
            if (!error.empty()) message += "，" + error;
            toast(message, 6000);
            process_local_open();
        });
    });
}
void open_local_files(std::vector<std::string> paths) {
    if (paths.empty() || g_shutdown.load()) return;
    if (g_local_open_requests.size() >= 8) { toast("正在处理多批本地文件，请稍候"); return; }
    if (paths.size() > 10000) { paths.resize(10000); toast("一次最多打开 10000 个文件或文件夹", 6000); }
    g_local_open_requests.push_back({std::move(paths), ++g_local_open_serial, g_play_generation.load()});
    if (!g_ui.opening_local) { toast("正在后台读取本地音乐…"); process_local_open(); }
}
void choose_local_audio() {
    if (g_folder_selecting || g_ui.choosing_local || g_shutdown.load()) return;
    g_folder_selecting = true; g_ui.choosing_local = true; g_folder_cancel.store(false);
    void* owner = g_host.native_window ? g_host.native_window() : nullptr;
    background::start([owner] {
        std::string error;
        auto paths = platform::choose_audio_files(owner, g_folder_cancel, error);
        post_ui([paths = std::move(paths), error]() mutable {
            g_folder_selecting = false; g_ui.choosing_local = false;
            if (!error.empty()) toast(error, 6000);
            if (!paths.empty()) open_local_files(std::move(paths));
        });
    });
}
void configure_default_player() {
    if (g_ui.setting_default_player || g_shutdown.load()) return;
    g_ui.setting_default_player = true;
    background::start([] {
        std::string error;
        const bool ok = platform::set_default_music_player(error, [] { return g_shutdown.load(); });
        post_ui([ok, error] {
            g_ui.setting_default_player = false;
#ifdef _WIN32
            toast(ok ? "已打开系统默认应用，请选择 Aino Player 并设置音乐文件关联" : error, 8000);
#else
            toast(ok ? "已将 Aino Player 设为默认音乐播放器" : error, 8000);
#endif
        });
    });
}

void toggle_pause() {
    PlayState ps;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        ps = g_as.ps;
        if (ps == PlayState::Playing) g_as.ps = PlayState::Paused;
        else if (ps == PlayState::Paused) g_as.ps = PlayState::Playing;
        else if (ps == PlayState::Resolving || ps == PlayState::Buffering || ps == PlayState::Ready) {
            g_play_ready_paused = !g_play_ready_paused;
            return;
        }
    }
    if (ps == PlayState::Playing) g_player.pause();
    else if (ps == PlayState::Paused) g_player.resume();
    else {
        int index;
        { std::lock_guard<std::mutex> lock(g_mu); index = g_as.playing < 0 ? 0 : g_as.playing; }
        start_play(index);
    }
}

// 只选择索引，不触碰音频设备；UI 和自然结束共用边界规则。
static int next_queue_index(int delta, bool automatic, bool consume = true, const std::set<std::string>* excluded = nullptr) {
    std::lock_guard<std::mutex> lock(g_mu);
    const int count = int(g_as.tracks.size());
    if (!count) return -1;
    const auto allowed = [&](int i) {
        return !g_as.tracks[size_t(i)].variants.empty() &&
            (!excluded || !excluded->count(track_uid(g_as.tracks[size_t(i)])));
    };
    int cur = -1;
    for (int i = 0; i < count; ++i)
        if (track_uid(g_as.tracks[size_t(i)]) == g_as.now_uid) { cur = i; break; }
    if (cur < 0) cur = g_as.playing;
    if (delta > 0) {
        for (auto it = g_play_next.begin(); it != g_play_next.end(); ++it) {
            const auto uid = *it;
            for (int i = 0; i < count; ++i)
                if (track_uid(g_as.tracks[size_t(i)]) == uid && allowed(i)) {
                    if (consume) g_play_next.erase(g_play_next.begin(), std::next(it));
                    return i;
                }
        }
        if (consume) g_play_next.clear();
    }
    if (automatic && g_ui.play_mode == PlayMode::RepeatOne && cur >= 0 && allowed(cur)) return cur;
    if (g_ui.play_mode == PlayMode::Shuffle) {
        if (delta < 0) {
            while (g_shuffle_cursor > 0) {
                const auto& uid = g_shuffle_history[size_t(--g_shuffle_cursor)];
                for (int i = 0; i < count; ++i) if (track_uid(g_as.tracks[size_t(i)]) == uid && allowed(i)) return i;
            }
            return cur >= 0 && allowed(cur) ? cur : -1;
        }
        int cursor = g_shuffle_cursor;
        while (cursor + 1 < int(g_shuffle_history.size())) {
            const auto& uid = g_shuffle_history[size_t(++cursor)];
            for (int i = 0; i < count; ++i) if (track_uid(g_as.tracks[size_t(i)]) == uid && allowed(i)) {
                if (consume) g_shuffle_cursor = cursor;
                return i;
            }
        }
        if (g_shuffle_version != g_as.tracks_version) {
            g_shuffle_remaining.clear(); g_shuffle_version = g_as.tracks_version;
        }
        if (excluded) g_shuffle_remaining.erase(std::remove_if(g_shuffle_remaining.begin(), g_shuffle_remaining.end(),
            [&](const auto& uid) { return excluded->count(uid) != 0; }), g_shuffle_remaining.end());
        if (g_shuffle_remaining.empty()) {
            for (int i = 0; i < count; ++i) if (i != cur && allowed(i)) g_shuffle_remaining.push_back(track_uid(g_as.tracks[size_t(i)]));
            std::shuffle(g_shuffle_remaining.begin(), g_shuffle_remaining.end(), g_shuffle_random);
        }
        if (g_shuffle_remaining.empty()) return cur >= 0 && allowed(cur) ? cur : -1;
        const auto uid = g_shuffle_remaining.back();
        if (consume) g_shuffle_remaining.pop_back();
        for (int i = 0; i < count; ++i) if (track_uid(g_as.tracks[size_t(i)]) == uid) return i;
    }
    int next = cur < 0 ? 0 : cur + delta;
    for (int tried = 0; tried < count; ++tried, next += delta) {
        if (next < 0) next = g_ui.play_mode == PlayMode::Sequential ? 0 : count - 1;
        if (next >= count) {
            if (g_ui.play_mode == PlayMode::Sequential) return -1;
            next = 0;
        }
        if (allowed(next)) return next;
    }
    return -1;
}
static void refresh_preload() {
    if (g_shutdown.load() || g_ui.sleep_after_track || g_snapshot.ps != PlayState::Playing) {
        if (g_snapshot.ps != PlayState::Paused) cancel_preload();
        return;
    }
    bool streaming;
    { std::lock_guard<std::mutex> lock(g_mu); streaming = !g_active_stream_spec.url.empty() || !g_active_stream_spec.local_path.empty(); }
    if (streaming &&
        (g_player.buffering() || g_ui.dur_ms <= 0 || g_ui.dur_ms - g_ui.pos_ms > 15000)) { cancel_preload(); return; }
    const int next = next_queue_index(1, true, false);
    music::Track track;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (next >= 0 && next < int(g_as.tracks.size())) track = g_as.tracks[size_t(next)];
        if (!track.variants.empty() && track_uid(track) == g_as.now_uid) track = {};
    }
    if (track.variants.empty()) { cancel_preload(); return; }
    auto settings = g_ui.settings;
    for (int i = 0; i < music::kSourceCount; ++i) settings.enable[i] = (g_ui.src_mask >> i) & 1u;
    request_preload(track, settings);
}
static void stop_playback() {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        ++g_as.gen; g_play_generation.store(g_as.gen); // 拒绝仍在解析/缓冲的旧任务，避免定时停止后重新起播。
        g_as.ps = PlayState::Idle; g_as.pending_path.clear(); g_as.error.clear();
        g_as.position_ms = 0; g_pending_seek = 0; g_resume_on_start = 0;
        g_pending_stream.reset(); g_active_stream_spec = {}; g_play_ready_paused = false;
    }
    g_failed_tracks.clear();
    g_player.close();
    cancel_preload(); g_current_media.reset();
    g_ui.pos_ms = 0;
    g_ui.seek_to = -1;
}
void play_step(int delta, bool automatic) {
    const int next = next_queue_index(delta, automatic);
    if (next >= 0) start_play(next);
    else if (automatic) stop_playback();
}
void set_play_mode(PlayMode mode) {
    if (int(mode) < 0 || int(mode) > 3) return;
    g_ui.play_mode = mode; g_ui.config_dirty = true;
    { std::lock_guard<std::mutex> lock(g_mu);
      g_shuffle_remaining.clear(); g_shuffle_version = -1; g_shuffle_history.clear(); g_shuffle_cursor = -1; }
}
void set_sleep_timer(int minutes, bool after_track) {
    if (minutes < 0 || minutes > 24 * 60) return;
    if (after_track) {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_as.ps != PlayState::Playing && g_as.ps != PlayState::Paused && g_as.ps != PlayState::Resolving &&
            g_as.ps != PlayState::Buffering && g_as.ps != PlayState::Ready) { toast("请先播放一首歌曲"); return; }
    }
    g_ui.sleep_after_track = after_track;
    g_ui.sleep_deadline = !after_track && minutes > 0 ? platform::ticks() + (unsigned long long)minutes * 60000 : 0;
    g_ui.sleep_remaining_seconds = after_track ? 0 : unsigned(minutes * 60);
    g_sleep_deadline.store(g_ui.sleep_deadline);
    toast(after_track ? "播完当前歌曲后停止" : minutes > 0 ? std::to_string(minutes) + " 分钟后停止播放" : "已取消定时停止");
}

void set_volume(float v) {
    g_ui.volume = std::min(100.0f, std::max(0.0f, v));
    g_player.set_volume(int(g_ui.volume));
    g_ui.config_dirty = true;
}

// 主线程每帧的"抄快照"
static void refresh_snapshot(Snapshot& snap) {
    std::lock_guard<std::mutex> lk(g_mu);
    snap.searching = g_as.searching;
    snap.elapsed_ms = g_as.elapsed_ms;
    snap.playing = g_as.playing;
    snap.ps = g_as.ps;
    snap.pending_paused = g_play_ready_paused;
    snap.now_uid = g_as.now_uid;
    snap.now_track = g_as.now_track;
    snap.now_cover = g_as.now_cover;
    snap.now_title = g_as.now_title;
    snap.now_artist = g_as.now_artist;
    snap.now_quality = g_as.now_quality;
    snap.now_lane = g_as.now_lane;
    snap.now_engine = g_as.now_engine;
    snap.now_origin = g_as.now_origin;
    snap.now_swapped = g_as.now_swapped;
    snap.now_short = g_as.now_short;
    snap.now_source = g_as.now_source;
    snap.error = g_as.error;
    snap.buffer = g_as.buffer;
    snap.cached_bytes = g_as.cached_bytes;
    snap.pending_path = g_as.pending_path;
    snap.pending_duration_ms = g_as.pending_duration_ms;
    snap.gen = g_as.gen;
    // 原地保留未变化的容器，版本相同时不再复制整个队列和搜索结果。
    if (snap.results_version != g_as.results_version) {
        snap.results = g_as.results;
        snap.reports = g_as.reports;
        snap.results_version = g_as.results_version;
    }
    if (snap.tracks_version != g_as.tracks_version) {
        snap.tracks = g_as.tracks;
        snap.tracks_version = g_as.tracks_version;
    }
}

// =============================================================================
//  五、界面
// =============================================================================

// 已发布的资料库只读；所有修改由一个写入任务合并、原子保存后发布。
static bool library_edit(std::function<void(library::Library&)> edit, std::function<void()> saved = {}, bool reset = false) {
    if ((!g_library_writable && !reset) || g_shutdown.load()) {
        toast("资料库加载失败，已保护原文件；请先修复或清除本地资料库", 7000); return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_library_write_mutex);
        g_library_edits.push_back({std::move(edit), std::move(saved)});
        if (g_library_writer) return true;
        g_library_writer = true;
        if (!g_library_durable) g_library_durable = g_library;
    }
    const auto path = library_path();
    background::start([path] {
        for (;;) {
            // 短时间内连续收藏只复制和落盘一批；退出仍会排空已接受的修改。
            platform::sleep_ms(80);
            std::deque<LibraryEdit> edits;
            std::shared_ptr<library::Library> base;
            {
                std::lock_guard<std::mutex> lock(g_library_write_mutex);
                if (g_library_edits.empty()) { g_library_writer = false; return; }
                edits.swap(g_library_edits); base = g_library_durable;
            }
            std::shared_ptr<library::Library> next;
            std::string error;
            bool ok = false;
            try {
                next = std::make_shared<library::Library>(*base);
                for (auto& action : edits) action.apply(*next);
                ok = next->save(path, error);
            } catch (...) { error = "资料库保存失败，原文件保留，请重试"; }
            if (!ok) {
                std::fprintf(stderr, "Library save failed: %s\n", error.c_str());
                post_ui([error] { toast(error, 8000); });
                continue;
            }
            { std::lock_guard<std::mutex> lock(g_library_write_mutex); g_library_durable = next; }
            post_ui([next, edits = std::move(edits)]() mutable {
                g_library = next; ++g_library_revision;
                for (auto& action : edits) if (action.saved) action.saved();
            });
        }
    });
    return true;
}
void favorite_toggle(const music::Track& track) {
    if (track.variants.empty()) return;
    auto added = std::make_shared<bool>(false);
    library_edit([track, added](library::Library& next) { *added = next.toggle(track); },
        [title = track.title, added] { toast((*added ? "已收藏：" : "已取消收藏：") + title, 2600); });
}
void batch_tracks(std::vector<music::Track> tracks, BatchAction action, std::string playlist_key) {
    if (tracks.empty()) { toast("请先选择歌曲"); return; }
    if (action == BatchAction::Favorite || action == BatchAction::RemovePlaylist) {
        library_edit([tracks = std::move(tracks), action, playlist_key](library::Library& next) {
            if (action == BatchAction::Favorite) {
                library::FavoriteIndex index(next.playlists.front().tracks);
                for (const auto& track : tracks) if (!track.variants.empty() && !index.contains(track)) {
                    next.playlists.front().tracks.push_back(track);
                    index.add(track);
                }
            } else {
                std::set<std::string> ids;
                for (const auto& track : tracks) ids.insert(track_uid(track));
                for (auto& playlist : next.playlists) if (playlist.key == playlist_key)
                    playlist.tracks.erase(std::remove_if(playlist.tracks.begin(), playlist.tracks.end(),
                        [&](const auto& track) { return ids.count(track_uid(track)) != 0; }), playlist.tracks.end());
            }
        }, [] { toast("批量修改已保存，原音乐文件保留"); });
    } else if (action == BatchAction::Download) {
        size_t online = 0;
        for (const auto& track : tracks) if (!track.variants.empty() && track.current().source != music::Source::Local) { start_download(track, g_ui.settings); ++online; }
        toast(online ? "已将所选在线歌曲加入下载任务，本地歌曲已跳过" : "所选歌曲已在本机，无需下载");
    } else {
        std::set<std::string> ids;
        for (const auto& track : tracks) ids.insert(track_uid(track));
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (action == BatchAction::Enqueue) {
                std::set<std::string> existing;
                for (const auto& track : g_as.tracks) for (const auto& v : track.variants) existing.insert(music::variant_uid(v));
                for (const auto& track : tracks) {
                    bool duplicate = track.variants.empty();
                    for (const auto& v : track.variants) duplicate |= existing.count(music::variant_uid(v)) != 0;
                    if (!duplicate) { g_as.tracks.push_back(track); for (const auto& v : track.variants) existing.insert(music::variant_uid(v)); }
                }
            } else {
                for (int i = int(g_as.tracks.size()) - 1; i >= 0; --i) if (ids.count(track_uid(g_as.tracks[size_t(i)]))) {
                    g_as.tracks.erase(g_as.tracks.begin() + i);
                    if (g_as.playing >= i) --g_as.playing;
                }
                g_play_next.erase(std::remove_if(g_play_next.begin(), g_play_next.end(), [&](const auto& id) { return ids.count(id) != 0; }), g_play_next.end());
            }
            ++g_as.tracks_version;
        }
        queue_mark_dirty(); toast(action == BatchAction::Enqueue ? "已将所选歌曲加入队列" : "已从队列移除所选歌曲");
    }
}
void cancel_all_downloads() {
    std::vector<std::string> ids;
    { std::lock_guard<std::mutex> lock(g_dlm); for (const auto& row : g_dls) if (!row.second.done && !row.second.failed) ids.push_back(row.first); }
    for (const auto& uid : ids) cancel_download(uid);
}
void retry_download(const std::string& uid) {
    music::Track track;
    { std::lock_guard<std::mutex> lock(g_dlm); auto it = g_dls.find(uid); if (it == g_dls.end() || !it->second.failed || g_download_cancel.count(uid)) { toast("请等待原任务结束后重试"); return; } track = it->second.track; }
    start_download(std::move(track), g_ui.settings);
}
void retry_failed_downloads() {
    std::vector<std::string> ids;
    { std::lock_guard<std::mutex> lock(g_dlm); for (const auto& row : g_dls) if (row.second.failed && row.second.err != "已取消" && !g_download_cancel.count(row.first)) ids.push_back(row.first); }
    for (const auto& uid : ids) retry_download(uid);
}
void clear_finished_downloads() {
    std::lock_guard<std::mutex> lock(g_dlm);
    for (auto it = g_dls.begin(); it != g_dls.end();) {
        if ((it->second.done || it->second.failed) && !it->second.publishing && !g_download_cancel.count(it->first)) it = g_dls.erase(it); else ++it;
    }
    ++g_download_revision;
}
void reveal_download(const std::string& uid) {
    std::string path;
    { std::lock_guard<std::mutex> lock(g_dlm); auto it = g_dls.find(uid); if (it != g_dls.end()) path = it->second.path; }
    if (path.empty() || !platform::exists(path)) { toast("下载文件已移动或删除"); return; }
    background::start([path] { platform::open_folder(platform::fs::u8path(path).parent_path().u8string()); });
}
void reveal_local_track(const music::Track& track) {
    if (track.variants.empty() || track.current().source != music::Source::Local) return;
    const auto path = track.current().id;
    background::start([path] {
        if (!platform::exists(path)) { toast("音乐文件已移动或删除：" + path, 6000); return; }
        if (!platform::open_folder(platform::fs::u8path(path).parent_path().u8string())) toast("无法打开音乐文件所在文件夹", 6000);
    });
}
void recent_play(size_t index) {
    if (index >= g_library->recent.size()) return;
    const auto track = g_library->recent[index];
    queue_push(track);
    int selected = -1;
    { std::lock_guard<std::mutex> lock(g_mu); for (size_t i = 0; i < g_as.tracks.size(); ++i) if (library::same_track(track, g_as.tracks[i])) { selected = int(i); break; } }
    if (selected >= 0) start_play(selected);
}
void recent_clear() { library_edit([](auto& next) { next.recent.clear(); }, [] { toast("已清空最近播放"); }); }
int lyric_offset() {
    const auto it = g_library->lyric_preferences.find(g_snapshot.now_uid);
    return it == g_library->lyric_preferences.end() ? 0 : it->second.offset_ms;
}
void change_lyric_offset(int delta, bool reset) {
    const auto uid = g_snapshot.now_uid; if (uid.empty()) return;
    library_edit([uid, delta, reset](auto& next) {
        if (!next.lyric_preferences.count(uid) && next.lyric_preferences.size() >= 4096) return;
        auto& pref = next.lyric_preferences[uid]; pref.offset_ms = reset ? 0 : std::clamp(pref.offset_ms + delta, -60000, 60000);
    });
}
void use_local_lyrics(bool reset) {
    const auto uid = g_snapshot.now_uid; if (uid.empty()) { toast("请先播放一首歌"); return; }
    const auto path = reset ? std::string() : music::text::trim(g_ui.lyric_file_path);
    if (!reset && (path.empty() || path.size() > 8192 || !platform::exists(path))) { toast("请输入有效的 UTF-8 LRC 文件完整路径"); return; }
    library_edit([uid, path](auto& next) {
        if (!next.lyric_preferences.count(uid) && next.lyric_preferences.size() >= 4096) return;
        next.lyric_preferences[uid].path = path;
    }, [uid] {
        { std::lock_guard<std::mutex> lock(g_lyr.m); if (g_lyr.for_uid == uid) { ++g_lyr.generation; g_lyr.doc.reset(); g_lyr.loading = false; } }
        if (g_snapshot.now_uid == uid) request_current_lyrics();
    });
}
void choose_local_lyrics() {
    if (g_lyric_selecting || g_snapshot.now_uid.empty()) return;
    g_lyric_selecting = true; g_lyric_cancel.store(false);
    const auto uid = g_snapshot.now_uid;
    auto* owner = g_host.native_window ? g_host.native_window() : nullptr;
    background::start([owner, uid] {
        std::string error;
        const auto path = platform::choose_music_folder(owner, g_lyric_cancel, error, false);
        post_ui([uid, path, error] {
            g_lyric_selecting = false;
            if (!error.empty()) toast(error);
            if (!path.empty() && g_snapshot.now_uid == uid) { g_ui.lyric_file_path = path; use_local_lyrics(false); }
        });
    });
}
void media_action(const platform::MediaEvent& event) {
    PlayState state;
    {
        std::lock_guard<std::mutex> lock(g_mu); state = g_as.ps;
        if (state == PlayState::Buffering || state == PlayState::Resolving || state == PlayState::Ready) {
            if (event.action == platform::MediaAction::Pause) { g_play_ready_paused = true; return; }
            if (event.action == platform::MediaAction::Play) { g_play_ready_paused = false; return; }
            if (event.action == platform::MediaAction::Toggle) { g_play_ready_paused = !g_play_ready_paused; return; }
        }
    }
    switch (event.action) {
        case platform::MediaAction::Toggle: toggle_pause(); break;
        case platform::MediaAction::Play:
            if (state == PlayState::Paused) toggle_pause();
            else if (state != PlayState::Playing && state != PlayState::Resolving && state != PlayState::Buffering) {
                if (g_snapshot.playing >= 0) start_play(g_snapshot.playing); else if (!g_snapshot.tracks.empty()) start_play(0);
            }
            break;
        case platform::MediaAction::Pause: if (state == PlayState::Playing) toggle_pause(); break;
        case platform::MediaAction::Stop: stop_playback(); break;
        case platform::MediaAction::Next: play_step(1); break;
        case platform::MediaAction::Previous: play_step(-1); break;
        case platform::MediaAction::Volume: set_volume(float(std::clamp(event.value, 0LL, 100LL))); break;
        case platform::MediaAction::SeekAbsolute:
            if (event.track.empty() || event.track == g_snapshot.now_uid) seek_playback(int(std::clamp(event.value, 0LL, 2147483647LL)));
            break;
        case platform::MediaAction::SeekRelative: seek_playback(int(std::clamp(static_cast<long long>(g_ui.pos_ms) + event.value, 0LL, 2147483647LL))); break;
        case platform::MediaAction::Shuffle: set_play_mode(event.value ? PlayMode::Shuffle : PlayMode::RepeatAll); break;
        case platform::MediaAction::Loop: set_play_mode(event.value == 2 ? PlayMode::RepeatOne : event.value == 1 ? PlayMode::RepeatAll : PlayMode::Sequential); break;
        default: break;
    }
}
void resolve_window_close(bool to_tray) {
    if (to_tray && (!g_ui.tray_enabled || !g_ui.tray_available || !g_host.hide || !g_host.hide())) {
        toast("系统托盘不可用，窗口已保留"); return;
    }
    if (g_ui.close_remember) { g_ui.close_behavior = to_tray ? 1 : 2; g_ui.config_dirty = true; }
    g_ui.close_dialog = false;
    if (!to_tray && g_host.close) g_host.close();
}
void refresh_output_devices() {
    if (g_ui.device_loading) return;
    g_ui.device_loading = true;
    background::start([] {
        std::string error; auto devices = audio::output_devices(error);
        post_ui([devices = std::move(devices), error] { g_ui.output_devices = devices; g_ui.device_error = error; g_ui.device_loading = false; });
    });
}
void select_output_device(const std::string& id) {
    bool paused, active; int index; long long position;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        const auto state = g_as.ps; index = g_as.playing;
        active = state == PlayState::Playing || state == PlayState::Paused || state == PlayState::Buffering || state == PlayState::Resolving || state == PlayState::Ready;
        paused = state == PlayState::Paused || ((state == PlayState::Buffering || state == PlayState::Resolving || state == PlayState::Ready) && g_play_ready_paused);
        position = (state == PlayState::Playing || state == PlayState::Paused) ? g_player.position_ms() : g_as.position_ms;
    }
    g_ui.device_error.clear();
    g_player.set_device(id); g_ui.output_device = id; g_ui.config_dirty = true;
    if (index >= 0 && active) {
        { std::lock_guard<std::mutex> lock(g_mu); g_resume_on_start = position; }
        start_play(index);
        { std::lock_guard<std::mutex> lock(g_mu); g_play_ready_paused = paused; }
    }
}
void playlist_filter_rows() {
    auto& view = g_playlist_view;
    view.rows.clear();
    view.album_rows.clear();
    if (view.catalog_open) {
        const auto filter = music::text::lower_ascii(music::text::trim(g_ui.playlist_filter));
        for (int i = 0; i < int(view.albums.size()); ++i) {
            const auto& album = view.albums[size_t(i)];
            if (filter.empty() || music::text::lower_ascii(album.name + " " + album.subtitle).find(filter) != std::string::npos)
                view.album_rows.push_back(i);
        }
        if (g_ui.playlist_sort == 1) std::stable_sort(view.album_rows.begin(), view.album_rows.end(),
            [&](int a, int b) { return view.albums[size_t(a)].name < view.albums[size_t(b)].name; });
    }
    const auto needle = music::text::normalize_title(g_ui.playlist_filter);
    for (int i = 0; i < int(view.playlist.tracks.size()); ++i) {
        const auto& t = view.playlist.tracks[size_t(i)];
        if (needle.empty() || music::text::normalize_title(t.title + " " + t.artist + " " + t.album).find(needle) != std::string::npos) view.rows.push_back(i);
    }
    if (g_ui.playlist_sort) std::stable_sort(view.rows.begin(), view.rows.end(), [&](int a, int b) {
        const auto& x = view.playlist.tracks[size_t(a)]; const auto& y = view.playlist.tracks[size_t(b)];
        if (g_ui.playlist_sort == 1) return x.title < y.title;
        if (g_ui.playlist_sort == 2) return x.artist == y.artist ? x.title < y.title : x.artist < y.artist;
        if (g_ui.playlist_sort == 3) return x.album == y.album ? x.title < y.title : x.album < y.album;
        return x.duration_ms < y.duration_ms;
    });
}
void playlist_rename_local() {
    const auto name = music::text::trim(g_ui.playlist_rename);
    if (!g_playlist_view.saved || name.empty() || name.size() > 1024) { toast("请输入有效的歌单名称"); return; }
    const auto key = g_playlist_view.playlist.key;
    library_edit([key, name](library::Library& next) { for (auto& p : next.playlists) if (p.key == key) p.name = name; }, [] { toast("歌单已重命名"); });
}

void library_export(bool playlist_only) {
    const auto path = music::text::trim(g_ui.library_transfer_path);
    if (path.empty()) { toast("请先填写导出文件路径（JSON）"); return; }
    if (platform::fs::u8path(path).lexically_normal() == platform::fs::u8path(library_path()).lexically_normal() || platform::exists(path)) { toast("请选择一个尚不存在的备份文件，避免覆盖原数据"); return; }
    const auto snapshot = g_library;
    const auto selected = playlist_only ? g_playlist_view.playlist.key : std::string();
    background::start([snapshot, selected, path] {
        library::Library backup;
        if (selected.empty()) backup = *snapshot;
        else for (const auto& p : snapshot->playlists) if (p.key == selected) {
            if (p.key == "favorites") backup.playlists.front() = p; else backup.upsert(p);
        }
        std::string error;
        static std::atomic<unsigned long long> serial{0};
        const auto temporary = path + ".aino-" + std::to_string(platform::process_id()) + "-" + std::to_string(++serial) + ".tmp";
        bool ok = backup.save(temporary, error);
        if (ok && !platform::move_file_no_replace(temporary, path)) { ok = false; error = "备份目标已存在或无法写入，原文件保留"; }
        platform::remove(temporary);
        post_ui([ok, path, error] { toast(ok ? "已导出：" + path : error, 7000); });
    });
}
void library_restore() {
    const auto path = music::text::trim(g_ui.library_transfer_path);
    if (path.empty()) { toast("请先填写备份文件路径（JSON）"); return; }
    const auto epoch = g_library_epoch;
    background::start([path, epoch] {
        auto imported = std::make_shared<library::Library>(); std::string error;
        const bool ok = platform::exists(path) && imported->load(path, error);
        post_ui([imported, ok, error, epoch] {
            if (epoch != g_library_epoch) return;
            if (!ok) { toast(error.empty() ? "备份文件不存在" : error, 7000); return; }
            library_edit([imported](library::Library& next) {
                for (const auto& p : imported->playlists) {
                    if (p.key == "favorites") for (const auto& t : p.tracks) { if (!next.contains(t)) next.toggle(t); }
                    else next.upsert(p);
                }
                for (auto it = imported->recent.rbegin(); it != imported->recent.rend(); ++it) next.remember(*it);
                for (const auto& pref : imported->lyric_preferences) if (next.lyric_preferences.count(pref.first) || next.lyric_preferences.size() < 4096) next.lyric_preferences[pref.first] = pref.second;
            }, [] { toast("备份已合并恢复，原有收藏保留"); });
        });
    });
}
void queue_move(int from, int to) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (from < 0 || to < 0 || from >= int(g_as.tracks.size()) || to >= int(g_as.tracks.size()) || from == to) return;
        const auto current = g_as.now_uid;
        auto track = std::move(g_as.tracks[size_t(from)]);
        g_as.tracks.erase(g_as.tracks.begin() + from); g_as.tracks.insert(g_as.tracks.begin() + to, std::move(track));
        g_as.playing = -1;
        for (int i = 0; i < int(g_as.tracks.size()); ++i) if (track_uid(g_as.tracks[size_t(i)]) == current) { g_as.playing = i; break; }
        ++g_as.tracks_version;
    }
    queue_mark_dirty();
}
static void detail_begin() {
    const int return_tab = g_ui.tab == 3 && g_playlist_view.open ? g_playlist_view.return_tab : g_ui.tab;
    if (g_ui.tab == 3 && g_playlist_view.open && !g_playlist_view.loading) {
        if (g_detail_history.size() >= 8) g_detail_history.erase(g_detail_history.begin());
        auto old = std::move(g_playlist_view); old.loading_more = false;
        g_detail_history.push_back({std::move(old), g_ui.playlist_filter, g_ui.playlist_rename,
            g_ui.playlist_sort, g_ui.catalog_section, g_ui.playlist_manage});
    } else if (g_ui.tab != 3 || !g_playlist_view.open) g_detail_history.clear();
    ++g_playlist_generation;
    g_playlist_view = {}; g_playlist_view.open = true; g_playlist_view.return_tab = return_tab;
    g_ui.tab = 3; g_ui.scroll_to_top = true; g_ui.catalog_section = 0;
    g_ui.playlist_filter.clear(); g_ui.playlist_sort = 0; g_ui.playlist_manage = false;
}

void library_open(size_t index) {
    if (index >= g_library->playlists.size()) return;
    g_detail_history.clear();
    ++g_playlist_generation;
    g_playlist_view = {};
    g_playlist_view.open = true; g_playlist_view.saved = true;
    g_playlist_view.playlist = g_library->playlists[index];
    g_playlist_view.library_revision = g_library_revision;
    g_ui.tab = 3; g_ui.scroll_to_top = true;
    g_ui.playlist_filter.clear(); g_ui.playlist_rename = g_playlist_view.playlist.name;
    g_ui.playlist_manage = false; g_ui.playlist_sort = 0;
    playlist_filter_rows();
}
void playlist_home() {
    ++g_playlist_generation; g_detail_history.clear(); g_playlist_view = {};
    g_ui.scroll_to_top = true;
}
void playlist_close() {
    ++g_playlist_generation;
    if (!g_detail_history.empty()) {
        auto old = std::move(g_detail_history.back()); g_detail_history.pop_back();
        g_playlist_view = std::move(old.view); g_ui.playlist_filter = std::move(old.filter);
        g_ui.playlist_rename = std::move(old.rename); g_ui.playlist_sort = old.sort;
        g_ui.catalog_section = old.section; g_ui.playlist_manage = old.manage;
        g_ui.tab = 3; g_ui.scroll_to_top = true;
        return;
    }
    const auto tab = g_playlist_view.return_tab;
    g_playlist_view = {};
    if (g_ui.tab == 3) g_ui.tab = tab;
}
void playlist_open_remote(library::Playlist playlist) {
    detail_begin();
    const auto generation = g_playlist_generation.load();
    g_playlist_view.loading = true;
    playlist.key = std::string(music::source_key(playlist.source)) + "#" +
        (playlist.remote_owner.empty() ? "" : playlist.remote_owner + "#") + playlist.remote_id;
    g_playlist_view.playlist = playlist; g_ui.tab = 3;
    g_ui.playlist_manage = false; g_ui.playlist_filter.clear(); g_ui.playlist_sort = 0;
    const auto settings = g_ui.settings;
    const auto uin = g_ui.qq_uin;
    background::start([playlist = std::move(playlist), settings, uin, generation]() mutable {
        net::CancelScope cancel([generation] { return g_shutdown.load() || generation != g_playlist_generation.load(); });
        std::string error;
        if (playlist.source == music::Source::Bilibili) {
            auto parts = music::bilibili_parts(playlist.remote_id, error);
            playlist.tracks = std::move(parts.tracks);
            if (!parts.title.empty()) playlist.name = parts.title;
            if (!parts.cover.empty()) playlist.cover = parts.cover;
        } else if (music::script_platform(playlist.source)) {
            playlist.tracks = music::board_tracks(playlist.source, playlist.remote_id, 200, error);
        } else playlist.tracks = playlist.source == music::Source::Netease ?
            ne::fetch_playlist(settings.netease_cookie, playlist.remote_id, error) :
            qq::fetch_playlist(settings.qq_cookie, playlist.remote_id, uin, error, 15000, playlist.remote_owner.empty());
        post_ui([playlist = std::move(playlist), error, generation]() mutable {
            if (generation != g_playlist_generation) return;
            g_playlist_view.loading = false; g_playlist_view.error = error;
            g_playlist_view.playlist = std::move(playlist);
            playlist_filter_rows();
            g_ui.scroll_to_top = true;
        });
    });
}

static music::CatalogPage local_catalog(const music::CatalogTarget& target, const library::Library& saved,
                                       const std::vector<music::Track>& queue, const music::Track& seed, bool albums) {
    music::CatalogPage out; out.target = target;
    std::set<std::string> seen, album_keys;
    const auto equal = [](const auto& a, const auto& b) {
        return music::text::lower_ascii(music::text::trim(a)) == music::text::lower_ascii(music::text::trim(b));
    };
    const auto add = [&](const music::Track& original) {
        if (net::cancelled()) return;
        int local = -1;
        for (int i = 0; i < int(original.variants.size()); ++i) if (original.variants[size_t(i)].source == music::Source::Local) { local = i; break; }
        if (local < 0) return;
        auto track = original; track.chosen = local;
        const auto& variant = track.current();
        const auto folder = platform::fs::u8path(variant.id).parent_path().u8string();
        bool matches = false;
        if (target.kind == music::CatalogKind::Artist) {
            for (const auto& a : music::track_artists(track)) if (equal(a.name, target.name)) { matches = true; break; }
        } else matches = equal(music::track_album(track), target.name) && folder == target.id;
        if (!matches || !seen.insert(track_uid(track)).second) return;
        if (albums) {
            const auto name = music::track_album(track);
            if (name.empty() || !album_keys.insert(folder + "\n" + name).second) return;
            music::CatalogTarget album; album.source = music::Source::Local; album.kind = music::CatalogKind::Album;
            album.id = folder; album.name = name; album.cover = track.cover_url; album.subtitle = track.artist;
            out.albums.push_back(std::move(album));
        } else out.tracks.push_back(std::move(track));
    };
    for (const auto& playlist : saved.playlists) for (const auto& track : playlist.tracks) add(track);
    for (const auto& track : queue) add(track);
    add(seed);
    out.description = "资料库中已导入的本地音乐";
    out.next_offset = int(albums ? out.albums.size() : out.tracks.size());
    return out;
}
static void catalog_request(bool albums, bool append) {
    auto& view = g_playlist_view;
    if (!view.open || !view.catalog_open || view.loading || view.loading_more) return;
    if (albums && view.catalog.kind != music::CatalogKind::Artist) return;
    const auto generation = g_playlist_generation.load();
    const auto target = view.catalog;
    const auto seed = view.catalog_seed;
    const int artist_index = view.catalog_artist_index;
    const int offset = append ? (albums ? view.albums_offset : view.tracks_offset) : 0;
    const auto continuation = append ? (albums ? view.albums_continuation : view.tracks_continuation) : std::string();
    if (append && !(albums ? view.albums_more : view.tracks_more)) return;
    if (albums || append || !view.playlist.tracks.empty()) view.loading_more = true; else view.loading = true;
    if (albums) { view.albums_error.clear(); view.albums_retry_append = append; }
    else { view.error.clear(); view.tracks_retry_append = append; }
    auto settings = g_ui.settings;
    settings.cancelled = [generation] { return g_shutdown.load() || generation != g_playlist_generation.load(); };
    const auto saved = g_library;
    std::vector<music::Track> queue;
    if (target.source == music::Source::Local) { std::lock_guard<std::mutex> lock(g_mu); queue = g_as.tracks; }
    background::start([generation, target, seed, artist_index, offset, continuation, settings, saved, queue = std::move(queue), albums, append]() mutable {
        net::CancelScope scope(settings.cancelled);
        music::CatalogPage page; page.target = target;
        auto resolved = target;
        if (resolved.id.empty() && !music::catalog_target_from_track(seed, target.kind, artist_index, settings, resolved, page.error)) {
            // Preserve the requested identity so retry resolves this song again.
        } else if (!settings.cancelled()) {
            page = resolved.source == music::Source::Local ? local_catalog(resolved, *saved, queue, seed, albums) :
                music::fetch_catalog(std::move(resolved), settings, albums, offset, continuation);
        }
        if (settings.cancelled()) return;
        if (page.more && (page.next_offset <= offset || page.next_offset >= 5000)) page.more = false;
        post_ui([generation, page = std::move(page), albums, append]() mutable {
            if (generation != g_playlist_generation.load() || !g_playlist_view.catalog_open) return;
            auto& view = g_playlist_view; view.loading = false; view.loading_more = false;
            if (!page.error.empty()) {
                if (albums) view.albums_error = page.error; else view.error = page.error;
                toast(page.error, 7000); return;
            }
            view.catalog = std::move(page.target);
            view.playlist.name = view.catalog.name; view.playlist.cover = view.catalog.cover;
            view.playlist.source = view.catalog.source;
            view.playlist.key = std::string(view.catalog.kind == music::CatalogKind::Artist ? "artist#" : "album#") +
                music::source_key(view.catalog.source) + "#" + view.catalog.id +
                (view.catalog.source == music::Source::Local ? "#" + view.catalog.name : "");
            if (!page.description.empty()) view.description = music::providers::strip_tags(page.description);
            if (albums) {
                if (!append) view.albums.clear();
                const auto before = view.albums.size();
                std::set<std::string> ids;
                const auto key = [](const music::CatalogTarget& album) { return album.source == music::Source::Local ? album.id + "\n" + album.name : album.id; };
                for (const auto& album : view.albums) ids.insert(key(album));
                for (auto& album : page.albums) if (ids.insert(key(album)).second) view.albums.push_back(std::move(album));
                if (append && !page.albums.empty() && view.albums.size() == before) {
                    page.more = false; view.albums_retry_append = false;
                    view.albums_error = "平台分页返回重复内容，已保留现有专辑";
                }
                view.albums_loaded = true; view.albums_more = page.more;
                view.albums_offset = page.next_offset; view.albums_continuation = std::move(page.continuation);
            } else {
                if (!append) view.playlist.tracks.clear();
                const auto before = view.playlist.tracks.size();
                std::set<std::string> ids;
                for (const auto& track : view.playlist.tracks) ids.insert(track_uid(track));
                for (auto& track : page.tracks) if (ids.insert(track_uid(track)).second) view.playlist.tracks.push_back(std::move(track));
                if (append && !page.tracks.empty() && view.playlist.tracks.size() == before) {
                    page.more = false; view.tracks_retry_append = false;
                    view.error = "平台分页返回重复内容，已保留现有歌曲";
                }
                view.tracks_more = page.more; view.tracks_offset = page.next_offset;
                view.tracks_continuation = std::move(page.continuation);
            }
            playlist_filter_rows();
            if (!append && g_ui.catalog_section == (albums ? 1 : 0)) g_ui.scroll_to_top = true;
            if (!albums && g_ui.catalog_section == 1 && !view.albums_loaded && view.albums_error.empty()) catalog_request(true, false);
        });
    });
}
static void open_catalog(music::CatalogTarget target, music::Track seed = {}, int artist_index = 0) {
    detail_begin();
    g_playlist_view.catalog_open = true; g_playlist_view.catalog = std::move(target);
    g_playlist_view.catalog_seed = std::move(seed); g_playlist_view.catalog_artist_index = artist_index;
    g_playlist_view.playlist.name = g_playlist_view.catalog.name;
    g_playlist_view.playlist.cover = g_playlist_view.catalog.cover;
    g_playlist_view.playlist.source = g_playlist_view.catalog.source;
    catalog_request(false, false);
}
void open_track_artist(const music::Track& track, int artist_index) {
    const auto artists = music::track_artists(track);
    if (artist_index < 0 || artist_index >= int(artists.size())) { toast("没有歌手信息"); return; }
    music::CatalogTarget target; target.source = track.current().source; target.kind = music::CatalogKind::Artist;
    target.name = artists[size_t(artist_index)].name; target.id = artists[size_t(artist_index)].id;
    open_catalog(std::move(target), track, artist_index);
}
void open_track_album(const music::Track& track) {
    if (track.variants.empty() || music::track_album(track).empty()) { toast("没有专辑信息"); return; }
    if (track.current().source == music::Source::Bilibili) { bilibili_open_parts(track); return; }
    music::CatalogTarget target; target.source = track.current().source; target.kind = music::CatalogKind::Album;
    target.name = music::track_album(track); target.id = track.current().album_id;
    target.cover = track.current().cover_url.empty() ? track.cover_url : track.current().cover_url;
    target.subtitle = track.current().artist.empty() ? track.artist : track.current().artist;
    if (target.source == music::Source::Local) target.id = platform::fs::u8path(track.current().id).parent_path().u8string();
    open_catalog(std::move(target), track);
}
void catalog_open_album(int index) {
    if (index < 0 || index >= int(g_playlist_view.albums.size())) return;
    open_catalog(g_playlist_view.albums[size_t(index)]);
}
void catalog_switch_section(int section) {
    if (!g_playlist_view.catalog_open || section < 0 || section > 1) return;
    g_ui.catalog_section = section; g_ui.playlist_filter.clear(); g_ui.playlist_sort = 0;
    playlist_filter_rows(); g_ui.scroll_to_top = true;
    if (section == 1 && !g_playlist_view.albums_loaded) catalog_request(true, false);
}
void catalog_load_more() { catalog_request(g_ui.catalog_section == 1, true); }
void catalog_retry() {
    const bool albums = g_ui.catalog_section == 1;
    const bool failed = !(albums ? g_playlist_view.albums_error : g_playlist_view.error).empty();
    catalog_request(albums, failed && (albums ? g_playlist_view.albums_retry_append : g_playlist_view.tracks_retry_append));
}

static void replace_queue_and_play(std::vector<music::Track> tracks, int index) {
    if (index < 0 || index >= int(tracks.size())) return;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        ++g_as.gen; g_play_generation.store(g_as.gen);
        g_as.tracks = std::move(tracks); g_as.playing = -1; ++g_as.tracks_version;
        g_play_next.clear(); g_shuffle_remaining.clear(); g_shuffle_history.clear(); g_shuffle_cursor = -1;
    }
    queue_mark_dirty(); start_play(index);
}
void bilibili_open_parts(const music::Track& track) {
    for (const auto& variant : track.variants) if (variant.source == music::Source::Bilibili) {
        library::Playlist playlist; playlist.source = music::Source::Bilibili;
        playlist.remote_id = variant.id; playlist.name = variant.album.empty() ? variant.title : variant.album;
        playlist.cover = variant.cover_url; playlist_open_remote(std::move(playlist)); return;
    }
}
void playlist_play(int index) {
    std::vector<music::Track> tracks;
    tracks.reserve(g_playlist_view.rows.size());
    int selected = 0;
    for (const int row : g_playlist_view.rows) {
        if (row == index) selected = int(tracks.size());
        tracks.push_back(g_playlist_view.playlist.tracks[size_t(row)]);
    }
    replace_queue_and_play(std::move(tracks), selected);
}
void play_search_result(int index) {
    // 点播使用本帧显示的结果，后台新搜索不能把同一索引换成另一首歌。
    replace_queue_and_play(g_snapshot.results, index);
}
void choose_search_source(int index, int variant, bool replay) {
    if (index < 0 || index >= int(g_snapshot.results.size())) return;
    const auto& displayed = g_snapshot.results[size_t(index)];
    if (variant < 0 || variant >= int(displayed.variants.size())) return;
    const auto& selected = displayed.variants[size_t(variant)];
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (auto& track : g_as.results) {
            if (track_uid(track) != track_uid(displayed)) continue;
            for (size_t i = 0; i < track.variants.size(); ++i) {
                const auto& candidate = track.variants[i];
                if (music::same_variant(candidate, selected)) {
                    track.chosen = int(i); ++g_as.results_version; break;
                }
            }
            break;
        }
    }
    if (replay) {
        auto tracks = g_snapshot.results; tracks[size_t(index)].chosen = variant;
        replace_queue_and_play(std::move(tracks), index);
    }
}
void playlist_choose_source(int index, int variant, bool replay) {
    if (index < 0 || index >= int(g_playlist_view.playlist.tracks.size())) return;
    auto& track = g_playlist_view.playlist.tracks[size_t(index)];
    if (variant < 0 || variant >= int(track.variants.size())) return;
    track.chosen = variant;
    if (replay) playlist_play(index);
}
void playlist_save_view() {
    if (!g_playlist_view.open || g_playlist_view.loading || !g_playlist_view.error.empty()) return;
    const auto generation = g_playlist_generation.load();
    library_edit([playlist = g_playlist_view.playlist](library::Library& next) { next.upsert(playlist); }, [generation] {
        if (generation == g_playlist_generation && !g_playlist_view.catalog_open) {
            g_playlist_view.saved = true;
            g_ui.playlist_rename = g_playlist_view.playlist.name;
        }
        toast("歌单已保存到本地");
    });
}
void library_save_current() {
    library::Playlist playlist;
    playlist.name = music::text::trim(g_ui.local_name);
    if (playlist.name.empty()) { toast("请先输入本地歌单名称"); return; }
    { std::lock_guard<std::mutex> lock(g_mu); playlist.tracks = g_as.tracks; }
    if (playlist.tracks.empty()) { toast("播放队列为空，请先播放歌曲或加入队列"); return; }
    playlist.key = "local#" + std::to_string(platform::ticks());
    library_edit([playlist = std::move(playlist)](library::Library& next) { next.upsert(playlist); },
        [] { g_ui.local_name.clear(); toast("播放队列已保存为本地歌单"); });
}
void library_import_folder(const std::string& folder, bool recursive) {
    if (g_ui.importing_playlist) { toast("正在导入，请稍候"); return; }
    if (music::text::trim(folder).empty()) { toast("请先选择或输入音乐文件夹路径"); return; }
    g_ui.importing_playlist = true;
    const auto epoch = g_library_epoch;
    toast("正在扫描音乐文件夹…");
    background::start([folder, recursive, epoch] {
        std::string error;
        auto playlist = library::scan_folder(folder, recursive, error, [] { return g_shutdown.load(); });
        if (error.empty()) for (auto& track : playlist.tracks) {
            if (g_shutdown.load()) return;
            const auto info = audio::inspect_mp3(track.current().id);
            if (info.valid && info.duration_ms > 0) track.duration_ms = int(std::min(info.duration_ms, 2147483647LL));
            track.variants.front().duration_ms = track.duration_ms;
        }
        if (g_shutdown.load()) return;
        post_ui([playlist = std::move(playlist), error, epoch]() mutable {
            if (epoch != g_library_epoch) return;
            g_ui.importing_playlist = false;
            if (!error.empty()) { toast(error, 7000); return; }
            const auto count = playlist.tracks.size();
            library_edit([playlist = std::move(playlist)](library::Library& next) { next.upsert(playlist); },
                [count] { toast("已导入 " + std::to_string(count) + " 首本地歌曲"); });
        });
    });
}
void library_fetch(library::Playlist playlist) {
    if (g_ui.importing_playlist) { toast("正在保存在线歌单，请稍候"); return; }
    if (playlist.source == music::Source::QQ && !playlist.remote_owner.empty() && playlist.remote_owner != g_ui.qq_uin) {
        toast("请登录保存此歌单时使用的 QQ 账号"); return;
    }
    g_ui.importing_playlist = true;
    const auto epoch = g_library_epoch;
    const auto settings = g_ui.settings;
    const auto uin = g_ui.qq_uin;
    toast("正在保存在线歌单…");
    background::start([playlist = std::move(playlist), settings, uin, epoch]() mutable {
        net::CancelScope cancel([] { return g_shutdown.load(); });
        std::string error;
        ne::Playlist ne_metadata;
        qq::Playlist qq_metadata;
        std::vector<music::Track> tracks;
        if (playlist.source == music::Source::Bilibili) {
            auto parts = music::bilibili_parts(playlist.remote_id, error);
            tracks = std::move(parts.tracks);
            if (!parts.title.empty()) playlist.name = parts.title;
            if (!parts.cover.empty()) playlist.cover = parts.cover;
        } else if (music::script_platform(playlist.source)) {
            tracks = music::board_tracks(playlist.source, playlist.remote_id, 200, error);
        } else tracks = playlist.source == music::Source::Netease ?
            ne::fetch_playlist(settings.netease_cookie, playlist.remote_id, error, 15000, &ne_metadata) :
            qq::fetch_playlist(settings.qq_cookie, playlist.remote_id, uin, error, 15000, playlist.remote_owner.empty(), &qq_metadata);
        const auto name = playlist.source == music::Source::Netease ? ne_metadata.name : qq_metadata.name;
        const auto cover = playlist.source == music::Source::Netease ? ne_metadata.cover : qq_metadata.cover;
        if (!name.empty()) playlist.name = name;
        if (!cover.empty()) playlist.cover = cover;
        if (error.empty() && tracks.empty()) error = "歌单没有可读取的曲目";
        if (error.empty()) {
            playlist.tracks = std::move(tracks);
            if (playlist.cover.empty()) playlist.cover = playlist.tracks.front().cover_url;
        }
        post_ui([playlist = std::move(playlist), error, epoch]() mutable {
            if (epoch != g_library_epoch) return;
            g_ui.importing_playlist = false;
            if (!error.empty()) { toast("保存歌单失败：" + error, 7000); return; }
            library_edit([playlist = std::move(playlist)](library::Library& next) { next.upsert(playlist); },
                [] { g_ui.playlist_link.clear(); toast("在线歌单已保存到本地，曲目快照可随时打开"); });
        });
    });
}
void library_import_link() {
    library::Playlist playlist;
    if (!library::parse_link(g_ui.playlist_link, playlist.source, playlist.remote_id)) {
        toast("请粘贴网易云或 QQ 的完整歌单链接（短链接请先在浏览器打开）", 6500); return;
    }
    playlist.key = std::string(music::source_key(playlist.source)) + "#" + playlist.remote_id;
    playlist.name = std::string(music::source_name(playlist.source)) + "歌单 " + playlist.remote_id;
    library_fetch(std::move(playlist));
}
void library_save_remote(int site, int index) {
    library::Playlist playlist;
    if (site == 1) {
        if (index < 0 || index >= int(g_qq_snap.playlists.size())) return;
        const auto& p = g_qq_snap.playlists[size_t(index)];
        playlist.source = music::Source::QQ; playlist.remote_id = p.dirid;
        playlist.name = p.name; playlist.cover = p.cover; playlist.remote_owner = g_ui.qq_uin;
    } else {
        if (index < 0 || index >= int(g_ne_snap.playlists.size())) return;
        const auto& p = g_ne_snap.playlists[size_t(index)];
        playlist.source = music::Source::Netease; playlist.remote_id = p.id;
        playlist.name = p.name; playlist.cover = p.cover;
    }
    playlist.key = std::string(music::source_key(playlist.source)) + "#" +
        (playlist.remote_owner.empty() ? "" : playlist.remote_owner + "#") + playlist.remote_id;
    library_fetch(std::move(playlist));
}

void begin_login(int site) {
    if (site < 0 || site >= 3) return;
    if (g_wait_login_site.load() >= 0) { toast("已有登录连接正在进行，请先完成或取消"); return; }
    g_wait_login_site.store(site);
    login::start(login_site(site));
}

void clear_data(ClearAction action) {
    if (action == ClearAction::Cookies) {
        g_wait_login_site.store(-1);
        login::forget(login::Site::QQ); login::forget(login::Site::NetEase);
        {
            std::lock_guard<std::mutex> lock(g_qq.m);
            g_qq.cookie.clear(); g_qq.uin.clear(); g_qq.nickname.clear(); g_qq.playlists.clear();
            ++g_qq.playlists_revision;
            g_qq.err.clear(); g_qq.logged = false; g_qq.loading = false;
        }
        {
            std::lock_guard<std::mutex> lock(g_ne.m);
            g_ne.cookie.clear(); g_ne.uid.clear(); g_ne.nickname.clear(); g_ne.playlists.clear();
            ++g_ne.playlists_revision;
            g_ne.err.clear(); g_ne.logged = false; g_ne.loading = false;
        }
        g_ui.settings.qq_cookie.clear(); g_ui.settings.netease_cookie.clear();
        g_ui.qq_uin.clear(); g_ui.ne_uid.clear();
        g_ui.config_dirty = true;
        toast(save_config() ? "已清除两个账号的登录信息；浏览器中的登录不受影响" : "账号已退出，但配置文件保存失败，请检查配置目录权限", 6500);
    } else if (action == ClearAction::Library) {
        ++g_library_epoch; // 先拒绝清理前启动的导入，清理动作仍与已有写入串行。
        library_edit([](library::Library& next) { next.reset(); }, [] {
            g_library_writable = true; playlist_home();
            g_ui.importing_playlist = false; g_ui.playlist_link.clear(); g_ui.local_name.clear();
            toast("已清除本地收藏、保存的歌单及在线快照");
        }, true);
    } else if (action == ClearAction::Queue) {
        queue_clear();
        toast(queue_save() ? "已清空当前列表；正在播放的歌曲可继续播放" : "列表已清空，但保存失败，请检查配置目录权限", 6500);
    } else if (action == ClearAction::Cache) {
        cancel_preload();
        std::unique_lock<std::shared_mutex> cache_write(g_play_cache_mutex, std::try_to_lock);
        std::string keep;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (g_as.ps == PlayState::Playing || g_as.ps == PlayState::Paused || g_as.ps == PlayState::Ready)
                keep = "track-" + std::to_string(g_as.gen) + ".";
        }
        if (!keep.empty() && g_current_media) {
            const auto name = platform::fs::u8path(g_current_media->converted).filename().u8string();
            keep = name.substr(0, name.find('.')) + ".";
        }
        const auto result = cache::clear_audio(platform::cache_root(), platform::process_id(), keep, cache_write.owns_lock());
        g_covers.clear();
        toast("已清理 " + std::to_string(result.files) + " 个音频缓存（" + fmt_size((long long)result.bytes) + "）及封面缓存" +
            (result.kept ? "；使用中的文件已保留" : "") + (result.errors ? "；部分文件未能删除" : ""), 7500);
    }
}

void check_updates(bool notify) {
    if (g_update_view.status == UpdateStatus::Installed) { if (notify) toast("新版已安装，请重新启动播放器", 6000); return; }
    if (g_update_view.status == UpdateStatus::Checking || g_update_view.status == UpdateStatus::Downloading ||
        g_update_view.status == UpdateStatus::Installing) return;
    g_update_view.status = UpdateStatus::Checking;
    g_update_view.message = "正在检查 GitHub 正式版…";
    background::start([notify] {
        UpdateView result;
        try {
            net::Request request;
            request.url = updates::kApi;
            request.headers = {"Accept: application/vnd.github+json"};
            request.timeout_ms = 5000; request.follow_redirects = false;
            result = updates::parse_release(net::execute(request), kVersion, updates::asset_suffix());
        } catch (...) { result.status = UpdateStatus::Failed; result.message = "检查更新失败，请稍后重试"; }
        post_ui([result = std::move(result), notify]() mutable {
            g_update_view = std::move(result);
            if (g_update_view.status == UpdateStatus::Available) toast("发现新版本 " + g_update_view.version + "，可在设置 → 版本更新中下载", 7000);
            else if (notify) toast(g_update_view.message, 5000);
        });
    });
}
void open_update(bool download) {
#if defined(_WIN32) || defined(__linux__)
    if (download && g_update_view.installer) {
        if (g_update_view.status != UpdateStatus::Available) return;
#ifdef __linux__
        if (!platform::deb_installer_available()) {
            toast("系统缺少 pkexec 或 apt-get，请安装 pkexec 后重试；无需卸载旧版", 7000); return;
        }
#endif
        const auto release = g_update_view;
        if (!release.asset_size || release.digest.size() != 71 || release.digest.rfind("sha256:", 0) != 0 ||
            release.digest.substr(7).find_first_not_of("0123456789abcdef") != std::string::npos) {
            toast("安装包缺少有效的 SHA-256 信息，请从 GitHub 发布页下载", 6000); return;
        }
        g_update_cancel.store(false);
        g_update_view.status = UpdateStatus::Downloading; g_update_view.progress = 0;
        g_update_view.message = "正在下载安装包并校验 SHA-256…";
        background::start([release] {
            const auto tag = release.version.front() == 'v' ? release.version : "v" + release.version;
#ifdef _WIN32
            const auto path = platform::join(g_temp_dir, "aino-update-" + tag + ".exe");
#else
            const auto path = platform::join(g_temp_dir, "aino-update-" + tag + ".deb");
#endif
            std::string error;
            bool ok = false;
            try {
                int previous = -1;
                ok = net::download(release.asset_url, path, {}, [&](long long received, long long) {
                    if (received > release.asset_size) { g_update_cancel.store(true); return; }
                    const int progress = int(received * 100 / release.asset_size);
                    if (progress != previous) {
                        previous = progress;
                        post_ui([progress] { g_update_view.progress = progress; });
                    }
                }, g_update_cancel, error, 60000);
                if (ok) ok = platform::file_size(path) == release.asset_size &&
                             platform::sha256_file(path) == release.digest.substr(7);
                if (!ok && error.empty()) error = "安装包大小或 SHA-256 校验不符";
            } catch (...) { ok = false; error = "安装包下载失败，请稍后重试"; }
            if (!ok || g_update_cancel.load()) {
                platform::remove(path);
                post_ui([error] { g_update_view.status = UpdateStatus::Available; g_update_view.message = error; toast(error, 7000); });
                return;
            }
#ifdef _WIN32
            post_ui([path] {
                if (!g_host.close || !platform::launch_installer(path)) {
                    g_update_view.status = UpdateStatus::Available; g_update_view.message = "无法启动安装器，请重试或从发布页下载";
                    platform::remove(path); return;
                }
                g_update_view.status = UpdateStatus::Installing;
                g_update_view.message = "安装器已启动，正在保存配置并退出…";
                g_host.close();
            });
#else
            post_ui([] {
                g_update_view.status = UpdateStatus::Installing;
                g_update_view.message = "请完成系统授权窗口，正在覆盖升级。安装期间退出会等待安装结束…";
            });
            if (g_update_cancel.load() || g_shutdown.load()) { platform::remove(path); return; }
            const auto data_dir = platform::config_dir();
            const auto log = platform::join(data_dir.empty() ? g_temp_dir : data_dir, "update-install.log");
            const int code = platform::install_deb(path, log);
            platform::remove(path);
            post_ui([code, log] {
                g_update_view.status = code == 0 ? UpdateStatus::Installed : UpdateStatus::Available;
                g_update_view.message = code == 0 ? "新版已覆盖安装，请重新启动播放器；配置和歌单已保留" :
                    code == 126 ? "已取消系统授权，旧版仍可使用" :
                    "覆盖安装未完成（退出码 " + std::to_string(code) + "），无需卸载旧版。详情见 " + log;
                toast(g_update_view.message, 8000);
            });
#endif
        });
        return;
    }
#endif
    const auto url = download && !g_update_view.asset_url.empty() ? g_update_view.asset_url :
        !g_update_view.release_url.empty() ? g_update_view.release_url : std::string(updates::kReleases);
    if (!platform::open_browser(url)) toast("无法打开默认浏览器，请检查浏览器设置");
}
void open_project(ProjectLink link) {
    std::string url = project::github;
    if (link == ProjectLink::Issues) url += "/issues";
    else if (link == ProjectLink::License) url += "/blob/master/LICENSE";
    else if (link == ProjectLink::Dependencies) url += "/blob/master/vendor/README.md";
    if (!platform::open_browser(url)) toast("无法打开默认浏览器");
}
const cover::Image& brand_icon() {
    static const cover::Image icon = [] {
        cover::Image image;
        cover::detail::decode(std::vector<uint8_t>(std::begin(brand::png), std::end(brand::png)), 64, image);
        return image;
    }();
    return icon;
}

static std::array<LoginView, 3> g_login_view;
static LyricView g_lyric_view;
static StoragePaths g_paths;
ViewModel view_model() {
    static std::shared_ptr<const library::Library> indexed;
    static library::FavoriteIndex favorites;
    static unsigned indexed_revision = ~0u;
    if (indexed != g_library || indexed_revision != g_library_revision) {
        favorites = library::FavoriteIndex(g_library->playlists.front().tracks);
        indexed = g_library;
        indexed_revision = g_library_revision;
    }
    return {g_ui, g_snapshot, *g_library, g_qq_snap, g_ne_snap, g_disc_snap, g_playlist_search_snap, g_dls_snap,
        g_login_view, g_lyric_view, g_paths, g_update_view, g_playlist_view, g_sync_site.load(), g_sync_index.load(), g_toast_until.load(), g_library, g_library_revision, favorites};
}
bool initialize(bool smoke_test) {
    net::shutdown_signal = &g_shutdown;
    if (!smoke_test) config::load(g_ui);
    g_player.set_device(g_ui.output_device);
    std::string library_error;
    if (!smoke_test) g_library_writable = g_library->load(library_path(), library_error);
    ++g_library_revision;
    if (!library_error.empty()) toast(library_error, 10000);
    g_temp_dir = platform::temp_dir();
    if (g_temp_dir.empty()) { std::fprintf(stderr, "Cannot create cache directory\n"); return false; }

    g_paths = {platform::config_dir(), platform::cache_root(), g_temp_dir};
    return true;
}
void start(HostCallbacks host) {
    g_host = std::move(host);
    g_win = &g_host;
    g_covers.set_notifier([] { if (g_win) g_win->wake(); });
    // 上次登录过就直接拉歌单（后台，不挡首屏）
    if (!g_ui.settings.qq_cookie.empty()) {
        {
            std::lock_guard<std::mutex> lk(g_qq.m);
            g_qq.cookie = g_ui.settings.qq_cookie;
            g_qq.uin = g_ui.qq_uin;
            g_qq.logged = true;
        }
        qq_refresh();
    }
    if (!g_ui.settings.netease_cookie.empty()) {
        {
            std::lock_guard<std::mutex> lk(g_ne.m);
            g_ne.cookie = g_ui.settings.netease_cookie;
            g_ne.uid = g_ui.ne_uid;
            g_ne.logged = true;
        }
        ne_refresh();
    }
    // 播放/缓冲/检索期间的刷新泵：只在需要时 wake()，其余时间主循环睡着
    background::start([] {
        unsigned long long toast_seen = 0, timer_seen = 0, memory_seen = platform::ticks();
        while (!g_shutdown.load()) {
            bool busy = false;
            {
                std::lock_guard<std::mutex> lk(g_mu);
                busy = background::error_pending.load() || g_wait_login_site.load() >= 0 || login::state(login::Site::QQ).return_requested.load() ||
                       login::state(login::Site::NetEase).return_requested.load() || g_as.searching || g_as.ps == PlayState::Playing || g_as.ps == PlayState::Buffering ||
                       g_as.ps == PlayState::Resolving;
            }
            const unsigned long long now = platform::ticks();
            // 暂停且无输入时也维护缓存；先补一帧更新可见封面的访问时间，再回收离屏资源。
            if (now >= memory_seen + 60000) { if (g_win) g_win->wake(); memory_seen = now; }
            if (g_sleep_deadline.load()) {
                if (now >= timer_seen + 1000) { if (g_win) g_win->wake(); timer_seen = now; }
            } else timer_seen = 0;
            const unsigned long long until = g_toast_until.load();
            if (now < until) {
                busy = true;
                toast_seen = until;
            } else if (toast_seen != 0) {
                toast_seen = 0;
                if (g_win) g_win->wake();   // toast 刚过期，补一帧把它擦掉
            }
            if (busy && g_win) g_win->wake();
            // 歌词滚动保留高帧率；普通进度刷新按实际需要降频。
            const int fps = g_fps_hint.load();
            // 目标周期减去本帧已经花掉的时间：不然 30fps 实际只有 1000/(33+14) ≈ 21fps
            const int spent = g_frame_ms_x10.load() / 10;
            int wait = 250;
            if (fps > 0) wait = 1000 / fps - spent;
            else if (busy) wait = 90;
            if (wait < 2) wait = 2;
            platform::sleep_ms(wait);
        }
    });

}
void tick() {
        background::reap();
        for (const auto& error : background::take_errors()) toast("后台任务失败：" + error, 8000);
        drain_ui_mail();
        if (g_ui.sleep_deadline) {
            const auto now = platform::ticks();
            if (now >= g_ui.sleep_deadline) {
                g_ui.sleep_deadline = 0; g_ui.sleep_remaining_seconds = 0; g_sleep_deadline.store(0);
                stop_playback(); toast("定时已到，已停止播放");
            } else g_ui.sleep_remaining_seconds = unsigned((g_ui.sleep_deadline - now + 999) / 1000);
        }
        if (g_playlist_view.open && g_playlist_view.saved && g_playlist_view.library_revision != g_library_revision) {
            const auto key = g_playlist_view.playlist.key;
            for (const auto& playlist : g_library->playlists) if (playlist.key == key) { g_playlist_view.playlist = playlist; break; }
            g_playlist_view.library_revision = g_library_revision;
            playlist_filter_rows();
        }
        refresh_snapshot(g_snapshot);
        auto& snap = g_snapshot;
        {
            // 下载状态变化后才复制，绘制读取只读快照。
            std::lock_guard<std::mutex> lk(g_dlm);
            if (g_download_snapshot_revision != g_download_revision) {
                g_dls_snap = g_dls; g_download_snapshot_revision = g_download_revision;
            }
        }
        qq_snapshot();
        ne_snapshot();
        disc_snapshot();
        // 网易云的 uid 是登录后由账号接口带回来的，回填一次让下次启动不用再查
        if (!g_ne_snap.uid.empty() && g_ui.ne_uid != g_ne_snap.uid) {
            g_ui.ne_uid = g_ne_snap.uid;
            g_ui.config_dirty = true;
        }

        // 官网浏览器回传（QQ / 网易云各一套）：成功就保存 Cookie 并拉取歌单。
        if (const int site = g_wait_login_site.load(); site >= 0) {
            const login::Site ls = login_site(site);
            auto& st = login::state(ls);
            if (st.done.load()) {
                g_wait_login_site.store(-1);
                std::string cookie, uid, note;
                {
                    std::lock_guard<std::mutex> lk(st.m);
                    cookie = st.cookie;
                    uid = st.uid;
                    note = st.note;
                }
                if (!cookie.empty()) {
                    if (site == 1) {
                        {
                            std::lock_guard<std::mutex> lk(g_qq.m);
                            g_qq.cookie = cookie;
                            g_qq.uin = uid;
                            g_qq.logged = true;
                            g_qq.err.clear();
                        }
                        g_ui.settings.qq_cookie = cookie;
                        g_ui.qq_uin = uid;
                        g_ui.config_dirty = true;
                        toast("QQ 音乐登录成功");
                        qq_refresh();
                    } else {
                        {
                            std::lock_guard<std::mutex> lk(g_ne.m);
                            g_ne.cookie = cookie;
                            g_ne.uid = uid;
                            g_ne.logged = true;
                            g_ne.err.clear();
                        }
                        g_ui.settings.netease_cookie = cookie;
                        if (!uid.empty()) g_ui.ne_uid = uid;
                        g_ui.config_dirty = true;
                        toast("网易云音乐登录成功");
                        ne_refresh();
                    }
                } else {
                    toast(note.empty() ? "登录已取消" : note, 6000);
                }
            }
        }
        for (auto site : {login::Site::QQ, login::Site::NetEase})
            if (login::state(site).return_requested.exchange(false)) g_host.activate();
        if (const int goto_tab = g_goto_tab.exchange(-1); goto_tab >= 0) g_ui.tab = goto_tab;

        // ---- 后台交过来的"可以播了"信号，在主线程打开 miniaudio ----
        if (snap.ps == PlayState::Ready && !snap.pending_path.empty()) {
            std::shared_ptr<audio::Stream> stream;
            bool paused;
            { std::lock_guard<std::mutex> lock(g_mu); stream = std::move(g_pending_stream); paused = g_play_ready_paused; }
            const bool streaming = bool(stream);
            if ((streaming ? g_player.open_stream(std::move(stream)) : g_player.open(snap.pending_path)) && (paused || g_player.play())) {
                g_player.set_volume(int(g_ui.volume));
                g_ui.dur_ms = float(g_player.duration_ms());
                if (g_ui.dur_ms <= 0) g_ui.dur_ms = float(snap.pending_duration_ms);
                long long resume;
                { std::lock_guard<std::mutex> lock(g_mu); resume = g_pending_seek; g_pending_seek = 0; }
                resume = std::clamp(resume, 0LL, std::max(0LL, static_cast<long long>(g_ui.dur_ms) - 1));
                if (!streaming && resume) g_player.seek_ms(resume);
                g_ui.pos_ms = float(g_player.position_ms());
                std::lock_guard<std::mutex> lock(g_mu);
                if (g_as.gen == snap.gen) g_as.ps = paused ? PlayState::Paused : PlayState::Playing;
                snap.ps = paused ? PlayState::Paused : PlayState::Playing;
            } else {
                const auto error = g_player.error(); g_player.close();
                std::lock_guard<std::mutex> lock(g_mu);
                if (g_as.gen == snap.gen) { g_as.ps = PlayState::Failed; g_as.error = error; }
                snap.ps = PlayState::Failed;
                snap.error = error;
            }
        }
        // 起播后的网络或解码失败也有明确恢复路径，不能当作自然结束跳歌。
        if ((snap.ps == PlayState::Playing || snap.ps == PlayState::Paused) && g_player.stream_failed()) {
            const auto position = g_player.position_ms();
            const auto error = g_player.error();
            g_player.close(); cancel_preload();
            music::Track track; int gen;
            {
                std::lock_guard<std::mutex> lock(g_mu);
                track = g_as.now_track; gen = g_as.gen;
                g_as.ps = PlayState::Buffering; g_as.pending_path.clear(); g_as.buffer = 0;
                g_as.position_ms = position; g_pending_seek = position; g_play_ready_paused = snap.ps == PlayState::Paused;
                g_active_stream_spec = {}; g_pending_stream.reset();
            }
            toast(error + "，正在恢复完整文件播放", 6000);
            auto settings = g_ui.settings;
            for (int i = 0; i < music::kSourceCount; ++i) settings.enable[i] = (g_ui.src_mask >> i) & 1u;
            background::start(do_play, g_win, std::move(track), std::move(settings), gen, false);
            snap.ps = PlayState::Buffering;
        }

        // 每代失败仅处理一次；单曲循环也跳过失败曲，候选耗尽后停止，避免无限重试。
        {
            static int handled_failure = -1;
            if (snap.ps == PlayState::Failed && handled_failure != snap.gen) {
                handled_failure = snap.gen;
                bool skip, paused;
                { std::lock_guard<std::mutex> lock(g_mu);
                  skip = g_track_failure_gen == snap.gen; paused = g_play_ready_paused; }
                if (skip && !paused && !g_ui.sleep_after_track) {
                    g_failed_tracks.insert(snap.now_uid);
                    const int next = next_queue_index(1, false, true, &g_failed_tracks);
                    if (next >= 0) {
                        toast("已跳过无法播放的歌曲：" + snap.now_title + " · " + snap.error, 6500);
                        start_play_attempt(next, true);
                    } else {
                        cancel_preload();
                        toast("没有可继续播放的歌曲，已停止。" + snap.error, 9000);
                    }
                } else {
                    if (g_ui.sleep_after_track) g_ui.sleep_after_track = false;
                    toast(snap.error.empty() ? "播放失败，请重试或检查音频设备" : snap.error, 9000);
                }
                refresh_snapshot(g_snapshot);
            }
        }

        // ---- 每帧同步播放位置 ----
        if (snap.ps == PlayState::Playing || snap.ps == PlayState::Paused) {
            const long long p = g_player.position_ms();
            if (p >= 5000 && !g_player.buffering()) g_failed_tracks.clear();
            g_ui.pos_ms = float(std::max(p, 0LL));
            { std::lock_guard<std::mutex> lock(g_mu); g_as.position_ms = std::max(p, 0LL); }
            const long long d = g_player.duration_ms();
            if (d > 0) g_ui.dur_ms = float(d);
            // 放完了自动下一首
            if (snap.ps == PlayState::Playing && g_player.is_ended()) {
                if (g_ui.sleep_after_track) {
                    g_ui.sleep_after_track = false;
                    stop_playback(); toast("当前歌曲播放结束，已定时停止");
                } else play_step(1, true);
                refresh_snapshot(g_snapshot);
            }
        }

        for (int site = 0; site < 3; ++site) {
            auto& state = login::state(login_site(site));
            g_login_view[size_t(site)].connecting = state.running.load() && !state.done.load();
            std::lock_guard<std::mutex> lock(state.m);
            g_login_view[size_t(site)].note = state.note;
        }
        {
            std::lock_guard<std::mutex> lock(g_lyr.m);
            g_lyric_view = {g_lyr.doc, g_lyr.for_uid, g_lyr.loading};
        }
        static int recorded_generation = -1;
        if (snap.ps == PlayState::Playing && g_ui.pos_ms >= 5000 && !snap.now_uid.empty() && recorded_generation != snap.gen) {
            recorded_generation = snap.gen;
            const auto track = snap.now_track;
            library_edit([track](auto& next) { next.remember(track); });
        }
        if (g_player.take_device_fallback()) { g_ui.device_error = "所选设备不可用，当前使用系统默认输出"; toast(g_ui.device_error); }
        static unsigned long long last_device_recovery = 0;
        if ((snap.ps == PlayState::Playing || snap.ps == PlayState::Paused) && g_player.take_device_change() && platform::ticks() > last_device_recovery + 3000) {
            last_device_recovery = platform::ticks();
            select_output_device(""); toast("音频设备已断开，正在恢复系统默认输出"); refresh_output_devices();
        }
        refresh_preload();
}
void bootstrap(bool initial_search, bool smoke_test) {
    if (!smoke_test) {
        lx::Engine::instance().set_restoring(true);
        lx_source_task(LxTask::Reload, [] {
            struct Done { ~Done() { lx::Engine::instance().set_restoring(false); } } done;
            const auto dir = lx::default_dir();
            if (dir.empty()) throw std::runtime_error("无法访问用户配置目录，不能恢复音源");
            const auto target = platform::fs::u8path(dir);
            platform::fs::create_directories(target);
            std::string migration_error;
#ifdef _WIN32
            // 旧便携版只复制缺失脚本，不覆盖配置目录中的新版，也不删除原文件。
            try {
                if (platform::env("AINO_LX_DIR").empty()) {
                    wchar_t executable[32768]{};
                    if (GetModuleFileNameW(nullptr, executable, 32768)) {
                        const auto legacy = platform::fs::path(executable).parent_path() / "sources";
                        std::error_code ec;
                        if (platform::fs::exists(legacy, ec) && !platform::fs::equivalent(legacy, target)) {
                            for (const auto& entry : platform::fs::directory_iterator(legacy)) {
                                if (!entry.is_regular_file() || !lx::script_file(entry.path())) continue;
                                platform::fs::copy_file(entry.path(), target / entry.path().filename(), platform::fs::copy_options::skip_existing);
                            }
                        }
                        if (ec) throw std::runtime_error("无法读取旧版音源目录：" + ec.message());
                    }
                }
            } catch (const std::exception& error) { migration_error = error.what(); }
#endif
            if (g_shutdown.load()) return;
            if (lx::Engine::instance().load_dir(dir) < 0) throw std::runtime_error(lx::Engine::instance().preferences_error());
            if (!migration_error.empty()) post_ui([migration_error] { toast("部分旧音源未迁移，请重新导入：" + migration_error, 6000); });
        });
    }
    if (!smoke_test) queue_load();
    if (initial_search) start_search();
    if (!smoke_test) { disc_refresh(g_ui.disc_site); check_updates(false); }
}
void finish_frame(float frame_ms) {
    const auto& snap = g_snapshot;
    static unsigned long long last_trim = 0;
    static std::atomic<bool> collecting{false};
    const auto now = platform::ticks();
    if (!g_shutdown.load() && now > last_trim + 60000) {
        last_trim = now;
        g_covers.trim_idle(now);
        if (!lx::Engine::instance().loading() && !collecting.exchange(true)) background::start([] {
            struct Done { ~Done() { collecting.store(false); } } done;
            net::CancelScope cancel([] { return g_shutdown.load(); });
            lx::Engine::instance().collect_idle();
        });
    }
        // Cookie / 音量 / 音源开关变了就落盘，节流到 2 秒一次，别每帧写文件
        if (g_ui.config_dirty) {
            const double now_s = double(platform::ticks()) / 1000.0;
            if (now_s - g_ui.last_save_s > 2.0) {
                g_ui.last_save_s = now_s;
                save_config();
            }
        }

        {
            int fps = 0;
            if (snap.ps == PlayState::Playing) {
                // 播放时仅定时刷新进度。歌词滚动由 UI 按需请求动画帧，静止时不重画整窗到 60 FPS。
                fps = 10;
            } else if (snap.ps == PlayState::Buffering || snap.ps == PlayState::Resolving) {
                fps = 30;
            }
            if (g_ui.window_hidden) fps = 1;
            g_fps_hint.store(fps);
        }

        g_ui.frame_ms = g_ui.frame_ms * 0.9f + frame_ms * 0.1f;
        g_frame_ms_x10.store(int(g_ui.frame_ms * 10.0f));
}
void shutdown(bool smoke_test) {
    { std::lock_guard<std::mutex> lock(g_queue_writer_mutex); g_shutdown.store(true); }
    g_queue_writer_changed.notify_all();
    g_folder_cancel.store(true); g_lyric_cancel.store(true);
    { std::lock_guard<std::mutex> lock(g_dlm); for (auto& entry : g_download_cancel) entry.second->store(true); g_download_pending.clear(); }
    cancel_preload();
    g_update_cancel.store(true);
    g_covers.clear(); // 退出只等待正在下载的封面，不继续处理积压的队列。
    login::shutdown();
    g_player.shutdown(); // 入口仍持有 UI 线程的 COM apartment，此处先释放音频引擎。
    { std::lock_guard<std::mutex> lock(g_mu); g_pending_stream.reset(); g_active_stream_spec = {}; }
    background::stop();
    g_current_media.reset();
    if (!smoke_test) {
        save_config();
        queue_save(); // 后台任务全部结束后保存最终队列。
    }
    g_win = nullptr;
}
void cancel_login(int site) { if (site >= 0 && site < 3) login::cancel(login_site(site)); }
void seek_playback(int milliseconds) {
    audio::StreamSpec spec; music::Track track; int gen = 0;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        spec = g_active_stream_spec;
        if (!spec.url.empty() || !spec.local_path.empty()) {
            spec.start_ms = std::clamp<long long>(milliseconds, 0, spec.duration_ms > 0 ? spec.duration_ms - 1 : std::max(0,milliseconds));
            const bool paused = g_as.ps == PlayState::Paused || ((g_as.ps == PlayState::Buffering || g_as.ps == PlayState::Ready) && g_play_ready_paused);
            gen = ++g_as.gen; g_play_generation.store(gen);
            track = g_as.now_track; g_pending_stream.reset(); g_as.pending_path.clear();
            g_as.position_ms = spec.start_ms; g_pending_seek = spec.start_ms; g_play_ready_paused = paused;
            g_active_stream_spec = spec; g_as.ps = PlayState::Buffering; g_as.buffer = 0;
        }
    }
    if (!gen) {
        g_player.seek_ms(milliseconds);
        std::lock_guard<std::mutex> lock(g_mu); g_as.position_ms = g_player.position_ms();
        return;
    }
    g_player.close(); cancel_preload(); g_ui.pos_ms = float(spec.start_ms);
    auto settings = g_ui.settings;
    for (int i = 0; i < music::kSourceCount; ++i) settings.enable[i] = (g_ui.src_mask >> i) & 1u;
    background::start([spec, track, settings, gen]() mutable {
        auto cancelled = [gen] { return g_shutdown.load() || gen != g_play_generation.load(); };
        std::string error; auto stream = audio::Stream::open(spec, cancelled, error);
        if (cancelled()) return;
        if (stream) finish_stream(g_win, track.current(), spec, std::move(stream), gen);
        else {
            { std::lock_guard<std::mutex> lock(g_mu); if (gen != g_as.gen) return; g_active_stream_spec = {}; }
            do_play(g_win, std::move(track), std::move(settings), gen, false);
        }
    });
}

void choose_source(int index, int variant, bool replay) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (index < 0 || index >= int(g_as.tracks.size())) return;
        auto& track = g_as.tracks[size_t(index)];
        if (variant < 0 || variant >= int(track.variants.size())) return;
        track.chosen = variant;
        ++g_as.tracks_version;
    }
    queue_mark_dirty();
    if (replay) start_play(index);
}
void library_remove(size_t index) {
    if (!index || index >= g_library->playlists.size()) return;
    const auto key = g_library->playlists[index].key;
    library_edit([key](library::Library& next) {
        auto& rows = next.playlists;
        rows.erase(std::remove_if(rows.begin() + 1, rows.end(), [&](const auto& p) { return p.key == key; }), rows.end());
    }, [] { toast("已从本机移除歌单"); });
}
void library_choose_folder() {
    if (g_folder_selecting || g_shutdown.load()) return;
    g_folder_selecting = true;
    g_folder_cancel.store(false);
    void* owner = g_host.native_window ? g_host.native_window() : nullptr;
    const auto epoch = g_library_epoch;
    background::start([owner, epoch] {
        std::string error;
        const auto folder = platform::choose_music_folder(owner, g_folder_cancel, error);
        post_ui([folder, error, epoch] {
            g_folder_selecting = false;
            if (epoch != g_library_epoch) return;
            if (!folder.empty()) g_ui.local_folder = folder;
            if (!error.empty()) toast(error, 6000);
        });
    });
}
void download_current() {
    music::Track track;
    { std::lock_guard<std::mutex> lock(g_mu); track = g_as.now_track; }
    if (!track.variants.empty()) start_download(std::move(track), g_ui.settings);
    else toast("这一首没有可下载的源");
}
void request_current_lyrics(bool force) {
    music::Track track;
    { std::lock_guard<std::mutex> lock(g_mu); track = g_as.now_track; }
    if (!track.variants.empty()) request_lyrics(track, force);
}
void open_data_directory(const std::string& path) {
    if (!platform::open_folder(path)) toast("无法打开目录，请检查目录是否可用");
}
std::shared_ptr<const cover::Image> cover_image(const std::string& url) { return g_covers.get(url); }
} // namespace player
