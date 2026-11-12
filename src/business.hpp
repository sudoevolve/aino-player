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
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

// 业务：搜索、播放、登录、下载、歌单、持久化与后台状态；不依赖 aino。
// UI 可编辑交互状态，业务结果通过只读快照展示；不暴露锁或播放设备。
namespace player {
enum class PlayState { Idle, Resolving, Buffering, Ready, Playing, Paused, Failed };
enum class ClearAction { None, Cookies, Library, Queue, Cache };
#ifndef AINO_PLAYER_VERSION
#define AINO_PLAYER_VERSION "0.0.0"
#endif
inline constexpr const char* kVersion = AINO_PLAYER_VERSION;
enum class UpdateStatus { Idle, Checking, Current, Available, Failed, NoRelease };
struct UpdateView {
    UpdateStatus status = UpdateStatus::Idle;
    std::string version, release_url, asset_url, message;
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
            url == prefix + name) { view.asset_url = url; break; }
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
    std::vector<music::Track> tracks;
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
    int sel = -1;
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
    // 账号登录态（cookie 存在 settings 里，uin/uid 单独存）
    std::string qq_uin;
    std::string ne_uid;
    // 歌单页当前看的是哪个账号（0=网易云 1=QQ）
    int pl_site = 2;
    std::string playlist_link;
    std::string local_name;
    bool importing_playlist = false;
    bool storage_settings = false;
    bool update_settings = false;
    ClearAction clear_action = ClearAction::None;
    // 发现页当前看的是哪个平台（0=网易云 1=QQ 2=酷我）
    int disc_site = 0;
    // 队列：播完最后一首要不要回到第一首
    bool loop_list = true;
    // 下载
    std::string download_dir;    // 空 = 用默认 %USERPROFILE%\Music\aino player
    // 提示条：底栏不再放长错误文本，一次性消息统一走 toast
    std::string toast;
};

struct DlItem {
    int percent = 0;
    bool done = false;
    bool failed = false;
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
struct ViewModel {
    UiState& ui;
    const Snapshot& playback;
    const library::Library& library;
    const QqSnap& qq;
    const NeSnap& netease;
    const DiscSnap& discovery;
    const std::map<std::string, DlItem>& downloads;
    const std::array<LoginView, 2>& login;
    const LyricView& lyrics;
    const StoragePaths& paths;
    const UpdateView& update;
    int sync_site, sync_index;
    unsigned long long toast_until;
};
inline std::string track_uid(const music::Track& track) {
    const auto& variant = track.current();
    return std::string(music::source_key(variant.source)) + "#" + variant.id;
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
struct HostCallbacks { std::function<void()> wake; std::function<void()> activate; };
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
void download_current();
void request_current_lyrics();
void open_data_directory(const std::string& path);
std::shared_ptr<const cover::Image> cover_image(const std::string& url);
void start_search();
void start_play(int index);
void toggle_pause();
void play_step(int delta);
void set_volume(float v);
void toast(const std::string& text, int ms = 3600);
void start_download(music::Track track, music::Settings st);
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
void library_fetch(library::Playlist playlist);
void library_import_link();
void library_save_remote(int site, int index);
void begin_login(int site);
void clear_data(ClearAction action);
void check_updates(bool notify = true);
void open_update(bool download);
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
    char buf[4096];
    bool has_version = false;
    while (std::fgets(buf, sizeof buf, f)) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::string v = read_line_value(line, "volume");
        if (!v.empty()) state.volume = float(std::atof(v.c_str()));
        v = read_line_value(line, "sources");
        if (!v.empty()) {
            const uint32_t m = uint32_t(std::strtoul(v.c_str(), nullptr, 10)) & 0x7u;
            state.src_mask = m ? m : 0x3u;   // 全关等于搜不出东西，退回默认的网易云 + QQ
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
        v = read_line_value(line, "embed_cover");
        if (!v.empty()) state.settings.embed_cover = (v != "0");
        v = read_line_value(line, "embed_lyrics");
        if (!v.empty()) state.settings.embed_lyrics = (v != "0");
        v = read_line_value(line, "loop_list");
        if (!v.empty()) state.loop_list = (v != "0");
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
    std::fprintf(f, "download_dir=%s\n", state.download_dir.c_str());
    std::fprintf(f, "qq_uin=%s\n", state.qq_uin.c_str());
    std::fprintf(f, "netease_uid=%s\n", state.ne_uid.c_str());
    std::fprintf(f, "dark=%d\n", state.dark ? 1 : 0);
    std::fprintf(f, "quality=%d\n", state.settings.quality);
    std::fprintf(f, "embed_cover=%d\n", state.settings.embed_cover ? 1 : 0);
    std::fprintf(f, "embed_lyrics=%d\n", state.settings.embed_lyrics ? 1 : 0);
    std::fprintf(f, "loop_list=%d\n", state.loop_list ? 1 : 0);
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
    std::vector<music::Track> tracks;
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
};

static AsyncState g_as;
static std::mutex g_mu;
static std::atomic<bool> g_shutdown{false};
static std::atomic<int> g_search_gen{0};
// 界面希望的刷新节奏（帧/秒）：0 = 不用动，30 = 有进度要跟，60 = 歌词在滚动
static std::atomic<int> g_fps_hint{0};
static std::atomic<int> g_frame_ms_x10{100};

static std::string g_temp_dir;
static std::shared_mutex g_play_cache_mutex;
static HostCallbacks g_host;
static HostCallbacks* g_win = nullptr;

// UI 自己的状态：只有主线程碰，不需要锁

static UiState g_ui;
static UpdateView g_update_view;
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
static library::Library g_library;
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
void play_step(int delta);
void set_volume(float v);
static bool save_config() { return config::save(g_ui); }

static void do_search(HostCallbacks* win, std::string keyword, music::Settings st, int gen) {
    music::SearchResult r = music::run_search(keyword, st, 12, 9000);
    if (g_shutdown.load()) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (gen != g_search_gen.load()) return;  // 又搜了别的词，这次结果作废
        g_as.tracks = r.tracks;
        g_as.reports = std::move(r.reports);
        g_as.elapsed_ms = r.elapsed_ms;
        g_as.searching = false;
        ++g_as.tracks_version;
    }
    queue_mark_dirty();   // 列表被搜索结果覆盖了，落盘跟着更新
    win->wake();

    // 封面单独一轮：要额外打网易云 song/detail 和酷我 pic.web（最慢的那部分），
    // 不能让它挡住列表 —— 列表先出，封面到货后补一次版本号让界面重画。
    music::fill_covers(r.tracks, st);
    if (g_shutdown.load()) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (gen != g_search_gen.load()) return;
        if (g_as.tracks.size() == r.tracks.size()) {
            for (size_t i = 0; i < r.tracks.size(); ++i) g_as.tracks[i].cover_url = r.tracks[i].cover_url;
            ++g_as.tracks_version;
        }
    }
    win->wake();
}

