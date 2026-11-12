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
enum class UpdateStatus { Idle, Checking, Current, Available, Downloading, Installing, Failed, NoRelease };
struct UpdateView {
    UpdateStatus status = UpdateStatus::Idle;
    std::string version, release_url, asset_url, message, digest;
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
    view.status = latest > installed ? UpdateStatus::Available : UpdateStatus::Current;
    view.message = latest > installed ? "发现新版，可下载后更新" : "当前已是最新正式版";
    const auto prefix = std::string(kReleases) + "/download/" + tag + "/";
    for (const auto& asset : release["assets"].array) {
        const auto name = asset["name"].str(), url = asset["browser_download_url"].str();
        if (!suffix.empty() && name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0 &&
            name.rfind("aino-player", 0) == 0 && name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == std::string::npos &&
            url == prefix + name) {
            const bool installer = name == "aino-player-" + tag + "-setup-windows-x64.exe";
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
    std::string now_uid, now_cover, now_title, now_artist, now_quality, now_short, error;
    music::Track now_track;
    int now_source = -1;
    float buffer = 0.0f;
    long long cached_bytes = 0;
    std::string pending_path;
    long long pending_duration_ms = 0;
    int gen = 0;
};

struct UiState {
    std::string query = "周杰伦";
    music::Settings settings;
    uint32_t src_mask = 0x3;   // 默认只开网易云 + QQ（酷我受限曲目只给 11 秒试听）
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
    // 歌词面板（Apple Music 那种右侧抽屉）
    bool lyrics_open = false;
    bool queue_open = false;
    // 账号登录态（cookie 存在 settings 里，uin/uid 单独存）
    std::string qq_uin;
    std::string ne_uid;
    // 歌单页当前看的是哪个账号（0=网易云 1=QQ）
    int pl_site = 2;
    std::string playlist_link;
    std::string local_name;
    std::string youtube_playlist_name;
    std::string local_folder;
    bool scan_recursive = true;
    std::string playlist_filter, playlist_rename, library_transfer_path;
    int playlist_sort = 0;
    bool playlist_manage = false;
    std::set<std::string> playlist_selected;
    bool importing_playlist = false;
    int settings_tab = 0;
    bool update_settings = false;
    ClearAction clear_action = ClearAction::None;
    // 发现页当前看的是哪个平台（0=网易云 1=QQ 2=酷我）
    int disc_site = 0;
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
struct YouTubeSnap {
    bool logged = false, loading = false, writing = false;
    std::string nickname, err;
    std::vector<music::YouTubePlaylist> playlists;
};

struct DiscSnap {
    bool loading = false;
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
};
struct ViewModel {
    UiState& ui;
    const Snapshot& playback;
    const library::Library& library;
    const QqSnap& qq;
    const NeSnap& netease;
    const YouTubeSnap& youtube;
    const DiscSnap& discovery;
    const std::map<std::string, DlItem>& downloads;
    const std::array<LoginView, 3>& login;
    const LyricView& lyrics;
    const StoragePaths& paths;
    const UpdateView& update;
    const PlaylistView& playlist;
    int sync_site, sync_index;
    unsigned long long toast_until;
    std::shared_ptr<const library::Library> library_storage;
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
struct HostCallbacks { std::function<void()> wake; std::function<void()> activate; std::function<void()> close; std::function<void*()> native_window; };
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
void playlist_filter_rows();
void playlist_rename_local();
void playlist_remove_selected();
void library_export(bool playlist_only);
void library_restore();
void queue_move(int from, int to);
void download_current();
void request_current_lyrics();
void open_data_directory(const std::string& path);
std::shared_ptr<const cover::Image> cover_image(const std::string& url);
void start_search();
void search_more();
void retry_search_source(int source);
void start_play(int index);
void toggle_pause();
void play_step(int delta, bool automatic = false);
void set_play_mode(PlayMode mode);
void set_sleep_timer(int minutes, bool after_track = false);
void queue_play_next(const music::Track& track);
void set_volume(float v);
void toast(const std::string& text, int ms = 3600);
void start_download(music::Track track, music::Settings st);
void cancel_download(const std::string& uid);
void qq_refresh();
void ne_refresh();
void qq_open_playlist(int idx);
void ne_open_playlist(int idx);
void playlist_sync_to(int site, int idx);
bool queue_push(const music::Track& t);
void queue_clear();
void queue_remove(int idx);
void disc_refresh(int site);
void disc_open(int idx);
void favorite_toggle(const music::Track& track);
void library_open(size_t index);
void library_save_current();
void youtube_refresh();
void youtube_open_playlist(int index);
void youtube_create_playlist();
void youtube_sync_queue(int index);
void playlist_open_remote(library::Playlist playlist);
void bilibili_open_parts(const music::Track& track);
void playlist_close();
void playlist_play(int index);
void playlist_choose_source(int index, int variant, bool replay);
void play_search_result(int index);
void choose_search_source(int index, int variant, bool replay);
void playlist_save_view();
void library_import_folder(const std::string& folder, bool recursive);
void library_fetch(library::Playlist playlist);
void library_import_link();
void library_save_remote(int site, int index);
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
            const uint32_t m = uint32_t(std::strtoul(v.c_str(), nullptr, 10)) & ((1u << music::kSourceCount) - 1);
            state.src_mask = m ? m : 0x3u;   // 全关等于搜不出东西，退回默认的网易云 + QQ
        }
        v = read_line_value(line, "netease_cookie");
        if (!v.empty()) state.settings.netease_cookie = v;
        v = read_line_value(line, "qq_cookie");
        if (!v.empty()) state.settings.qq_cookie = v;
        v = read_line_value(line, "youtube_cookie");
        if (!v.empty()) state.settings.youtube_cookie = v;
        v = read_line_value(line, "youtube_authuser");
        if (!v.empty() && v.size() <= 3 && v.find_first_not_of("0123456789") == std::string::npos) state.settings.youtube_authuser = v;
        v = read_line_value(line, "youtube_delegated");
        if (!v.empty()) state.settings.youtube_delegated_session = v;
        v = read_line_value(line, "youtube_visitor");
        if (!v.empty()) state.settings.youtube_visitor = v;
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

    // 老配置里三个源都是开的。这一版把酷我改成默认关（受限曲目只给 11 秒试听，
    // 留着它会让人以为"歌手没唱完"），所以迁移一次：只做一次，之后用户自己开就保留。
    if (!has_version) {
        if (state.src_mask & (1u << 2)) {
            state.src_mask &= ~(1u << 2);
            state.config_dirty = true;
        }
        state.config_dirty = true;   // 让新版本号落盘
    }
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
    std::fprintf(f, "youtube_cookie=%s\nyoutube_authuser=%s\nyoutube_delegated=%s\nyoutube_visitor=%s\n",
        state.settings.youtube_cookie.c_str(), state.settings.youtube_authuser.c_str(),
        state.settings.youtube_delegated_session.c_str(), state.settings.youtube_visitor.c_str());
    std::fprintf(f, "download_dir=%s\n", state.download_dir.c_str());
    std::fprintf(f, "qq_uin=%s\n", state.qq_uin.c_str());
    std::fprintf(f, "netease_uid=%s\n", state.ne_uid.c_str());
    std::fprintf(f, "dark=%d\n", state.dark ? 1 : 0);
    std::fprintf(f, "quality=%d\n", state.settings.quality);
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
static YouTubeSnap g_youtube;
static unsigned g_youtube_generation = 0;
static login::Site login_site(int site) { return site == 2 ? login::Site::YouTube : site == 1 ? login::Site::QQ : login::Site::NetEase; }
static UpdateView g_update_view;
static std::atomic<bool> g_update_cancel{false};
static PlaylistView g_playlist_view;
static unsigned g_playlist_generation = 0, g_library_revision = 0;
static std::atomic<bool> g_folder_cancel{false};
static bool g_folder_selecting = false;
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
    std::mutex m;
    std::string for_uid;
    std::shared_ptr<const lyr::Doc> doc;
    bool loading = false;
};
static LyricState g_lyr;

// 下载表：uid → 状态。主线程每帧拷一份快照（几十条小结构，开销可忽略）

static std::mutex g_dlm;
static std::map<std::string, DlItem> g_dls;
static std::map<std::string, std::shared_ptr<std::atomic<bool>>> g_download_cancel;
static std::deque<std::function<void()>> g_download_pending;
static int g_download_workers = 0;
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
    if (music::text::trim(g_ui.query).empty()) return;
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

// 按文件头判容器：QQ 给的是 m4a（ftypmp42），酷我/网易云是 mp3。
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
    for (bool enabled : st.enable) key += enabled ? '1' : '0';
    for (const auto* value : {&st.netease_cookie, &st.qq_cookie, &st.youtube_cookie,
         &st.youtube_authuser, &st.youtube_delegated_session, &st.youtube_visitor}) key += library::quote(*value);
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
            const auto resolved = music::resolve(variant, st);
            if (!resolved.ok || st.cancelled()) continue;
            const auto probe = net::probe_audio(resolved.url, resolved.headers, 6000);
            // 预加载不缓存试听片段；需要换源或试听兜底时仍走完整点播流程。
            if (int(variant.source) <= int(music::Source::Kuwo) && probe.total_bytes > 0 && variant.duration_ms > 0 &&
                probe.total_bytes * 5 < variant.duration_ms * 16LL * 3) continue;
            if (st.cancelled()) return {};
            media->original = base + ".mp3"; media->owns_original = true;
            if (!net::download(resolved.url, media->original, resolved.headers,
                [&](long long, long long) {}, request->cancelled, error) || st.cancelled()) continue;
            const auto extension = sniff_ext(media->original);
            if (extension != "mp3") {
                const auto renamed = base + "." + extension;
                if (!platform::replace_file(media->original, renamed)) continue;
                media->original = renamed;
            }
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
    g_as.now_quality = quality;
    g_as.now_short = sbuf;
    g_as.pending_duration_ms = v.duration_ms;
    g_as.buffer = 1.0f;
    g_as.ps = PlayState::Ready;
    g_as.error = is_preview ? "该曲目可用音源只给试听片段，放的是最长的一段" : std::string();
    win->wake();
}

static void finish_stream(HostCallbacks* win, const music::Variant& variant, const audio::StreamSpec& spec,
                          std::shared_ptr<audio::Stream> stream, int gen) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_shutdown.load() || gen != g_as.gen) return;
    g_pending_stream = std::move(stream); g_active_stream_spec = spec;
    g_as.pending_path = "stream:"; g_as.pending_duration_ms = spec.duration_ms;
    g_as.now_source = int(variant.source); g_as.now_quality = std::string(music::source_name(variant.source)) + " · 流式解码";
    g_as.now_short = "流式"; g_as.buffer = 1; g_as.ps = PlayState::Ready; g_as.error.clear();
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
        g_as.ps = PlayState::Failed; g_as.now_quality = "本地文件不可用";
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

    // 一个候选的完整尝试：取流 → 探体积（识别酷我 11 秒片段）→ 下载 → 交给播放器。
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

        const music::ResolveResult rr = music::resolve(v, st);
        if (!rr.ok) {
            add_reason(std::string(music::source_name(v.source)) + " " + rr.reason);
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (gen != g_as.gen) return true;
            g_as.now_source = int(v.source);
            g_as.now_quality = rr.detail.empty() ? std::string("取流中") : rr.detail;
        }

        // 探体积：只取 2 字节，代价极小
        const net::AudioProbe probe = net::probe_audio(rr.url, rr.headers, 6000);
        const long long expected = v.duration_ms > 0 ? v.duration_ms * 16LL : 0;  // ms × 128kbps/8/1000
        const bool preview =
            int(v.source) <= int(music::Source::Kuwo) && probe.total_bytes > 0 && expected > 0 && probe.total_bytes * 5 < expected * 3;  // < 60%
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
            if (stream) { finish_stream(win, v, spec, std::move(stream), gen); return true; }
            if (st.cancelled()) return true;
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
        if (!audio::prepare_file(final_path, playable, err, {}, st.cancelled)) {
            add_reason(err);
            return false;
        }
        finish_play(win, v, final_path, gen, /*is_preview=*/false, playable);
        return true;
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
    g_as.now_quality = "不可用";
    g_as.error = reasons.empty() ? "没有可用音源"
                                 : ("取流失败 — " + reasons + "（可在「音源」页填 Cookie，或换一行）");
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
static void request_lyrics(const music::Track& track) {
    const std::string uid = track_uid(track);
    if (uid.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_lyr.m);
        if (g_lyr.for_uid == uid && (g_lyr.doc || g_lyr.loading)) return;
        g_lyr.for_uid = uid;
        g_lyr.doc.reset();
        g_lyr.loading = true;
    }
    music::Track t = track;
    music::Settings st = g_ui.settings;
    background::start([t, st, uid] {
        lyr::Doc d = lyr::fetch(t, st);
        if (g_shutdown.load()) return;
        auto shared = std::make_shared<const lyr::Doc>(std::move(d));
        {
            std::lock_guard<std::mutex> lk(g_lyr.m);
            if (g_lyr.for_uid != uid) return;  // 已经切到别的歌了，这份丢掉
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
    if (!item.done && !item.publishing) { item.failed = true; item.err = "已取消"; }
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
                it->second.failed = true; it->second.err = "已取消";
                toast("已取消下载：" + track.title);
            }
            return;
        }
        if (g_download_pending.size() >= 32) { toast("下载等待队列已满，请稍后再添加"); return; }
        DlItem item;
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
            name = it->second.name;
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
            const music::ResolveResult rr = music::resolve(v, st);
            if (!rr.ok) {
                if (!reasons.empty()) reasons += "；";
                reasons += std::string(music::source_name(v.source)) + " " + rr.reason;
                return false;
            }
            if (!allow_preview) {
                const net::AudioProbe probe = net::probe_audio(rr.url, rr.headers, 8000);
                const long long expected = v.duration_ms > 0 ? v.duration_ms * 16LL : 0;
                const bool preview =
                    int(v.source) <= int(music::Source::Kuwo) && probe.total_bytes > 0 && expected > 0 && probe.total_bytes * 5 < expected * 3;
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
                if (!reasons.empty()) reasons += "；";
                reasons += std::string(music::source_name(v.source)) + " 下载失败（" + err + "）";
                return false;
            }
            // 落盘后按**实际字节数**再复核一次：有些 CDN 用 chunked 编码不给
            // Content-Length，下载前的探测拿不到 total，会漏判成"完整曲目"，
            // 结果用户下到的是一个 11 秒片段还以为是整首。
            const long long got_bytes = file_size_of(part);
            const long long expected2 = (v.duration_ms > 0 ? v.duration_ms : track.duration_ms) * 16LL;
            if (int(v.source) <= int(music::Source::Kuwo) && got_bytes > 0 && expected2 > 0 && got_bytes * 5 < expected2 * 3) {
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
        };

        for (const auto& v : track.variants) {
            if (st.cancelled()) return;
            if (try_one(v, false)) {
                saved = true;
                break;
            }
        }
        // 兜底 2：这条目在网易云上没有对应（常见于 QQ/酷我独有），而酷我对受限曲目
        // 只给 11 秒片段、QQ 匿名直接拒。按「歌名 + 歌手」去网易云翻一遍，命中就下完整曲。
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
            g_dls[uid].publishing = true;
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
            item.publishing = false;
            if (!final_path.empty()) {
                item.name = final_name; item.err.clear();
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
    library::Playlist playlist;
    {
        std::lock_guard<std::mutex> lock(g_qq.m);
        if (idx < 0 || idx >= int(g_qq.playlists.size())) return;
        const auto& p = g_qq.playlists[size_t(idx)];
        playlist.name = p.name; playlist.cover = p.cover; playlist.remote_id = p.dirid;
        playlist.remote_owner = g_qq.uin; playlist.source = music::Source::QQ;
    }
    playlist_open_remote(std::move(playlist));
}

// 把当前列表里属于某个源的歌加进指定歌单。
// site：0 = 网易云，1 = QQ —— 两个站点的写入接口完全不同，但外层流程一模一样
// （取目标 → 挑同源的 id → 后台写 → toast 回显），所以合并成一个函数。
void playlist_sync_to(int site, int idx) {
    if (site == 3) { youtube_sync_queue(idx); return; }
    std::string cookie, uid, pid, name;
    if (site == 1) {
        std::lock_guard<std::mutex> lk(g_qq.m);
        if (idx < 0 || idx >= int(g_qq.playlists.size())) return;
        cookie = g_qq.cookie;
        uid = g_qq.uin;
        pid = g_qq.playlists[size_t(idx)].dirid;
        name = g_qq.playlists[size_t(idx)].name;
    } else {
        std::lock_guard<std::mutex> lk(g_ne.m);
        if (idx < 0 || idx >= int(g_ne.playlists.size())) return;
        cookie = g_ne.cookie;
        uid = g_ne.uid;
        pid = g_ne.playlists[size_t(idx)].id;
        name = g_ne.playlists[size_t(idx)].name;
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
    library::Playlist playlist;
    {
        std::lock_guard<std::mutex> lock(g_ne.m);
        if (idx < 0 || idx >= int(g_ne.playlists.size())) return;
        const auto& p = g_ne.playlists[size_t(idx)];
        playlist.name = p.name; playlist.cover = p.cover; playlist.remote_id = p.id;
        playlist.source = music::Source::Netease;
    }
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
// 三家接口的可用性不一样，这里如实呈现：网易云 / QQ 有热搜也有热门歌单，
// 酷我只有热搜（web 端歌单接口要 kw_token + csrf 双校验，匿名一律拒绝）。
struct DiscState {
    std::mutex m;
    bool loading = false;
    int site = 0;
    int version = 0;   // 数据变了才 ++，界面按它决定要不要重新拷贝
    std::vector<disc::Card> cards;
    disc::Heats heats;
    std::string err;
};
static DiscState g_disc;

// 主线程绘制用的快照（同样不持锁画界面）

static DiscSnap g_disc_snap;

void disc_refresh(int site) {
    if (site < 0 || site >= 3) return;
    {
        std::lock_guard<std::mutex> lk(g_disc.m);
        if (g_disc.loading && g_disc.site == site) return;  // 同一个平台别重复打请求
        g_disc.loading = true;
        g_disc.site = site;
        g_disc.err.clear();
        g_disc.cards.clear();
        g_disc.heats = disc::Heats{};
    }
    background::start([site] {
        disc::Heats h = disc::heats(music::Source(site));
        std::string err;
        auto cards = disc::playlists(music::Source(site), 30, err);
        if (g_shutdown.load()) return;
        {
            std::lock_guard<std::mutex> lk(g_disc.m);
            if (g_disc.site != site) return;  // 期间又切了平台，这份结果作废
            g_disc.heats = std::move(h);
            g_disc.cards = std::move(cards);
            g_disc.err = err;
            g_disc.loading = false;
            ++g_disc.version;
        }
        if (g_win) g_win->wake();
    });
}

static void disc_snapshot() {
    std::lock_guard<std::mutex> lk(g_disc.m);
    g_disc_snap.loading = g_disc.loading;
    g_disc_snap.site = g_disc.site;
    // 卡片和热词只在刷新完成时变，按版本号拷贝 —— 否则每帧几十条字符串白白复制一遍
    if (g_disc_snap.version != g_disc.version) {
        g_disc_snap.version = g_disc.version;
        g_disc_snap.cards = g_disc.cards;
        g_disc_snap.heats = g_disc.heats;
        g_disc_snap.err = g_disc.err;
    } else if (g_disc_snap.err.empty() != g_disc.err.empty()) {
        g_disc_snap.err = g_disc.err;
    }
}

static void queue_mark_dirty();

// 打开发现页里的歌单：热门歌单 / 官方榜都是公开歌单，不依赖登录态
void disc_open(int idx) {
    library::Playlist playlist;
    {
        std::lock_guard<std::mutex> lock(g_disc.m);
        if (idx < 0 || idx >= int(g_disc.cards.size())) return;
        const auto& p = g_disc.cards[size_t(idx)];
        playlist.name = p.name; playlist.cover = p.cover; playlist.remote_id = p.id; playlist.source = p.source;
    }
    playlist_open_remote(std::move(playlist));
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
    std::string encoded;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_queue_writable) return false;
        encoded = "{\"version\":2,\"current\":" + library::quote(g_as.now_uid) +
            ",\"position_ms\":" + std::to_string(g_as.position_ms) + ",\"tracks\":[";
        bool first = true;
        for (const auto& track : g_as.tracks) {
            if (track.variants.empty() || track.variants.size() > 12) return false;
            if (!first) encoded += ',';
            first = false;
            encoded += library::encode_track(track);
            if (encoded.size() > kQueueFileLimit - 2) return false;
        }
        encoded += "]}";
    }
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
// 改动后延时 1.2 秒落盘：连续删十首只写一次文件。
// 不能只靠主循环里的判断 —— 界面静止时根本不出帧（0 帧 0% CPU 的代价），
// 那时候删完一首就再也不保存了。所以这里自己起一个短命线程。
static void queue_mark_dirty() {
    { std::lock_guard<std::mutex> lock(g_mu); g_queue_writable = true; }
    const int mine = ++g_queue_gen;
    g_queue_dirty.store(true);
    background::start([mine] {
        platform::sleep_ms(1200);
        if (g_queue_gen.load() != mine) return;  // 期间又改了，让最后一次负责写
        if (!g_queue_dirty.exchange(false)) return;
        if (!queue_save()) toast("播放队列保存失败，原文件已保留；请检查目录权限或队列是否超过 64 MiB", 7000);
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

void start_play(int index) {
    music::Track track;
    music::Settings st;
    int gen = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (index < 0 || index >= int(g_as.tracks.size())) return;
        track = g_as.tracks[size_t(index)];
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
        finish_play(g_win, g_current_media->variant, g_current_media->original, gen, false, g_current_media->playable);
        return;
    }
    background::start(do_play, g_win, track, st, gen, true);
}

void toggle_pause() {
    PlayState ps;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        ps = g_as.ps;
        if (ps == PlayState::Playing) g_as.ps = PlayState::Paused;
        else if (ps == PlayState::Paused) g_as.ps = PlayState::Playing;
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
static int next_queue_index(int delta, bool automatic, bool consume = true) {
    std::lock_guard<std::mutex> lock(g_mu);
    const int count = int(g_as.tracks.size());
    if (!count) return -1;
    int cur = -1;
    for (int i = 0; i < count; ++i)
        if (track_uid(g_as.tracks[size_t(i)]) == g_as.now_uid) { cur = i; break; }
    if (cur < 0) cur = g_as.playing;
    if (delta > 0) {
        for (auto it = g_play_next.begin(); it != g_play_next.end(); ++it) {
            const auto uid = *it;
            for (int i = 0; i < count; ++i)
                if (track_uid(g_as.tracks[size_t(i)]) == uid) {
                    if (consume) g_play_next.erase(g_play_next.begin(), std::next(it));
                    return i;
                }
        }
        if (consume) g_play_next.clear();
    }
    if (automatic && g_ui.play_mode == PlayMode::RepeatOne && cur >= 0) return cur;
    if (g_ui.play_mode == PlayMode::Shuffle) {
        if (delta < 0) {
            while (g_shuffle_cursor > 0) {
                const auto& uid = g_shuffle_history[size_t(--g_shuffle_cursor)];
                for (int i = 0; i < count; ++i) if (track_uid(g_as.tracks[size_t(i)]) == uid) return i;
            }
            return cur < 0 ? 0 : cur;
        }
        int cursor = g_shuffle_cursor;
        while (cursor + 1 < int(g_shuffle_history.size())) {
            const auto& uid = g_shuffle_history[size_t(++cursor)];
            for (int i = 0; i < count; ++i) if (track_uid(g_as.tracks[size_t(i)]) == uid) {
                if (consume) g_shuffle_cursor = cursor;
                return i;
            }
        }
        if (g_shuffle_version != g_as.tracks_version) {
            g_shuffle_remaining.clear(); g_shuffle_version = g_as.tracks_version;
        }
        if (g_shuffle_remaining.empty()) {
            for (int i = 0; i < count; ++i) if (i != cur) g_shuffle_remaining.push_back(track_uid(g_as.tracks[size_t(i)]));
            std::shuffle(g_shuffle_remaining.begin(), g_shuffle_remaining.end(), g_shuffle_random);
        }
        if (g_shuffle_remaining.empty()) return 0;
        const auto uid = g_shuffle_remaining.back();
        if (consume) g_shuffle_remaining.pop_back();
        for (int i = 0; i < count; ++i) if (track_uid(g_as.tracks[size_t(i)]) == uid) return i;
    }
    int next = cur < 0 ? 0 : cur + delta;
    if (next < 0) next = g_ui.play_mode == PlayMode::Sequential ? 0 : count - 1;
    if (next >= count) return g_ui.play_mode == PlayMode::Sequential ? -1 : 0;
    return next;
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
    snap.now_uid = g_as.now_uid;
    snap.now_track = g_as.now_track;
    snap.now_cover = g_as.now_cover;
    snap.now_title = g_as.now_title;
    snap.now_artist = g_as.now_artist;
    snap.now_quality = g_as.now_quality;
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
void playlist_filter_rows() {
    auto& view = g_playlist_view;
    view.rows.clear();
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
void playlist_remove_selected() {
    if (!g_playlist_view.saved || g_ui.playlist_selected.empty()) return;
    const auto selected = g_ui.playlist_selected; const auto key = g_playlist_view.playlist.key;
    library_edit([selected, key](library::Library& next) {
        for (auto& p : next.playlists) if (p.key == key) p.tracks.erase(std::remove_if(p.tracks.begin(), p.tracks.end(),
            [&](const auto& t) { return selected.count(track_uid(t)) != 0; }), p.tracks.end());
    }, [] { g_ui.playlist_selected.clear(); toast("已从本地歌单移除所选曲目，原音乐文件保留"); });
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
void library_open(size_t index) {
    if (index >= g_library->playlists.size()) return;
    ++g_playlist_generation;
    g_playlist_view = {};
    g_playlist_view.open = true; g_playlist_view.saved = true;
    g_playlist_view.playlist = g_library->playlists[index];
    g_playlist_view.library_revision = g_library_revision;
    g_ui.tab = 3; g_ui.scroll_to_top = true;
    g_ui.playlist_filter.clear(); g_ui.playlist_rename = g_playlist_view.playlist.name;
    g_ui.playlist_manage = false; g_ui.playlist_selected.clear(); g_ui.playlist_sort = 0;
    playlist_filter_rows();
}
void playlist_close() {
    ++g_playlist_generation;
    const auto tab = g_playlist_view.return_tab;
    g_playlist_view = {};
    if (g_ui.tab == 3) g_ui.tab = tab;
}
void playlist_open_remote(library::Playlist playlist) {
    const auto generation = ++g_playlist_generation;
    g_playlist_view = {};
    g_playlist_view.return_tab = g_ui.tab;
    g_playlist_view.open = true; g_playlist_view.loading = true;
    playlist.key = std::string(music::source_key(playlist.source)) + "#" +
        (playlist.remote_owner.empty() ? "" : playlist.remote_owner + "#") + playlist.remote_id;
    g_playlist_view.playlist = playlist; g_ui.tab = 3;
    g_ui.playlist_manage = false; g_ui.playlist_filter.clear(); g_ui.playlist_selected.clear(); g_ui.playlist_sort = 0;
    const auto settings = g_ui.settings;
    const auto uin = g_ui.qq_uin;
    background::start([playlist = std::move(playlist), settings, uin, generation]() mutable {
        std::string error;
        if (playlist.source == music::Source::Bilibili) {
            auto parts = music::bilibili_parts(playlist.remote_id, error);
            playlist.tracks = std::move(parts.tracks);
            if (!parts.title.empty()) playlist.name = parts.title;
            if (!parts.cover.empty()) playlist.cover = parts.cover;
        } else playlist.tracks = playlist.source == music::Source::YouTube ? music::youtube_playlist_tracks(settings, playlist.remote_id, error) : playlist.source == music::Source::Netease ?
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
    const auto generation = g_playlist_generation;
    library_edit([playlist = g_playlist_view.playlist](library::Library& next) { next.upsert(playlist); }, [generation] {
        if (generation == g_playlist_generation) g_playlist_view.saved = true;
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
        std::string error;
        ne::Playlist ne_metadata;
        qq::Playlist qq_metadata;
        music::YouTubePlaylist youtube_metadata;
        std::vector<music::Track> tracks;
        if (playlist.source == music::Source::Bilibili) {
            auto parts = music::bilibili_parts(playlist.remote_id, error);
            tracks = std::move(parts.tracks);
            if (!parts.title.empty()) playlist.name = parts.title;
            if (!parts.cover.empty()) playlist.cover = parts.cover;
        } else tracks = playlist.source == music::Source::YouTube ? music::youtube_playlist_tracks(settings, playlist.remote_id, error, &youtube_metadata) : playlist.source == music::Source::Netease ?
            ne::fetch_playlist(settings.netease_cookie, playlist.remote_id, error, 15000, &ne_metadata) :
            qq::fetch_playlist(settings.qq_cookie, playlist.remote_id, uin, error, 15000, playlist.remote_owner.empty(), &qq_metadata);
        const auto name = playlist.source == music::Source::YouTube ? youtube_metadata.name : playlist.source == music::Source::Netease ? ne_metadata.name : qq_metadata.name;
        const auto cover = playlist.source == music::Source::YouTube ? youtube_metadata.cover : playlist.source == music::Source::Netease ? ne_metadata.cover : qq_metadata.cover;
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
        toast("请粘贴网易云、QQ 或 YouTube 的完整歌单链接（短链接请先在浏览器打开）", 6500); return;
    }
    playlist.key = std::string(music::source_key(playlist.source)) + "#" + playlist.remote_id;
    playlist.name = std::string(music::source_name(playlist.source)) + "歌单 " + playlist.remote_id;
    library_fetch(std::move(playlist));
}
void library_save_remote(int site, int index) {
    library::Playlist playlist;
    if (site == 3) {
        if (index < 0 || index >= int(g_youtube.playlists.size())) return;
        const auto& p = g_youtube.playlists[size_t(index)];
        playlist.source = music::Source::YouTube; playlist.remote_id = p.id;
        playlist.name = p.name; playlist.cover = p.cover;
    } else if (site == 1) {
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

void youtube_refresh() {
    if (g_youtube.loading || g_youtube.writing || g_ui.settings.youtube_cookie.empty()) return;
    const auto generation = ++g_youtube_generation;
    const auto settings = g_ui.settings;
    g_youtube.logged = true; g_youtube.loading = true; g_youtube.err.clear();
    background::start([settings, generation] {
        std::string name, error;
        std::vector<music::YouTubePlaylist> playlists;
        if (music::youtube_profile(settings, name, error)) playlists = music::youtube_playlists(settings, error);
        post_ui([name, error, playlists = std::move(playlists), generation]() mutable {
            if (generation != g_youtube_generation) return;
            g_youtube.loading = false; g_youtube.err = error;
            if (error.empty()) { g_youtube.nickname = name; g_youtube.playlists = std::move(playlists); }
        });
    });
}
void youtube_open_playlist(int index) {
    if (index < 0 || index >= int(g_youtube.playlists.size())) return;
    const auto& item = g_youtube.playlists[size_t(index)];
    library::Playlist playlist; playlist.source = music::Source::YouTube;
    playlist.name = item.name; playlist.cover = item.cover; playlist.remote_id = item.id;
    playlist_open_remote(std::move(playlist));
}
void youtube_create_playlist() {
    const auto name = music::text::trim(g_ui.youtube_playlist_name);
    if (g_youtube.writing || g_youtube.loading || name.empty()) { toast("请先输入歌单名称，并等待当前操作完成"); return; }
    if (g_ui.settings.youtube_cookie.empty()) { toast("请先登录 YouTube Music"); return; }
    const auto settings = g_ui.settings;
    const auto generation = g_youtube_generation;
    g_youtube.writing = true;
    background::start([settings, generation, name] {
        std::string error;
        const auto id = music::youtube_create_playlist(settings, name, error);
        post_ui([id, error, generation] {
            if (generation != g_youtube_generation) return;
            g_youtube.writing = false;
            if (id.empty()) { toast("创建 YouTube 歌单失败：" + error, 8000); return; }
            g_ui.youtube_playlist_name.clear(); toast("YouTube 私人歌单已创建"); youtube_refresh();
        });
    });
}
void youtube_sync_queue(int index) {
    if (g_youtube.writing || g_youtube.loading || index < 0 || index >= int(g_youtube.playlists.size())) return;
    std::vector<std::string> ids; std::set<std::string> seen;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (const auto& track : g_as.tracks) for (const auto& variant : track.variants) {
            if (variant.source == music::Source::YouTube && seen.insert(variant.id).second) { ids.push_back(variant.id); break; }
        }
    }
    if (ids.empty()) { toast("播放队列中没有 YouTube 曲目，请先点播或加入队列", 6000); return; }
    const auto playlist = g_youtube.playlists[size_t(index)];
    const auto settings = g_ui.settings; const auto generation = g_youtube_generation;
    g_youtube.writing = true;
    background::start([settings, playlist, ids, generation] {
        std::string error;
        const bool ok = music::youtube_add_to_playlist(settings, playlist.id, ids, error);
        post_ui([ok, error, generation, count = ids.size()] {
            if (generation != g_youtube_generation) return;
            g_youtube.writing = false;
            toast(ok ? "已向 YouTube 歌单添加 " + std::to_string(count) + " 首歌曲" : "添加失败（已成功的批次可能已写入）：" + error, 8000);
            if (ok) youtube_refresh();
        });
    });
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
        login::forget(login::Site::QQ); login::forget(login::Site::NetEase); login::forget(login::Site::YouTube);
        ++g_youtube_generation; g_youtube = {};
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
        g_ui.settings.youtube_cookie.clear(); g_ui.settings.youtube_delegated_session.clear();
        g_ui.settings.youtube_visitor.clear(); g_ui.settings.youtube_authuser = "0";
        g_ui.qq_uin.clear(); g_ui.ne_uid.clear();
        g_ui.config_dirty = true;
        toast(save_config() ? "已清除三个账号的登录信息；浏览器中的登录不受影响" : "账号已退出，但配置文件保存失败，请检查配置目录权限", 6500);
    } else if (action == ClearAction::Library) {
        ++g_library_epoch; // 先拒绝清理前启动的导入，清理动作仍与已有写入串行。
        library_edit([](library::Library& next) { next.reset(); }, [] {
            g_library_writable = true; playlist_close();
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
#ifdef _WIN32
    if (download && g_update_view.installer) {
        if (g_update_view.status != UpdateStatus::Available) return;
        const auto release = g_update_view;
        if (!release.asset_size || release.digest.size() != 71 || release.digest.rfind("sha256:", 0) != 0 ||
            release.digest.substr(7).find_first_not_of("0123456789abcdef") != std::string::npos) {
            toast("安装包缺少有效的 SHA-256 信息，请从 GitHub 发布页下载", 6000); return;
        }
        g_update_cancel.store(false);
        g_update_view.status = UpdateStatus::Downloading; g_update_view.progress = 0;
        g_update_view.message = "正在下载安装包，完成校验后将关闭播放器并升级…";
        background::start([release] {
            const auto tag = release.version.front() == 'v' ? release.version : "v" + release.version;
            const auto path = platform::join(g_temp_dir, "aino-update-" + tag + ".exe");
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
            post_ui([path] {
                if (!g_host.close || !platform::launch_installer(path)) {
                    g_update_view.status = UpdateStatus::Available; g_update_view.message = "无法启动安装器，请重试或从发布页下载";
                    platform::remove(path); return;
                }
                g_update_view.status = UpdateStatus::Installing;
                g_update_view.message = "安装器已启动，正在保存配置并退出…";
                g_host.close();
            });
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
    return {g_ui, g_snapshot, *g_library, g_qq_snap, g_ne_snap, g_youtube, g_disc_snap, g_dls_snap,
        g_login_view, g_lyric_view, g_paths, g_update_view, g_playlist_view, g_sync_site.load(), g_sync_index.load(), g_toast_until.load(), g_library};
}
bool initialize(bool smoke_test) {
    net::shutdown_signal = &g_shutdown;
    if (!smoke_test) config::load(g_ui);
    std::string library_error;
    if (!smoke_test) g_library_writable = g_library->load(library_path(), library_error);
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
    if (!g_ui.settings.youtube_cookie.empty()) youtube_refresh();
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
        unsigned long long toast_seen = 0, timer_seen = 0;
        while (!g_shutdown.load()) {
            bool busy = false;
            {
                std::lock_guard<std::mutex> lk(g_mu);
                busy = g_wait_login_site.load() >= 0 || login::state(login::Site::QQ).return_requested.load() ||
                       login::state(login::Site::NetEase).return_requested.load() || g_as.searching || g_as.ps == PlayState::Playing || g_as.ps == PlayState::Buffering ||
                       g_as.ps == PlayState::Resolving;
            }
            const unsigned long long now = platform::ticks();
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
            // 下载表：几十条小结构，直接整份拷给界面读，省得绘制时持锁
            std::lock_guard<std::mutex> lk(g_dlm);
            g_dls_snap = g_dls;
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
                std::string cookie, uid, note, context;
                {
                    std::lock_guard<std::mutex> lk(st.m);
                    cookie = st.cookie;
                    uid = st.uid;
                    note = st.note;
                    context = st.context;
                }
                if (!cookie.empty()) {
                    if (site == 2) {
                        g_ui.settings.youtube_cookie = cookie;
                        js::Value data;
                        g_ui.settings.youtube_authuser = "0";
                        g_ui.settings.youtube_delegated_session.clear(); g_ui.settings.youtube_visitor.clear();
                        if (js::parse(context, data)) {
                            const auto user = data["authuser"].str();
                            if (!user.empty() && user.size() <= 3 && user.find_first_not_of("0123456789") == std::string::npos) g_ui.settings.youtube_authuser = user;
                            g_ui.settings.youtube_delegated_session = data["delegated"].str();
                            g_ui.settings.youtube_visitor = data["visitor"].str();
                        }
                        g_ui.src_mask |= 1u << int(music::Source::YouTube);
                        g_ui.config_dirty = true; toast("YouTube Music 登录成功，正在读取账号歌单"); youtube_refresh();
                    } else if (site == 1) {
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
        for (auto site : {login::Site::QQ, login::Site::NetEase, login::Site::YouTube})
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

        // ---- 失败要说出来 ----
        // 底栏现在只放"播放中/缓冲 xx%"，长错误全移到 toast；如果不在这里接一手，
        // 取流失败就变成"点了没反应"。
        {
            static std::string last_error;
            if (snap.ps == PlayState::Failed) {
                if (!snap.error.empty() && snap.error != last_error) {
                    last_error = snap.error;
                    toast(snap.error, 9000);
                }
            } else {
                last_error.clear();
            }
        }

        // ---- 每帧同步播放位置 ----
        if (snap.ps == PlayState::Playing || snap.ps == PlayState::Paused) {
            const long long p = g_player.position_ms();
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
        refresh_preload();
}
void bootstrap(bool initial_search, bool smoke_test) {
    if (!smoke_test) queue_load();
    if (initial_search) start_search();
    if (!smoke_test) { disc_refresh(g_ui.disc_site); check_updates(false); }
}
void finish_frame(float frame_ms) {
    const auto& snap = g_snapshot;
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
                // 歌词抽屉展开就一直 60fps：滚动是唯一需要连续动画的地方，
                // 30fps 下无论缓动怎么调都能看出顿感。帧耗时 10ms 上下，撑得住。
                fps = g_ui.lyrics_open && !g_ui.queue_open ? 60 : 10;
            } else if (snap.ps == PlayState::Buffering || snap.ps == PlayState::Resolving) {
                fps = 30;
            }
            g_fps_hint.store(fps);
        }

        g_ui.frame_ms = g_ui.frame_ms * 0.9f + frame_ms * 0.1f;
        g_frame_ms_x10.store(int(g_ui.frame_ms * 10.0f));
}
void shutdown(bool smoke_test) {
    g_shutdown.store(true);
    g_folder_cancel.store(true);
    { std::lock_guard<std::mutex> lock(g_dlm); for (auto& entry : g_download_cancel) entry.second->store(true); g_download_pending.clear(); }
    cancel_preload();
    g_update_cancel.store(true);
    g_covers.clear(); // 退出只等待正在下载的封面，不继续处理积压的队列。
    login::shutdown();
    g_player.close();
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
void request_current_lyrics() {
    music::Track track;
    { std::lock_guard<std::mutex> lock(g_mu); track = g_as.now_track; }
    if (!track.variants.empty()) request_lyrics(track);
}
void open_data_directory(const std::string& path) {
    if (!platform::open_folder(path)) toast("无法打开目录，请检查目录是否可用");
}
std::shared_ptr<const cover::Image> cover_image(const std::string& url) { return g_covers.get(url); }
} // namespace player