void start_search() {
    std::string keyword;
    music::Settings st;
    int gen = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_as.searching) return;
        if (g_ui.query.empty()) return;
        keyword = g_ui.query;
        st = g_ui.settings;
        for (int i = 0; i < music::kSourceCount; ++i) st.enable[i] = (g_ui.src_mask >> i) & 1u;
        g_as.searching = true;
        gen = ++g_search_gen;
    }
    g_ui.scroll_to_top = true;
    background::start(do_search, g_win, keyword, st, gen);
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
    if (n >= 3 && std::memcmp(h, "ID3", 3) == 0) return "mp3";
    if (n >= 2 && h[0] == 0xFF && (h[1] & 0xE0) == 0xE0) return "mp3";
    return "mp3";
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
    g_as.error = is_preview ? "该曲目三个源都只给试听片段，放的是最长的一段" : std::string();
    win->wake();
}

// 取流 + 下载；失败沿其它源换源（跨源自动换源）
//
static void do_play(HostCallbacks* win, music::Track track, music::Settings st, int gen, int index) {
    std::shared_lock<std::shared_mutex> cache_write(g_play_cache_mutex);
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
        if (index < 0 || index >= int(g_as.tracks.size())) return;
        music::Track& t = g_as.tracks[size_t(index)];
        for (const auto& x : t.variants)
            if (x.source == v.source && x.id == v.id) return;
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
            probe.total_bytes > 0 && expected > 0 && probe.total_bytes * 5 < expected * 3;  // < 60%
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

        const std::string path = g_temp_dir + "track-" + std::to_string(gen) + ".mp3";
        std::atomic<bool> cancel{false};
        auto last_wake = std::chrono::steady_clock::now();
        std::string err;
        const bool ok = net::download(
            rr.url, path, rr.headers,
            [&](long long got, long long total) {
                std::lock_guard<std::mutex> lk(g_mu);
                if (gen != g_as.gen) return;
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
        if (!audio::prepare_file(final_path, playable, err)) {
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
    if (!track.title.empty()) {
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
                    if (gen != g_as.gen) return;
                    g_as.cached_bytes = got;
                    g_as.buffer = total > 0 ? float(double(got) / double(total)) : 0.0f;
                },
                cancel, err)) {
            std::string playable;
            if (audio::prepare_file(path, playable, err)) {
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

static void dl_set(const std::string& uid, int percent, int state, const std::string& err) {
    // state: 0 进行中 / 1 完成 / 2 失败
    std::lock_guard<std::mutex> lk(g_dlm);
    auto it = g_dls.find(uid);
    if (it == g_dls.end()) return;
    if (percent >= 0) it->second.percent = percent;
    it->second.done = (state == 1);
    it->second.failed = (state == 2);
    if (!err.empty()) it->second.err = err;
}

// 沿所有源试一遍，第一个"完整曲目"落盘；三源都只有试听片段就把最长的那段存下来并标注
void start_download(music::Track track, music::Settings st) {
    const std::string uid = track_uid(track);
    if (uid.empty()) return;

    {
        std::lock_guard<std::mutex> lk(g_dlm);
        auto it = g_dls.find(uid);
        if (it != g_dls.end() && !it->second.failed) {
            // 已完成 / 正在下载：都不该再起一个线程。失败过的允许重试。
            if (it->second.done) toast("这首已经下载过了：" + it->second.name);
            else toast("正在下载：" + track.title);
            return;
        }
        DlItem item;
        item.name = sanitize_filename(track.artist.empty() ? track.title : track.artist + " - " + track.title) + ".mp3";
        g_dls[uid] = item;
    }

    const std::string dir = effective_download_dir();
    make_dirs(dir);
    toast("开始下载：" + track.title);

    background::start([track, st, uid, dir] {
        std::string name;
        {
            std::lock_guard<std::mutex> lk(g_dlm);
            auto it = g_dls.find(uid);
            if (it == g_dls.end()) return;
            name = it->second.name;
        }
        // 同名不同曲目：加序号，别静默覆盖用户已有的文件
        std::string path = dir + "/" + name;
        const std::string stem = name.size() > 4 ? name.substr(0, name.size() - 4) : name;
        for (int k = 2; k < 200; ++k) {
            if (!platform::exists(path)) break;
            path = dir + "/" + stem + " (" + std::to_string(k) + ").mp3";
        }
        const std::string part = path + ".part";

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
                    probe.total_bytes > 0 && expected > 0 && probe.total_bytes * 5 < expected * 3;
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
            std::atomic<bool> cancel{false};
            std::string err;
            auto last = std::chrono::steady_clock::now();
            const bool ok = net::download(
                rr.url, part, rr.headers,
                [&](long long got, long long total) {
                    if (total <= 0) return;
                    const auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() < 120) return;
                    last = now;
                    dl_set(uid, int(double(got) / double(total) * 100.0), 0, std::string());
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
            if (got_bytes > 0 && expected2 > 0 && got_bytes * 5 < expected2 * 3) {
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
            if (g_shutdown.load()) return;
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
            std::atomic<bool> cancel{false};
            std::string err;
            if (net::download(best_url, part, best_headers,
                              [&](long long got, long long total) {
                                  if (total <= 0) return;
                                  dl_set(uid, int(double(got) / double(total) * 100.0), 0, std::string());
                                  if (g_win) g_win->wake();
                              },
                              cancel, err, 60000)) {
                saved = true;
                saved_preview = true;
            }
        }

        if (g_shutdown.load()) return;
        if (!saved) {
            platform::remove(part);
            const std::string why = reasons.empty() ? "没有可用音源" : reasons;
            dl_set(uid, 0, 2, why);
            if (g_win) {
                // 失败原因必须露出来：行内只有一个警告图标，不说清就只能靠猜
                toast("下载失败：" + why, 7000);
                g_win->wake();
            }
            return;
        }
        // 只有试听片段时在文件名里标出来：过几天翻目录，光看名字没法分辨整曲和 11 秒片段
        std::string final_path = path;
        if (saved_preview) {
            // 标记必须留着（重名时加序号，而不是把 "(试听)" 丢掉）
            const std::string base = dir + "/" + stem + " (试听)";
            std::string cand = base + ".mp3";
            for (int k = 2; k < 200 && platform::exists(cand); ++k)
                cand = base + " (" + std::to_string(k) + ").mp3";
            final_path = cand;
        }
        // 扩展名跟着真实容器走：m4a / flac 存成 .mp3 的话，双击时系统的
        // 字节流处理器会按扩展名挑解码器，结果就是"文件在但打不开"。
        {
            const std::string ext = sniff_ext(part);
            if (ext != "mp3" && final_path.size() > 4 && final_path.compare(final_path.size() - 4, 4, ".mp3") == 0)
                final_path = final_path.substr(0, final_path.size() - 4) + "." + ext;
        }
        if (!platform::replace_file(part, final_path)) {
            dl_set(uid, 0, 2, "重命名失败（目标目录被占用？）");
            if (g_win) g_win->wake();
            return;
        }
        // 内嵌封面 / 歌词：先拿素材，再写标签。写失败不影响"文件已经下好了"这个事实，
        // 只额外提示一句，别把一次成功的下载报成失败。
        std::string tag_note;
        if (st.embed_cover || st.embed_lyrics) {
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
            if (st.embed_lyrics) {
                const lyr::Doc d = lyr::fetch(track, st);
                if (!d.lines.empty()) lrc_text = lyr::to_lrc(d);
            }
            std::string terr;
            if (!tag::embed(final_path, cover_bytes, is_png, lrc_text, terr) && !terr.empty())
                tag_note = "（内嵌失败：" + terr + "）";
        }

        dl_set(uid, 100, 1, std::string());
        if (g_win) {
            std::string shown = final_path;
            const size_t slash = shown.find_last_of("\\/");
            if (slash != std::string::npos) shown = shown.substr(slash + 1);
            toast((saved_preview ? "只拿到试听片段，已另存为：" : "已下载：") + shown + tag_note, 5000);
            g_win->wake();
        }
    });
}

// =============================================================================
//  四、QQ 音乐（登录 / 歌单 / 同步）
// =============================================================================

static void qq_snapshot() {
    std::lock_guard<std::mutex> lk(g_qq.m);
    g_qq_snap.logged = g_qq.logged;
    g_qq_snap.loading = g_qq.loading;
    g_qq_snap.nickname = g_qq.nickname;
    g_qq_snap.err = g_qq.err;
    g_qq_snap.playlists = g_qq.playlists;
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
            g_qq.err.clear();
        }
        if (g_win) g_win->wake();
    });
}

// 打开歌单 = 把歌直接灌进主列表，播放 / 下载 / 歌词全都复用现有那套
void qq_open_playlist(int idx) {
    std::string cookie, uin, dirid, name;
    {
        std::lock_guard<std::mutex> lk(g_qq.m);
        if (idx < 0 || idx >= int(g_qq.playlists.size())) return;
        cookie = g_qq.cookie;
        uin = g_qq.uin;
        dirid = g_qq.playlists[size_t(idx)].dirid;
        name = g_qq.playlists[size_t(idx)].name;
    }
    toast("正在打开歌单：" + name);

    const int gen = ++g_search_gen;
    { std::lock_guard<std::mutex> lock(g_mu); g_as.searching = false; }
    background::start([cookie, uin, dirid, name, gen] {
        std::string err;
        auto tracks = qq::fetch_playlist(cookie, dirid, uin, err);
        if (g_shutdown.load()) return;
        post_ui([tracks = std::move(tracks), err, name, gen]() mutable {
            if (gen != g_search_gen.load()) return;
            if (!err.empty()) { toast("打开歌单失败：" + err, 8000); return; }
            {
                std::lock_guard<std::mutex> lk(g_mu);
                g_as.tracks = std::move(tracks);
                g_as.reports.clear();
                g_as.elapsed_ms = 0;
                g_as.searching = false;
                ++g_as.tracks_version;
            }
            g_ui.query = name;
            g_ui.tab = 0;
            queue_mark_dirty();
        });
    });
}

// 把当前列表里属于某个源的歌加进指定歌单。
// site：0 = 网易云，1 = QQ —— 两个站点的写入接口完全不同，但外层流程一模一样
// （取目标 → 挑同源的 id → 后台写 → toast 回显），所以合并成一个函数。
void playlist_sync_to(int site, int idx) {
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
    std::lock_guard<std::mutex> lk(g_ne.m);
    g_ne_snap.logged = g_ne.logged;
    g_ne_snap.loading = g_ne.loading;
    g_ne_snap.nickname = g_ne.nickname;
    g_ne_snap.err = g_ne.err;
    g_ne_snap.uid = g_ne.uid;
    g_ne_snap.playlists = g_ne.playlists;
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
            g_ne.err.clear();
        }
        if (g_win) g_win->wake();
    });
}

void ne_open_playlist(int idx) {
    std::string cookie, pid, name;
    {
        std::lock_guard<std::mutex> lk(g_ne.m);
        if (idx < 0 || idx >= int(g_ne.playlists.size())) return;
        cookie = g_ne.cookie;
        pid = g_ne.playlists[size_t(idx)].id;
        name = g_ne.playlists[size_t(idx)].name;
    }
    toast("正在打开歌单：" + name);

    const int gen = ++g_search_gen;
    { std::lock_guard<std::mutex> lock(g_mu); g_as.searching = false; }
    background::start([cookie, pid, name, gen] {
        std::string err;
        auto tracks = ne::fetch_playlist(cookie, pid, err);
        if (g_shutdown.load()) return;
        post_ui([tracks = std::move(tracks), err, name, gen]() mutable {
            if (gen != g_search_gen.load()) return;
            if (!err.empty()) { toast("打开歌单失败：" + err, 8000); return; }
            {
                std::lock_guard<std::mutex> lk(g_mu);
                g_as.tracks = std::move(tracks);
                g_as.reports.clear();
                g_as.elapsed_ms = 0;
                g_as.searching = false;
                ++g_as.tracks_version;
            }
            g_ui.query = name;
            g_ui.tab = 0;
            queue_mark_dirty();
        });
    });
}

// ---- 队列操作（全局函数：后台线程与 UI 都会调）----
void queue_remove(int idx) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (idx < 0 || idx >= int(g_as.tracks.size())) return;
        g_as.tracks.erase(g_as.tracks.begin() + idx);
        if (g_as.playing > idx) g_as.playing--;   // 正在播的那首位置也跟着挪
        ++g_as.tracks_version;
    }
    queue_mark_dirty();
}

void queue_clear() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_as.tracks.clear();
        g_as.playing = -1;
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
                    if (a.source == b.source && a.id == b.id) {
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
    if (site < 0 || site >= music::kSourceCount) return;
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
    disc::Card card;
    {
        std::lock_guard<std::mutex> lk(g_disc.m);
        if (idx < 0 || idx >= int(g_disc.cards.size())) return;
        card = g_disc.cards[size_t(idx)];
    }
    toast("正在打开：" + card.name);
    const auto st = g_ui.settings;
    const auto uin = g_ui.qq_uin;
    const int gen = ++g_search_gen;
    { std::lock_guard<std::mutex> lock(g_mu); g_as.searching = false; }
    background::start([card, st, uin, gen] {
        std::string err;
        std::vector<music::Track> tracks;
        if (card.source == music::Source::Netease)
            tracks = ne::fetch_playlist(st.netease_cookie, card.id, err);
        else
            tracks = qq::fetch_playlist(st.qq_cookie, card.id, uin, err, 15000, /*plaza=*/true);
        if (g_shutdown.load()) return;
        post_ui([tracks = std::move(tracks), err, card, gen]() mutable {
            if (gen != g_search_gen.load()) return;
            if (!err.empty() || tracks.empty()) {
                toast("打开失败：" + (err.empty() ? std::string("歌单里没有能播的歌") : err), 8000);
                return;
            }
            const size_t n = tracks.size();
            {
                std::lock_guard<std::mutex> lk(g_mu);
                g_as.tracks = std::move(tracks);
                g_as.reports.clear();
                g_as.elapsed_ms = 0;
                g_as.searching = false;
                ++g_as.tracks_version;
            }
            g_ui.query = card.name;
            g_ui.tab = 0;
            queue_mark_dirty();
            toast("已载入「" + card.name + "」 " + std::to_string(n) + " 首", 4000);
        });
    });
}

// =============================================================================
//  播放队列持久化
// =============================================================================
// 队列就是主列表本身（g_as.tracks）—— 不另开一份数据，否则「搜索覆盖列表」
// 和「队列里删一首」会变成两套索引，播放位置、换源、下载全要跟着改。
// 文件格式：每行一首，制表符分隔，字段全转义（标题里真出现过制表符）。
static std::string queue_path() {
    const auto dir = platform::config_dir();
    return dir.empty() ? std::string() : platform::join(dir, "queue.txt");
}

static std::string q_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    return out;
}

static bool queue_save() {
    static std::mutex file_mutex;
    std::lock_guard<std::mutex> file_lock(file_mutex);
    const std::string path = queue_path();
    if (path.empty()) return false;
    std::vector<music::Track> copy;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        copy = g_as.tracks;
    }
    if (copy.size() > 800) copy.resize(800);
    const auto temporary = path + ".tmp";
    std::FILE* f = platform::fopen(temporary, "wb");
    if (!f) return false;
    std::fprintf(f, "# aino queue v1\n");
    for (const auto& t : copy) {
        if (t.variants.empty()) continue;
        const music::Variant& v = t.variants[size_t(std::min<int>(t.chosen, int(t.variants.size()) - 1))];
        std::fprintf(f, "%d\t%s\t%s\t%s\t%s\t%s\t%d\t%s\n", int(v.source), q_escape(v.id).c_str(),
                     q_escape(v.native_id).c_str(), q_escape(t.title).c_str(), q_escape(t.artist).c_str(),
                     q_escape(t.album).c_str(), t.duration_ms, q_escape(t.cover_url).c_str());
    }
    const bool ok = !std::ferror(f);
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
    const int mine = ++g_queue_gen;
    g_queue_dirty.store(true);
    background::start([mine] {
        platform::sleep_ms(1200);
        if (g_queue_gen.load() != mine) return;  // 期间又改了，让最后一次负责写
        if (!g_queue_dirty.exchange(false)) return;
        queue_save();
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
    std::FILE* f = platform::fopen(path, "rb");
    if (!f) return;
    std::vector<music::Track> tracks;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, f)) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const auto f8 = split_tab(line);
        if (f8.size() < 8) continue;
        music::Variant v;
        v.source = music::Source(std::atoi(f8[0].c_str()));
        v.id = f8[1];
        v.native_id = f8[2];
        if (v.id.empty()) continue;
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
    std::fclose(f);
    if (tracks.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_as.tracks = std::move(tracks);
        g_as.reports.clear();
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
        gen = ++g_as.gen;
        g_as.playing = index;
        g_as.ps = PlayState::Resolving;
        g_as.error.clear();
        g_as.buffer = 0.0f;
        g_as.now_uid = track_uid(track);
        g_as.now_track = track;
        g_as.now_cover = track.cover_url;
        g_as.now_title = track.title;
        g_as.now_artist = track.artist;
        g_as.now_source = int(track.current().source);
        g_as.now_quality = "解析中";
        g_as.cached_bytes = 0;
        g_as.pending_path.clear();
    }
    g_ui.sel = index;
    g_ui.pos_ms = 0.0f;
    g_ui.dur_ms = float(track.duration_ms);
    g_player.close();  // 播放设备只由主线程操作。
    cleanup_temp(gen);
    if (g_ui.lyrics_open) request_lyrics(track);
    background::start(do_play, g_win, track, st, gen, index);
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
}

void play_step(int delta) {
    int cur = -1, count = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        count = int(g_as.tracks.size());
        for (int i = 0; i < count; ++i) {
            if (track_uid(g_as.tracks[size_t(i)]) == g_as.now_uid) { cur = i; break; }
        }
        if (cur < 0) cur = g_as.playing;
    }
    if (count == 0) return;
    int next = cur < 0 ? 0 : cur + delta;
    if (next < 0) {                       // 上一首：到头了看循环开关
        if (!g_ui.loop_list) next = 0;
        else next = count - 1;
    }
    if (next >= count) {                  // 下一首：不循环就停在最后一首
        if (!g_ui.loop_list) return;
        next = 0;
    }
    start_play(next);
}

void set_volume(float v) {
    g_ui.volume = std::min(100.0f, std::max(0.0f, v));
    g_player.set_volume(int(g_ui.volume));
    g_ui.config_dirty = true;
}

// 主线程每帧的"抄快照"
static Snapshot take_snapshot(Snapshot& cached) {
    Snapshot snap;
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
    snap.tracks_version = g_as.tracks_version;
    snap.reports = g_as.reports;  // 很小，直接拷
    // 曲目列表只在新检索后变，按版本号缓存，避免每帧拷几十条字符串
    if (cached.tracks_version == snap.tracks_version) snap.tracks = cached.tracks;
    else snap.tracks = g_as.tracks;
    return snap;
}

// =============================================================================
//  五、界面
// =============================================================================

// 资料库由 UI 线程独占；网络任务只返回完整结果，持久化成功后再替换内存状态。
static bool library_commit(library::Library next) {
    if (!g_library_writable) { toast("资料库加载失败，已保护原文件；请先修复 library.json", 7000); return false; }
    std::string error;
    if (!next.save(library_path(), error)) { toast(error, 7000); return false; }
    g_library = std::move(next);
    return true;
}
void favorite_toggle(const music::Track& track) {
    if (track.variants.empty()) return;
    auto next = g_library;
    const bool added = next.toggle(track);
    if (library_commit(std::move(next))) toast(added ? "已收藏：" + track.title : "已取消收藏：" + track.title, 2600);
}
void library_open(size_t index) {
    if (index >= g_library.playlists.size()) return;
    const auto& playlist = g_library.playlists[index];
    if (playlist.tracks.empty()) { toast("歌单为空，点击歌曲旁的收藏按钮添加音乐"); return; }
    ++g_search_gen; // 未完成的旧搜索不能覆盖本地列表。
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_as.tracks = playlist.tracks;
        g_as.playing = -1;
        for (int i = 0; i < int(g_as.tracks.size()); ++i)
            if (track_uid(g_as.tracks[size_t(i)]) == g_as.now_uid) { g_as.playing = i; break; }
        g_as.reports.clear(); g_as.elapsed_ms = 0; g_as.searching = false; ++g_as.tracks_version;
    }
    g_ui.query = playlist.name; g_ui.tab = 0; g_ui.scroll_to_top = true;
    queue_mark_dirty();
}
void library_save_current() {
    library::Playlist playlist;
    playlist.name = music::text::trim(g_ui.local_name);
    if (playlist.name.empty()) { toast("请先输入本地歌单名称"); return; }
    { std::lock_guard<std::mutex> lock(g_mu); playlist.tracks = g_as.tracks; }
    if (playlist.tracks.empty()) { toast("当前列表为空，请先搜索或打开歌单"); return; }
    playlist.key = "local#" + std::to_string(platform::ticks());
    auto next = g_library;
    next.upsert(std::move(playlist));
    if (library_commit(std::move(next))) { g_ui.local_name.clear(); toast("当前列表已保存为本地歌单"); }
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
        auto tracks = playlist.source == music::Source::Netease ?
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
            auto next = g_library;
            next.upsert(std::move(playlist));
            if (library_commit(std::move(next))) { g_ui.playlist_link.clear(); toast("在线歌单已保存到本地，曲目快照可随时打开"); }
        });
    });
}
void library_import_link() {
    library::Playlist playlist;
    if (!library::parse_link(g_ui.playlist_link, playlist.source, playlist.remote_id)) {
        toast("请粘贴网易云或 QQ 的完整公开歌单链接（短链接请先在浏览器打开）", 6500); return;
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
    if (g_wait_login_site.load() >= 0) { toast("已有登录连接正在进行，请先完成或取消"); return; }
    g_wait_login_site.store(site);
    login::start(site == 1 ? login::Site::QQ : login::Site::NetEase);
}

void clear_data(ClearAction action) {
    if (action == ClearAction::Cookies) {
        g_wait_login_site.store(-1);
        login::forget(login::Site::QQ); login::forget(login::Site::NetEase);
        {
            std::lock_guard<std::mutex> lock(g_qq.m);
            g_qq.cookie.clear(); g_qq.uin.clear(); g_qq.nickname.clear(); g_qq.playlists.clear();
            g_qq.err.clear(); g_qq.logged = false; g_qq.loading = false;
        }
        {
            std::lock_guard<std::mutex> lock(g_ne.m);
            g_ne.cookie.clear(); g_ne.uid.clear(); g_ne.nickname.clear(); g_ne.playlists.clear();
            g_ne.err.clear(); g_ne.logged = false; g_ne.loading = false;
        }
        g_ui.settings.qq_cookie.clear(); g_ui.settings.netease_cookie.clear();
        g_ui.qq_uin.clear(); g_ui.ne_uid.clear();
        g_ui.config_dirty = true;
        toast(save_config() ? "已清除两个账号的登录信息；浏览器中的登录不受影响" : "账号已退出，但配置文件保存失败，请检查配置目录权限", 6500);
    } else if (action == ClearAction::Library) {
        library::Library empty;
        std::string error;
        if (!empty.save(library_path(), error)) { toast(error, 7000); return; }
        g_library = std::move(empty); g_library_writable = true; ++g_library_epoch;
        g_ui.importing_playlist = false; g_ui.playlist_link.clear(); g_ui.local_name.clear();
        toast("已清除本地收藏、保存的歌单及在线快照");
    } else if (action == ClearAction::Queue) {
        ++g_search_gen;
        { std::lock_guard<std::mutex> lock(g_mu); g_as.searching = false; g_as.reports.clear(); }
        queue_clear();
        toast(queue_save() ? "已清空当前列表；正在播放的歌曲可继续播放" : "列表已清空，但保存失败，请检查配置目录权限", 6500);
    } else if (action == ClearAction::Cache) {
        std::unique_lock<std::shared_mutex> cache_write(g_play_cache_mutex, std::try_to_lock);
        std::string keep;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (g_as.ps == PlayState::Playing || g_as.ps == PlayState::Paused || g_as.ps == PlayState::Ready)
                keep = "track-" + std::to_string(g_as.gen) + ".";
        }
        const auto result = cache::clear_audio(platform::cache_root(), platform::process_id(), keep, cache_write.owns_lock());
        g_covers.clear();
        toast("已清理 " + std::to_string(result.files) + " 个音频缓存（" + fmt_size((long long)result.bytes) + "）及封面缓存" +
            (result.kept ? "；使用中的文件已保留" : "") + (result.errors ? "；部分文件未能删除" : ""), 7500);
    }
}

void check_updates(bool notify) {
    if (g_update_view.status == UpdateStatus::Checking) return;
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
    const auto url = download && !g_update_view.asset_url.empty() ? g_update_view.asset_url :
        !g_update_view.release_url.empty() ? g_update_view.release_url : std::string(updates::kReleases);
    if (!platform::open_browser(url)) toast("无法打开默认浏览器，请检查浏览器设置");
}

static Snapshot g_snapshot;
static std::array<LoginView, 2> g_login_view;
static LyricView g_lyric_view;
static StoragePaths g_paths;
ViewModel view_model() {
    return {g_ui, g_snapshot, g_library, g_qq_snap, g_ne_snap, g_disc_snap, g_dls_snap,
        g_login_view, g_lyric_view, g_paths, g_update_view, g_sync_site.load(), g_sync_index.load(), g_toast_until.load()};
}
bool initialize(bool smoke_test) {
    if (!smoke_test) config::load(g_ui);
    std::string library_error;
    if (!smoke_test) g_library_writable = g_library.load(library_path(), library_error);
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
        unsigned long long toast_seen = 0;
        while (!g_shutdown.load()) {
            bool busy = false;
            {
                std::lock_guard<std::mutex> lk(g_mu);
                busy = g_wait_login_site.load() >= 0 || login::state(login::Site::QQ).return_requested.load() ||
                       login::state(login::Site::NetEase).return_requested.load() || g_as.searching || g_as.ps == PlayState::Playing || g_as.ps == PlayState::Buffering ||
                       g_as.ps == PlayState::Resolving;
            }
            const unsigned long long now = platform::ticks();
            const unsigned long long until = g_toast_until.load();
            if (now < until) {
                busy = true;
                toast_seen = until;
            } else if (toast_seen != 0) {
                toast_seen = 0;
                if (g_win) g_win->wake();   // toast 刚过期，补一帧把它擦掉
            }
            if (busy && g_win) g_win->wake();
            // 11fps 的刷新率下，歌词滚动和进度条都是"一格一格跳"的 —— 这就是卡顿感的来源。
            // 歌词面板开着时给 60fps，只放歌给 30fps，其余时间照旧睡觉（0 帧 0% CPU）。
            const int fps = g_fps_hint.load();
            // 目标周期减去本帧已经花掉的时间：不然 30fps 实际只有 1000/(33+14) ≈ 21fps
            const int spent = g_frame_ms_x10.load() / 10;
            int wait = 250;
            if (fps >= 60) wait = 16 - spent;
            else if (fps > 0) wait = 33 - spent;
            else if (busy) wait = 90;
            if (wait < 2) wait = 2;
            platform::sleep_ms(wait);
        }
    });

}
void tick() {
        drain_ui_mail();
        g_snapshot = take_snapshot(g_snapshot);
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
            const login::Site ls = (site == 1) ? login::Site::QQ : login::Site::NetEase;
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
            if (g_player.open(snap.pending_path) && g_player.play()) {
                g_player.set_volume(int(g_ui.volume));
                g_ui.dur_ms = float(g_player.duration_ms());
                if (g_ui.dur_ms <= 0) g_ui.dur_ms = float(snap.pending_duration_ms);
                g_ui.pos_ms = 0.0f;
                std::lock_guard<std::mutex> lk(g_mu);
                if (g_as.gen == snap.gen) g_as.ps = PlayState::Playing;
                snap.ps = PlayState::Playing;
            } else {
                std::lock_guard<std::mutex> lk(g_mu);
                if (g_as.gen == snap.gen) {
                    g_as.ps = PlayState::Failed;
                    g_as.error = g_player.error();
                }
                snap.ps = PlayState::Failed;
            }
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
            if (p > 0) g_ui.pos_ms = float(p);
            const long long d = g_player.duration_ms();
            if (d > 0) g_ui.dur_ms = float(d);
            // 放完了自动下一首
            if (snap.ps == PlayState::Playing && g_player.is_ended()) {
                play_step(1);
            }
        }

        for (int site = 0; site < 2; ++site) {
            auto& state = login::state(site == 1 ? login::Site::QQ : login::Site::NetEase);
            g_login_view[size_t(site)].connecting = state.running.load() && !state.done.load();
            std::lock_guard<std::mutex> lock(state.m);
            g_login_view[size_t(site)].note = state.note;
        }
        {
            std::lock_guard<std::mutex> lock(g_lyr.m);
            g_lyric_view = {g_lyr.doc, g_lyr.for_uid, g_lyr.loading};
        }
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
                fps = g_ui.lyrics_open ? 60 : 30;
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
    login::shutdown();
    background::stop();
    g_player.close();
    if (!smoke_test) {
        save_config();
        queue_save(); // 后台任务全部结束后保存最终队列。
    }
    g_win = nullptr;
}
void cancel_login(int site) { login::cancel(site == 1 ? login::Site::QQ : login::Site::NetEase); }
void seek_playback(int milliseconds) { g_player.seek_ms(milliseconds); }
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
    if (!index || index >= g_library.playlists.size()) return;
    auto next = g_library;
    next.playlists.erase(next.playlists.begin() + index);
    if (library_commit(std::move(next))) toast("已从本机移除歌单");
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
