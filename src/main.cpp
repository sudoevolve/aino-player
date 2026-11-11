// main.cpp —— aino player
//
// 界面：aino（单头文件 C++17，macOS 风格）
// 多音源：网易云 / QQ / 酷我 三源并发检索 + 跨源同曲合并
//         （并发扇出 → 指纹合并 → 相关性排序 → 跨源换源）
// 播放：WinMM MCI（系统自带，零第三方依赖）
//
// 线程模型（改之前先读）：
//   * 网络（搜索 / 取流 / 下载）全在后台线程，写状态时短暂加锁，写完 win.wake() 叫醒主循环。
//   * 主线程只画界面、抄后台结果、操作 MCI。**不持锁绘制**：每帧开头把异步区拷一份快照，
//     画的时候只读快照，避开了"后台等锁 / UI 等网络"这种互相拖死。
//   * 不播放、不搜索时，主循环阻塞在 WaitMessage()，0 帧 / 0.00% CPU。

#define AINO_IMPLEMENTATION
#include "aino.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <map>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio.hpp"
#include "cover.hpp"
#include "discover.hpp"
#include "login.h"
#include "lyrics.hpp"
#include "netease_api.hpp"
#include "net.hpp"
#include "qqapi.hpp"
#include "sources.hpp"
#include "tag.hpp"

// =============================================================================
//  一、小工具
// =============================================================================

namespace det {

constexpr float kSidebar = 216.0f;
constexpr float kBarHeight = 88.0f;
constexpr float kLyricsWidth = 320.0f;
constexpr float kRowHeight = 64.0f;

inline aino::Color hue_color(float h, float s, float l) {
    // HSL → RGB：封面渐变只需要好看，不需要色彩管理
    h = std::fmod(h, 360.0f);
    if (h < 0) h += 360.0f;
    const float c = (1.0f - std::fabs(2 * l - 1)) * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    const float m = l - c * 0.5f;
    float r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; }
    else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; }
    else { r = c; b = x; }
    auto to8 = [m](float v) { return uint8_t(std::lround((v + m) * 255.0f)); };
    return aino::Color(to8(r), to8(g), to8(b));
}

inline uint32_t fnv1a(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

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

inline aino::Color source_color(music::Source s) {
    switch (s) {
        case music::Source::Netease: return aino::Color::hex(0xD93A3A);
        case music::Source::QQ: return aino::Color::hex(0x1F9D6B);
        default: return aino::Color::hex(0x3E63DD);
    }
}

// UTF-8 单字符步进
inline size_t utf8_step(const std::string& s, size_t i) {
    if (i >= s.size()) return 1;
    const unsigned char c = (unsigned char)s[i];
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// 按像素宽度截断并补省略号（aino 不做自动省略，宽度得自己量）
//
// 量宽度是整帧里最贵的操作之一（每次都要重走一遍字形 shaping），而列表每帧画的
// 还是同一批字符串 —— 截断本身**从头到尾探前缀**，长中文标题要量十几次。
// 所以这里按 (文本, 最大宽度, 字号, 字重) 记忆化：滚动时帧耗时能砍掉一半。
inline std::string ellipsize_raw(aino::Ui& ui, const std::string& text, float max_w, float size, int weight) {
    if (ui.text_width(text, size, weight) <= max_w) return text;
    const float ell_w = ui.text_width("…", size, weight);
    size_t keep = 0;
    for (size_t i = 0; i < text.size();) {
        const size_t step = utf8_step(text, i);
        if (ui.text_width(text.substr(0, i + step), size, weight) + ell_w > max_w) break;
        keep = i + step;
        i += step;
    }
    return text.substr(0, keep) + "…";
}

// 文本测宽也记忆化：列表每帧对同一批固定字符串（源名、热搜词）反复量宽，
// 每次都要重走 shaping，三十行就是上百次。只有主线程碰，不用加锁。
inline float tw_cached(aino::Ui& ui, const std::string& text, float size, int weight = 400) {
    static std::map<std::string, float> cache;
    char kb[48];
    std::snprintf(kb, sizeof kb, "\x01%.1f\x01%d", size, weight);
    std::string key = text;
    key += kb;
    const auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    const float w = ui.text_width(text, size, weight);
    if (cache.size() > 4000) cache.clear();
    cache.emplace(std::move(key), w);
    return w;
}

inline std::string ellipsize(aino::Ui& ui, const std::string& text, float max_w, float size, int weight = 400) {
    if (text.empty() || max_w <= 0.0f) return std::string();
    static std::map<std::string, std::string> cache;  // 只有主线程碰，不用加锁
    char kb[64];
    std::snprintf(kb, sizeof kb, "\x01%.1f\x01%.1f\x01%d", max_w, size, weight);
    std::string key = text;
    key += kb;
    const auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    std::string out = ellipsize_raw(ui, text, max_w, size, weight);
    if (cache.size() > 1200) cache.clear();  // 换歌单/换搜索结果时自然回收
    cache.emplace(std::move(key), out);
    return out;
}

// 播放三角用横线堆出来，不依赖字体里有没有 U+25B6
inline void draw_play_triangle(aino::Ui& ui, const aino::Rect& box, const aino::Color& c) {
    const int steps = std::max(12, int(box.h * 1.5f));
    const float row_h = box.h / float(steps);
    for (int i = 0; i < steps; ++i) {
        const float t = (float(i) + 0.5f) / float(steps);
        const float k = 1.0f - std::fabs(2.0f * t - 1.0f);
        ui.fill_rect({box.x, box.y + float(i) * row_h, box.w * k, row_h + 0.6f}, c);
    }
}

inline void draw_pause_bars(aino::Ui& ui, const aino::Rect& box, const aino::Color& c) {
    const float bar_w = box.w * 0.3f;
    ui.fill_rect({box.x, box.y, bar_w, box.h}, c, 1.5f);
    ui.fill_rect({box.x + box.w - bar_w, box.y, bar_w, box.h}, c, 1.5f);
}

// 喇叭：箱体 + 锥体 + 两道声波。之前锥体太高、声波太近，13px 下看起来像播放键。
inline void draw_speaker(aino::Ui& ui, const aino::Rect& box, const aino::Color& c) {
    const float h = box.h;
    const float bw = box.w * 0.22f;
    ui.fill_rect({box.x, box.y + h * 0.38f, bw, h * 0.24f}, c, 0.8f);
    const aino::Rect cone{box.x + bw, box.y + h * 0.26f, box.w * 0.24f, h * 0.48f};
    const int steps = std::max(8, int(cone.h));
    const float row_h = cone.h / float(steps);
    for (int i = 0; i < steps; ++i) {
        const float t = (float(i) + 0.5f) / float(steps);
        const float k = 1.0f - std::fabs(2.0f * t - 1.0f);
        ui.fill_rect({cone.x, cone.y + float(i) * row_h, cone.w * k, row_h + 0.5f}, c);
    }
    ui.fill_rect({box.x + box.w * 0.62f, box.y + h * 0.40f, 1.2f, h * 0.20f}, c, 0.6f);
    ui.fill_rect({box.x + box.w * 0.75f, box.y + h * 0.32f, 1.2f, h * 0.36f}, c, 0.6f);
    ui.fill_rect({box.x + box.w * 0.88f, box.y + h * 0.24f, 1.2f, h * 0.52f}, c, 0.6f);
}

// 三根小竖条 = "正在播放"（静态，不做动画，空闲 CPU 才能真是 0）
inline void draw_playing_bars(aino::Ui& ui, const aino::Rect& box, const aino::Color& c) {
    const float w = 2.5f, gap = 2.5f;
    const float hs[3] = {0.5f, 1.0f, 0.72f};
    for (int i = 0; i < 3; ++i) {
        const float h = box.h * hs[i];
        ui.fill_rect({box.x + float(i) * (w + gap), box.y + box.h - h, w, h}, c, 1.2f);
    }
}

}  // namespace det

using namespace det;

// =============================================================================
//  二、状态
// =============================================================================

enum class PlayState { Idle, Resolving, Buffering, Ready, Playing, Paused, Failed };

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

struct Snapshot {
    bool searching = false;
    std::vector<music::Track> tracks;
    std::vector<music::SourceReport> reports;
    int elapsed_ms = 0;
    int tracks_version = -1;
    int playing = -1;
    PlayState ps = PlayState::Idle;
    std::string now_uid, now_cover, now_title, now_artist, now_quality, now_short, error;
    int now_source = -1;
    float buffer = 0.0f;
    long long cached_bytes = 0;
    std::string pending_path;
    long long pending_duration_ms = 0;
    int gen = 0;
};

static AsyncState g_as;
static std::mutex g_mu;
static std::atomic<bool> g_shutdown{false};
static std::atomic<int> g_search_gen{0};
// 界面希望的刷新节奏（帧/秒）：0 = 不用动，30 = 有进度要跟，60 = 歌词在滚动
static std::atomic<int> g_fps_hint{0};
static std::atomic<int> g_frame_ms_x10{100};
// 歌词滚动/换行的动画窗口：只有这段时间需要 60fps，平时 30fps 就够
static std::atomic<unsigned long long> g_anim_until{0};
static std::string g_temp_dir;
static aino::Window* g_win = nullptr;

// UI 自己的状态：只有主线程碰，不需要锁
struct UiState {
    std::string query = "周杰伦";
    music::Settings settings;
    uint32_t src_mask = 0x3;   // 默认只开网易云 + QQ（酷我受限曲目只给 11 秒试听）
    int tab = 0;
    int sel = -1;
    float volume = 70.0f;
    float pos_ms = 0.0f;
    float dur_ms = 0.0f;
    float frame_ms = 0.0f;
    int seek_to = -1;
    int chosen_override_track = -1;  // 用户在药丸上换了源
    int chosen_override_index = -1;
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
    int pl_site = 0;
    // 发现页当前看的是哪个平台（0=网易云 1=QQ 2=酷我）
    int disc_site = 0;
    // 队列：播完最后一首要不要回到第一首
    bool loop_list = true;
    // 下载
    std::string download_dir;    // 空 = 用默认 %USERPROFILE%\Music\aino player
    // 提示条：底栏不再放长错误文本，一次性消息统一走 toast
    std::string toast;
};
static UiState g_ui;
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
struct DlItem {
    int percent = 0;
    bool done = false;
    bool failed = false;
    std::string name;   // 保存的文件名
    std::string err;
};
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
struct QqSnap {
    bool logged = false;
    bool loading = false;
    std::string nickname, err, note;
    std::vector<qq::Playlist> playlists;
};
static QqSnap g_qq_snap;

struct NeState {
    std::mutex m;
    bool logged = false;
    bool loading = false;
    std::string cookie, uid, nickname, err;
    std::vector<ne::Playlist> playlists;
};
static NeState g_ne;

struct NeSnap {
    bool logged = false;
    bool loading = false;
    std::string nickname, err, uid;
    std::vector<ne::Playlist> playlists;
};
static NeSnap g_ne_snap;

// 同步进行中的目标：(站点, 下标)。站点 0=网易云 1=QQ，-1 = 空闲
static std::atomic<int> g_sync_site{-1};
static std::atomic<int> g_sync_index{-1};
static std::atomic<int> g_wait_login_site{-1};
static std::atomic<int> g_goto_tab{-1};

// 前向声明（UI 与动作互相引用）
// 曲目身份：用"源#id"而不是列表下标。列表重排（重新检索、排序变化）后
// 下标会指向别的歌，用 uid 才能把"正在播放"这一行稳稳认出来。
static std::string track_uid(const music::Track& t) {
    const music::Variant& v = t.current();
    return std::string(music::source_key(v.source)) + "#" + v.id;
}

static void start_search();
static void start_play(int index);
// 队列相关（定义在下面的"后台任务"之后，这里先声明给 do_search / 打开歌单用）
static void queue_mark_dirty();
static void queue_save();
static void queue_load();
static void toggle_pause();
static void play_step(int delta);
static void set_volume(float v);

// =============================================================================
//  三、后台任务
// =============================================================================

static void do_search(aino::Window* win, std::string keyword, music::Settings st, int gen) {
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

static void start_search() {
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
    std::thread(do_search, g_win, keyword, st, gen).detach();
}

// 按文件头判容器：QQ 给的是 m4a（ftypmp42），酷我/网易云是 mp3。
// 扩展名要和内容一致 —— Media Foundation 会按扩展名挑 handler，错配会加载失败。
static std::string sniff_ext(const std::string& path) {
    std::FILE* f = _wfopen(net::widen(path).c_str(), L"rb");
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

// 下载完成 → 交给主线程开 MCI
static void finish_play(aino::Window* win, const music::Variant& v, const std::string& path, int gen,
                        bool is_preview) {
    const audio::MediaInfo mi = audio::inspect_mp3(path);
    int kbps = mi.bitrate_kbps;
    if (!mi.valid && v.duration_ms > 0) {
        // m4a 没帧头可读，用 文件大小 / 时长 估平均码率（比"码率未解析"有用）
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(net::widen(path).c_str(), GetFileExInfoStandard, &fad)) {
            const long long sz = (static_cast<long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            kbps = int(double(sz) * 8.0 / double(v.duration_ms));
        }
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
    g_as.pending_path = path;
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
static void do_play(aino::Window* win, music::Track track, music::Settings st, int gen, int index) {
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
        // 扩展名跟着内容走：m4a 存成 .mp3 会让 Media Foundation 选错 handler
        std::string final_path = path;
        if (const std::string ext = sniff_ext(path); ext != "mp3") {
            final_path = g_temp_dir + "track-" + std::to_string(gen) + "." + ext;
            MoveFileExW(net::widen(path).c_str(), net::widen(final_path).c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        finish_play(win, v, final_path, gen, /*is_preview=*/false);
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
            finish_play(win, preview_variant, path, gen, /*is_preview=*/true);
            return;
        }
        add_reason("片段下载失败");
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

static void toast(const std::string& text, int ms = 3600) {
    g_ui.toast = text;
    g_toast_until.store(GetTickCount64() + static_cast<unsigned long long>(ms));
    if (g_win) g_win->wake();
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
    std::thread([t, st, uid] {
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
    }).detach();
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


static long long file_size_of(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(net::widen(path).c_str(), GetFileExInfoStandard, &fad)) return -1;
    return (static_cast<long long>(fad.nFileSizeHigh) << 32) | static_cast<long long>(fad.nFileSizeLow);
}

static std::string default_download_dir() {
    wchar_t buf[MAX_PATH] = {0};
    if (GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH) == 0) return g_temp_dir;
    return net::narrow(buf) + "\\Music\\aino player";
}

static std::string effective_download_dir() {
    return g_ui.download_dir.empty() ? default_download_dir() : g_ui.download_dir;
}

// 逐级建目录：CreateDirectoryW 只建一级，多级路径得自己走一遍
static void make_dirs(const std::string& dir) {
    for (size_t i = 2; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '\\' || dir[i] == '/')
            CreateDirectoryW(net::widen(dir.substr(0, i)).c_str(), nullptr);
    }
}

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
static void start_download(music::Track track, music::Settings st) {
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

    std::thread([track, st, uid, dir] {
        std::string name;
        {
            std::lock_guard<std::mutex> lk(g_dlm);
            auto it = g_dls.find(uid);
            if (it == g_dls.end()) return;
            name = it->second.name;
        }
        // 同名不同曲目：加序号，别静默覆盖用户已有的文件
        std::string path = dir + "\\" + name;
        const std::string stem = name.size() > 4 ? name.substr(0, name.size() - 4) : name;
        for (int k = 2; k < 200; ++k) {
            if (GetFileAttributesW(net::widen(path).c_str()) == INVALID_FILE_ATTRIBUTES) break;
            path = dir + "\\" + stem + " (" + std::to_string(k) + ").mp3";
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
                DeleteFileW(net::widen(part).c_str());
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
            DeleteFileW(net::widen(part).c_str());
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
            const std::string base = dir + "\\" + stem + " (试听)";
            std::string cand = base + ".mp3";
            for (int k = 2; k < 200 && GetFileAttributesW(net::widen(cand).c_str()) != INVALID_FILE_ATTRIBUTES; ++k)
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
        if (!MoveFileExW(net::widen(part).c_str(), net::widen(final_path).c_str(), MOVEFILE_REPLACE_EXISTING)) {
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
    }).detach();
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

static void qq_refresh() {
    std::string cookie, uin;
    {
        std::lock_guard<std::mutex> lk(g_qq.m);
        g_qq.loading = true;
        g_qq.err.clear();
        cookie = g_qq.cookie;
        uin = g_qq.uin;
    }
    if (g_win) g_win->wake();
    std::thread([cookie, uin] {
        qq::Profile p = qq::fetch_profile(cookie, uin);
        if (g_shutdown.load()) return;
        std::lock_guard<std::mutex> lk(g_qq.m);
        g_qq.loading = false;
        if (!p.ok()) {
            g_qq.err = p.error;
        } else {
            g_qq.nickname = p.nickname;
            g_qq.playlists = std::move(p.playlists);
            g_qq.err.clear();
        }
        if (g_win) g_win->wake();
    }).detach();
}

// 打开歌单 = 把歌直接灌进主列表，播放 / 下载 / 歌词全都复用现有那套
static void qq_open_playlist(int idx) {
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

    std::thread([cookie, uin, dirid, name] {
        std::string err;
        auto tracks = qq::fetch_playlist(cookie, dirid, uin, err);
        if (g_shutdown.load()) return;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (!err.empty()) {
                // 失败要回显原因，不然用户只看到列表没变
            } else {
                g_as.tracks = std::move(tracks);
                g_as.reports.clear();
                g_as.elapsed_ms = 0;
                g_as.searching = false;
                ++g_as.tracks_version;
            }
        }
        if (!err.empty()) {
            if (g_win) {
                toast("打开歌单失败：" + err, 8000);
                g_win->wake();
            }
            return;
        }
        g_ui.query = name;
        g_goto_tab.store(0);
        if (g_win) g_win->wake();
    }).detach();
}

// 把当前列表里属于某个源的歌加进指定歌单。
// site：0 = 网易云，1 = QQ —— 两个站点的写入接口完全不同，但外层流程一模一样
// （取目标 → 挑同源的 id → 后台写 → toast 回显），所以合并成一个函数。
static void playlist_sync_to(int site, int idx) {
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

    std::thread([site, cookie, uid, pid, ids, name] {
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
    }).detach();
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

static void ne_refresh() {
    std::string cookie, uid;
    {
        std::lock_guard<std::mutex> lk(g_ne.m);
        g_ne.loading = true;
        g_ne.err.clear();
        cookie = g_ne.cookie;
        uid = g_ne.uid;
    }
    if (g_win) g_win->wake();
    std::thread([cookie, uid] {
        ne::Profile p = ne::fetch_profile(cookie, uid);
        if (g_shutdown.load()) return;
        std::lock_guard<std::mutex> lk(g_ne.m);
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
    }).detach();
}

static void ne_open_playlist(int idx) {
    std::string cookie, pid, name;
    {
        std::lock_guard<std::mutex> lk(g_ne.m);
        if (idx < 0 || idx >= int(g_ne.playlists.size())) return;
        cookie = g_ne.cookie;
        pid = g_ne.playlists[size_t(idx)].id;
        name = g_ne.playlists[size_t(idx)].name;
    }
    toast("正在打开歌单：" + name);

    std::thread([cookie, pid, name] {
        std::string err;
        auto tracks = ne::fetch_playlist(cookie, pid, err);
        if (g_shutdown.load()) return;
        if (err.empty()) {
            std::lock_guard<std::mutex> lk(g_mu);
            g_as.tracks = std::move(tracks);
            g_as.reports.clear();
            g_as.elapsed_ms = 0;
            g_as.searching = false;
            ++g_as.tracks_version;
        }
        queue_mark_dirty();
        queue_mark_dirty();
        if (!err.empty()) {
            if (g_win) {
                toast("打开歌单失败：" + err, 8000);
                g_win->wake();
            }
            return;
        }
        g_ui.query = name;
        g_goto_tab.store(0);
        if (g_win) g_win->wake();
    }).detach();
}

// ---- 队列操作（全局函数：后台线程与 UI 都会调）----
static void queue_remove(int idx) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (idx < 0 || idx >= int(g_as.tracks.size())) return;
        g_as.tracks.erase(g_as.tracks.begin() + idx);
        if (g_as.playing > idx) g_as.playing--;   // 正在播的那首位置也跟着挪
        ++g_as.tracks_version;
    }
    queue_mark_dirty();
}

static void queue_clear() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_as.tracks.clear();
        g_as.playing = -1;
        ++g_as.tracks_version;
    }
    queue_mark_dirty();
}

// 加进队列末尾：搜索结果的「＋」走这里。同一首（源 + id 相同）不重复加。
static bool queue_push(const music::Track& t) {
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
struct DiscSnap {
    bool loading = false;
    int site = 0;
    int version = -1;
    std::vector<disc::Card> cards;
    disc::Heats heats;
    std::string err;
};
static DiscSnap g_disc_snap;

static void disc_refresh(int site) {
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
    std::thread([site] {
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
    }).detach();
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
static void disc_open(int idx) {
    disc::Card card;
    {
        std::lock_guard<std::mutex> lk(g_disc.m);
        if (idx < 0 || idx >= int(g_disc.cards.size())) return;
        card = g_disc.cards[size_t(idx)];
    }
    toast("正在打开：" + card.name);
    std::thread([card] {
        const music::Settings st = g_ui.settings;
        std::string err;
        std::vector<music::Track> tracks;
        if (card.source == music::Source::Netease)
            tracks = ne::fetch_playlist(st.netease_cookie, card.id, err);
        else
            tracks = qq::fetch_playlist(st.qq_cookie, card.id, g_ui.qq_uin, err, 15000, /*plaza=*/true);
        if (g_shutdown.load()) return;
        if (!err.empty() || tracks.empty()) {
            if (g_win) {
                toast("打开失败：" + (err.empty() ? std::string("歌单里没有能播的歌") : err), 8000);
                g_win->wake();
            }
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
        g_goto_tab.store(0);
        queue_mark_dirty();
        if (g_win) {
            toast("已载入「" + card.name + "」 " + std::to_string(n) + " 首", 4000);
            g_win->wake();
        }
    }).detach();
}

// =============================================================================
//  播放队列持久化
// =============================================================================
// 队列就是主列表本身（g_as.tracks）—— 不另开一份数据，否则「搜索覆盖列表」
// 和「队列里删一首」会变成两套索引，播放位置、换源、下载全要跟着改。
// 文件格式：每行一首，制表符分隔，字段全转义（标题里真出现过制表符）。
static std::string queue_path() {
    wchar_t buf[MAX_PATH] = {0};
    if (GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH) == 0) return std::string();
    const std::string dir = net::narrow(buf) + "\\aino-player\\";
    CreateDirectoryW(net::widen(dir).c_str(), nullptr);
    return dir + "queue.txt";
}

static std::string q_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    return out;
}

static void queue_save() {
    const std::string path = queue_path();
    if (path.empty()) return;
    std::vector<music::Track> copy;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        copy = g_as.tracks;
    }
    if (copy.size() > 800) copy.resize(800);
    std::FILE* f = _wfopen(net::widen(path).c_str(), L"wb");
    if (!f) return;
    std::fprintf(f, "# aino queue v1\n");
    for (const auto& t : copy) {
        if (t.variants.empty()) continue;
        const music::Variant& v = t.variants[size_t(std::min<int>(t.chosen, int(t.variants.size()) - 1))];
        std::fprintf(f, "%d\t%s\t%s\t%s\t%s\t%s\t%d\t%s\n", int(v.source), q_escape(v.id).c_str(),
                     q_escape(v.native_id).c_str(), q_escape(t.title).c_str(), q_escape(t.artist).c_str(),
                     q_escape(t.album).c_str(), t.duration_ms, q_escape(t.cover_url).c_str());
    }
    std::fclose(f);
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
    std::thread([mine] {
        Sleep(1200);
        if (g_queue_gen.load() != mine) return;  // 期间又改了，让最后一次负责写
        if (!g_queue_dirty.exchange(false)) return;
        queue_save();
    }).detach();
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
    std::FILE* f = _wfopen(net::widen(path).c_str(), L"rb");
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
    const std::string pattern = g_temp_dir + "track-*.*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(net::widen(pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::string name = net::narrow(fd.cFileName);
        if (name.rfind("track-" + std::to_string(keep_gen) + ".", 0) == 0) continue;
        DeleteFileW((net::widen(g_temp_dir) + net::widen(name)).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void start_play(int index) {
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
    g_player.close();  // 主线程：MCI 只由主线程操作
    cleanup_temp(gen);
    if (g_ui.lyrics_open) request_lyrics(track);
    std::thread(do_play, g_win, track, st, gen, index).detach();
}

static void toggle_pause() {
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

static void play_step(int delta) {
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

static void set_volume(float v) {
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

namespace ui_draw {

using namespace aino;

struct Ctx {
    Ui& ui;
    const Theme& th;
    float W = 0;
    float H = 0;
    float bar_y = 0;
    Snapshot& s;
};

// 没有封面图时的兜底：按歌名哈希生成的渐变 + 首字（纯程序化，零请求）
static void procedural_cover(Ctx& c, const std::string& seed, const Rect& box, float radius) {
    const uint32_t h = fnv1a(seed);
    const Color top = hue_color(float(h % 360), 0.55f, 0.68f);
    const Color bot = hue_color(float(h % 360) + 40.0f, 0.60f, 0.46f);
    c.ui.fill_rect_grad(box, top, bot, radius);
    std::string first;
    if (!seed.empty()) first = seed.substr(0, utf8_step(seed, 0));
    if (!first.empty()) {
        Style st;
        st.font_size = box.w * 0.42f;
        st.font_weight = 700;
        st.align = Align::Center;
        st.valign = VAlign::Middle;
        c.ui.draw_text(first, box, Color(255, 255, 255, 215), st);
    }
}


// ---------- 自绘进度条 / 音量条 ----------
// 为什么不用 ui.slider：它的判定是"这一帧鼠标按住且自己是 active"，按下与松开
// 落在同一帧的快速点击不会触发（合成点击实测无效，真人快点也一样）。
// 播放器最常用的就是"点一下进度条跳到那里"，所以这里自己实现：
// 按下开始拖，松手才提交一次 seek —— 拖动过程中不反复调 MCI。
struct SeekBar {
    bool dragging = false;
    float drag_pos = 0.0f;
};

static bool seek_bar(Ctx& c, float value, float max_value, const Rect& box, SeekBar* st, float* committed) {
    const auto& in = c.ui.input();
    const bool enabled = max_value > 0.001f;
    const bool inside = box.outset(6.0f).contains(in.mouse.x, in.mouse.y);

    // 按下的那一刻就按鼠标位置取值 —— 否则"按下"沿用旧值，
    // 快速点击（按下与松开之间没有拖动）会 seek 回原地，看起来像没反应。
    auto t_at_mouse = [&]() {
        const float half = 7.0f;
        return std::min(1.0f, std::max(0.0f, (in.mouse.x - box.x - half) /
                                                     std::max(box.w - 2.0f * half, 1.0f)));
    };

    bool commit = false;
    if (enabled && inside && in.pressed[0]) {
        st->dragging = true;
        st->drag_pos = max_value * t_at_mouse();
    }
    if (st->dragging) {
        if (in.down[0]) {
            st->drag_pos = max_value * t_at_mouse();
        } else {
            commit = true;
            st->dragging = false;
        }
    }

    const float shown = std::min(max_value, std::max(0.0f, st->dragging ? st->drag_pos : value));
    const float t = enabled ? shown / max_value : 0.0f;

    const float track_h = 4.0f;
    const Rect track{box.x + 7.0f, box.cy() - track_h * 0.5f, std::max(box.w - 14.0f, 1.0f), track_h};
    c.ui.fill_rect(track, c.th.track, track_h * 0.5f);
    if (t > 0.0f)
        c.ui.fill_rect({track.x, track.y, std::max(track.w * t, track_h), track.h}, c.th.tint, track_h * 0.5f);

    const float kx = track.x + track.w * t;
    const Rect knob = Rect::centered(kx, box.cy(), st->dragging ? 13.0f : 11.0f, st->dragging ? 13.0f : 11.0f);
    c.ui.drop_shadow(knob, knob.h * 0.5f, 3.0f, Color(0, 0, 0, 60), 0.5f);
    c.ui.fill_circle(knob.cx(), knob.cy(), knob.w * 0.5f, Color(255, 255, 255));

    if (commit && committed) *committed = st->drag_pos;
    return commit;
}

// 有封面就画封面，没有就画渐变。图片解码与下载都在 cover::Store 里异步进行，
// 这里每帧只是一次 map 查询 —— 没就绪就先显示渐变，到货后自动换过来。
static void cover_art(Ctx& c, const std::string& url, const std::string& seed, const Rect& box, float radius) {
    if (!url.empty()) {
        if (auto img = g_covers.get(url); img && img->w > 0) {
            c.ui.draw_bitmap(box, img->rgba.data(), img->w, img->h, radius);
            return;
        }
    }
    procedural_cover(c, seed, box, radius);
}

// ---------- 侧栏 ----------
static void sidebar(Ctx& c) {
    const float w = kSidebar;
    c.ui.fill_rect({0, 0, w, c.H}, c.th.sidebar_bg);
    c.ui.fill_rect({w - 1, 0, 1, c.H}, c.th.separator);

    Style logo;
    logo.font_size = 19.0f;
    logo.font_weight = 700;
    c.ui.draw_text("aino", {22, 20, 80, 26}, c.th.label, logo);
    Style sub;
    sub.font_size = 10.5f;
    sub.font_weight = 600;
    const float lw = c.ui.text_width("aino", 19.0f, 700);
    c.ui.draw_text("PLAYER", {22 + lw + 7, 26, 80, 16}, c.th.label3, sub);

    const char* navs[6] = {"搜索", "发现", "正在播放", "歌单", "音源", "设置"};
    const SystemIcon icons[6] = {SystemIcon::Search, SystemIcon::Star, SystemIcon::List,
                                 SystemIcon::People, SystemIcon::Network, SystemIcon::Settings};
    for (int i = 0; i < 6; ++i) {
        const Rect box{12, 66.0f + float(i) * 32.0f, w - 24, 30};
        if (c.ui.nav_item(navs[i], g_ui.tab == i, box, Style().padded(34, 0))) g_ui.tab = i;
        c.ui.system_icon(icons[i], {box.x + 11, box.y + 7, 16, 16}, g_ui.tab == i ? c.th.tint : c.th.label2);
    }

    c.ui.fill_rect({20, 270, w - 40, 1}, c.th.separator);

    Style sec;
    sec.font_size = 10.5f;
    sec.font_weight = 600;
    c.ui.draw_text("音源状态", {22, 286, 120, 16}, c.th.label2, sec);

    for (int i = 0; i < music::kSourceCount; ++i) {
        const float y = 308.0f + float(i) * 26.0f;
        const bool on = (g_ui.src_mask >> i) & 1u;
        c.ui.fill_circle(26, y + 8, 3.5f, on ? source_color(music::Source(i)) : c.th.label4);

        Style name;
        name.font_size = 12.0f;
        name.font_weight = 500;
        c.ui.draw_text(music::source_name(music::Source(i)), {38, y, 70, 16}, on ? c.th.label : c.th.label3,
                       name);

        std::string tail;
        for (const auto& r : c.s.reports) {
            if (int(r.source) != i) continue;
            if (!r.ok && r.count == 0) {
                // 失败要写清原因（需登录 / 超时 / 关闭），不能只画一道横线
                if (r.err.find("需登录") != std::string::npos) tail = "需登录";
                else if (r.err.find("超时") != std::string::npos) tail = "超时";
                else if (r.err.find("已关闭") != std::string::npos) tail = "关闭";
                else tail = r.err.empty() ? "失败" : "失败";
            } else {
                tail = std::to_string(r.count);
                if (r.ms > 0) tail += " · " + std::to_string(r.ms) + "ms";
            }
        }
        Style ts;
        ts.font_size = 10.5f;
        ts.align = Align::Right;
        c.ui.draw_text(ellipsize(c.ui, tail, 100, 10.5f), {w - 116, y, 96, 16}, c.th.label3, ts);
    }

    // 自报数字：帧耗时 + 缓存（不写形容词，写数字）
    char buf[128];
    std::snprintf(buf, sizeof buf, "帧 %.1fms · 缓存 %s", g_ui.frame_ms, fmt_size(c.s.cached_bytes).c_str());
    Style foot;
    foot.font_size = 10.0f;
    const float foot_y = c.bar_y - 34.0f;   // 必须在播放条上沿之上，否则被它盖住
    c.ui.draw_text("aino player · 多音源", {22, foot_y, w - 44, 14}, c.th.label4, foot);
    c.ui.draw_text(buf, {22, foot_y + 16, w - 44, 14}, c.th.label3, foot);
}

// ---------- 结果行 ----------
static void result_row(Ctx& c, const music::Track& t, int index, const Rect& box, bool is_playing) {
    const bool selected = (g_ui.sel == index);
    const int nvar = int(t.variants.size());
    const float dur_w = 50.0f;
    const float dl_w = 26.0f;
    const float q_w = 26.0f;   // 「加入队列」占位，和下载按钮并排
    const float pill_h = 18.0f;
    const float mid_y = box.y + box.h * 0.5f;

    float pills_w = 0;
    for (const auto& v : t.variants)
        pills_w += tw_cached(c.ui, music::source_name(v.source), 10.5f) + 16.0f + 4.0f;

    // ---- 1) 先把右侧的小控件"认领"掉，再画整行 ----
    // aino 的按下只属于**第一个**包含指针的控件（press_taken）。整行的 item 若先画，
    // 这些按钮就永远收不到点击 —— 症状是"点下载 / 点药丸，结果重新播放了整行"。
    const Rect dl{box.right() - dur_w - 6 - dl_w + 2, mid_y - 11.0f, 22.0f, 22.0f};
    const Rect qa{dl.x - q_w, mid_y - 11.0f, 22.0f, 22.0f};
    int pct = -1;
    bool dl_done = false, dl_failed = false;
    const std::string uid = track_uid(t);
    if (auto it = g_dls_snap.find(uid); it != g_dls_snap.end()) {
        pct = it->second.percent;
        dl_done = it->second.done;
        dl_failed = it->second.failed;
    }
    const bool row_hover = box.contains(c.ui.input().mouse.x, c.ui.input().mouse.y);
    const bool dl_show = row_hover || dl_done || dl_failed || pct >= 0;
    bool dl_clicked = false;
    if (dl_show && c.ui.item("", "", "", dl, Style().corner(6))) dl_clicked = true;
    bool qa_clicked = false;
    if (row_hover && c.ui.item("", "", "", qa, Style().corner(6))) qa_clicked = true;

    const int chosen = std::min<int>(t.chosen, std::max(0, nvar - 1));
    Rect pill_rect[6];
    int pill_n = 0;
    {
        float px = box.right() - dur_w - 6 - dl_w - q_w - pills_w;
        for (int i = 0; i < nvar && i < 6; ++i) {
            const float w = tw_cached(c.ui, music::source_name(t.variants[size_t(i)].source), 10.5f) + 16.0f;
            pill_rect[pill_n++] = Rect(px, mid_y - pill_h * 0.5f, w, pill_h);
            px += w + 4.0f;
        }
    }
    int pill_clicked = -1;
    for (int i = 0; i < pill_n; ++i)
        if (c.ui.item("", "", "", pill_rect[i], Style().corner(9))) pill_clicked = i;

    // ---- 2) 整行 ----
    const bool row_clicked = c.ui.item("", "", "", box, Style().corner(8).padded(0, 0));
    if (selected) c.ui.fill_rect(box, c.th.tint.scaled_alpha(0.15f), 8.0f);

    const Rect cov{box.x + 10, box.y + 10, 44, 44};
    cover_art(c, t.cover_url, t.title, cov, 7.0f);
    if (is_playing) {
        // 垫一层暗底：封面底色偏浅时白条会"消失"（七里香那张就是白底图）
        const Rect ind{cov.right() - 17.0f, cov.y + 3.0f, 14.0f, 14.0f};
        c.ui.fill_rect(ind, Color(0, 0, 0, 120), 4.0f);
        draw_playing_bars(c.ui, {ind.x + 2.5f, ind.y + 3.0f, 9.0f, 8.0f}, Color(255, 255, 255, 240));
    }

    const float text_x = cov.right() + 12;
    const float max_text = box.w - (text_x - box.x) - pills_w - dur_w - dl_w - q_w - 18;

    Style ts;
    ts.font_size = 13.5f;
    ts.font_weight = 600;
    c.ui.draw_text(ellipsize(c.ui, t.title, max_text, 13.5f, 600), {text_x, box.y + 11, max_text, 18}, c.th.label,
                   ts);

    Style ds;
    ds.font_size = 11.0f;
    std::string sub = t.artist;
    if (!t.album.empty()) sub += " · " + t.album;
    c.ui.draw_text(ellipsize(c.ui, sub, max_text, 11.0f), {text_x, box.y + 33, max_text, 16}, c.th.label2, ds);

    // 药丸：每个源一枚，点它切换取流源
    for (int i = 0; i < pill_n; ++i) {
        const music::Variant& v = t.variants[size_t(i)];
        const std::string label = music::source_name(v.source);
        const Rect& pill = pill_rect[i];
        const bool is_chosen = (i == chosen);
        const Color col = source_color(v.source);
        if (is_chosen) {
            c.ui.fill_rect(pill, col.scaled_alpha(0.15f), 9.0f);
            c.ui.stroke_rect(pill, col.scaled_alpha(0.5f), 9.0f, 1.0f);
        } else {
            c.ui.fill_rect(pill, c.th.fill3, 9.0f);
        }
        Style ps;
        ps.font_size = 10.5f;
        ps.font_weight = is_chosen ? 600 : 400;
        ps.align = Align::Center;
        c.ui.draw_text(label, {pill.x, pill.y, pill.w, pill.h}, is_chosen ? col : c.th.label2, ps);
    }

    // 下载按钮：常态不画（行里已经很满），hover 或"下载中 / 已完成 / 失败"时才出现
    if (dl_show) {
        if (dl_done) {
            c.ui.system_icon(SystemIcon::Check, dl.center(13, 13), c.th.tint);
        } else if (dl_failed) {
            c.ui.system_icon(SystemIcon::Warning, dl.center(13, 13), c.th.destructive);
        } else if (pct >= 0) {
            Style ps;
            ps.font_size = 8.5f;
            ps.align = Align::Center;
            ps.valign = VAlign::Middle;
            c.ui.draw_text(std::to_string(pct), dl, c.th.tint, ps);
        } else {
            c.ui.system_icon(SystemIcon::Download, dl.center(13, 13), c.th.label2);
        }
    }

    if (row_hover) c.ui.system_icon(SystemIcon::Plus, qa.center(13, 13), c.th.label2);

    Style dur;
    dur.font_size = 11.0f;
    dur.align = Align::Right;
    c.ui.draw_text(fmt_time(t.duration_ms), {box.right() - dur_w - 6, box.y, dur_w, box.h}, c.th.label3, dur);

    c.ui.fill_rect({box.x + 10, box.bottom() - 1, box.w - 20, 1}, c.th.separator.scaled_alpha(0.6f));

    if (dl_clicked) start_download(t, g_ui.settings);
    if (qa_clicked) {
        // 「＋」= 追加到队列末尾（不覆盖当前列表）
        if (queue_push(t)) toast("已加入队列：" + t.title, 2600);
        else toast("队列里已经有这首了", 2600);
    }
    if (pill_clicked >= 0 && pill_clicked != chosen) {
        if (is_playing) {
            // 换源要立刻生效：正在播这首时，直接把选中的源改掉并重新取流，
            // 否则用户点了药丸只看到高亮变了、声音还是原来那条（或者干脆没声）。
            {
                std::lock_guard<std::mutex> lk(g_mu);
                if (index >= 0 && index < int(g_as.tracks.size()))
                    g_as.tracks[size_t(index)].chosen = pill_clicked;
            }
            start_play(index);
        } else {
            g_ui.chosen_override_track = index;
            g_ui.chosen_override_index = pill_clicked;
        }
    }
    if (row_clicked) start_play(index);
}

// ---------- 歌单页（网易云 / QQ 两个账号共用一套版式） ----------
// site：0 = 网易云，1 = QQ
static void playlist_row(Ctx& c, int site, int index, const Rect& box) {
    std::string name, cover;
    int count = 0;
    if (site == 1) {
        const qq::Playlist& pl = g_qq_snap.playlists[size_t(index)];
        name = pl.name;
        cover = pl.cover;
        count = pl.count;
    } else {
        const ne::Playlist& pl = g_ne_snap.playlists[size_t(index)];
        name = pl.name;
        cover = pl.cover;
        count = pl.count;
    }
    const bool sync_here = (g_sync_site.load() == site && g_sync_index.load() == index);

    // 同步按钮必须先于整行绘制：aino 的"按下"只归第一个包含指针的控件，
    // 整行先画的话这个按钮永远收不到点击（结果就是"点同步变成了打开歌单"）。
    const Rect sync_btn{box.right() - 84, box.y + box.h * 0.5f - 13, 60, 26};
    const bool hover = box.contains(c.ui.input().mouse.x, c.ui.input().mouse.y);
    const bool sync_clicked = c.ui.item("", "", "", sync_btn, Style().corner(7));

    const bool row_clicked = c.ui.item("", "", "", box, Style().corner(8));

    const Rect cov{box.x + 10, box.y + 10, 44, 44};
    cover_art(c, cover, name, cov, 7.0f);

    Style ts;
    ts.font_size = 13.5f;
    ts.font_weight = 600;
    const float max_w = box.w - 78 - 100;
    c.ui.draw_text(ellipsize(c.ui, name, max_w, 13.5f, 600), {cov.right() + 12, box.y + 11, max_w, 18},
                   c.th.label, ts);
    Style ds;
    ds.font_size = 11.0f;
    c.ui.draw_text(std::to_string(count) + " 首", {cov.right() + 12, box.y + 33, max_w, 16}, c.th.label2, ds);

    // 按钮常显（只悬停才显示的话根本没人知道能同步），悬停时描边加深
    c.ui.stroke_rect(sync_btn, sync_here ? c.th.tint.scaled_alpha(0.6f)
                                        : (hover ? c.th.separator : c.th.separator.scaled_alpha(0.55f)),
                     7.0f, 1.0f);
    Style bs;
    bs.font_size = 10.5f;
    bs.align = Align::Center;
    c.ui.draw_text(sync_here ? "同步中…" : "同步到此", {sync_btn.x, sync_btn.y, sync_btn.w, sync_btn.h},
                   sync_here ? c.th.tint : c.th.label2, bs);

    c.ui.fill_rect({box.x + 10, box.bottom() - 1, box.w - 20, 1}, c.th.separator.scaled_alpha(0.6f));

    if (sync_clicked) playlist_sync_to(site, index);
    else if (row_clicked) {
        if (site == 1) qq_open_playlist(index);
        else ne_open_playlist(index);
    }
}

// ---------- 发现页（热搜 + 热门歌单）----------
// 热搜词点一下直接搜；热门歌单点一下直接载入列表（能播能下能出歌词）。
static void discover_chips(Ctx& c, const Rect& box, const disc::Heats& h, float* used_h) {
    float x = box.x;
    float y = box.y;
    const float row_h = 30.0f;
    const float gap = 8.0f;
    const float max_y = box.y + box.h;

    Style cs;
    cs.font_size = 11.0f;
    cs.valign = VAlign::Middle;
    cs.align = Align::Center;

    if (h.words.empty()) {
        c.ui.muted_text(h.err.empty() ? "热搜为空" : h.err, {box.x, box.y, box.w, 20}, Style().font(10.5f));
        *used_h = 30.0f;
        return;
    }
    for (size_t i = 0; i < h.words.size(); ++i) {
        const std::string& w = h.words[i].text;
        const float tw = tw_cached(c.ui, w, 11.0f) + 24.0f;
        if (x + tw > box.right()) {
            x = box.x;
            y += row_h;
        }
        if (y + row_h > max_y) break;
        const Rect chip{x, y, tw, 24.0f};
        const bool hovered = chip.contains(c.ui.input().mouse.x, c.ui.input().mouse.y);
        // 顺序很重要：先画底色和字，最后再交给 item 认领按压。
        // 反过来（item 先、底色后）时按压会被随后绘制的填充层吃掉，点不动。
        c.ui.fill_rect(chip, hovered ? c.th.tint.scaled_alpha(0.18f) : c.th.fill3, 12.0f);
        // 前三名给一点强调色：热榜的味道全在名次上
        const Color fg = (i < 3) ? c.th.tint : c.th.label2;
        c.ui.draw_text(w, chip, fg, cs);
        const bool hot = c.ui.item("", "", "", chip, Style().corner(12));
        if (hot) {
            g_ui.query = w;
            g_ui.tab = 0;
            start_search();
        }
        x += tw + gap;
    }
    *used_h = (y - box.y) + row_h;
}

static void discover_card_row(Ctx& c, int index, const Rect& box) {
    const disc::Card& cd = g_disc_snap.cards[size_t(index)];
    const bool hover = box.contains(c.ui.input().mouse.x, c.ui.input().mouse.y);
    const bool clicked = c.ui.item("", "", "", box, Style().corner(9));
    if (hover) c.ui.fill_rect(box, c.th.fill3.scaled_alpha(0.7f), 9.0f);

    const Rect cov{box.x + 6, box.y + (box.h - 48.0f) * 0.5f, 48, 48};
    cover_art(c, cd.cover, cd.name, cov, 7.0f);

    const float tx = cov.right() + 12;
    const float tw = box.w - (tx - box.x) - 120.0f;

    Style ns;
    ns.font_size = 13.0f;
    ns.font_weight = 600;
    c.ui.draw_text(ellipsize(c.ui, cd.name, tw, 13.0f, 600), {tx, box.y + 10, tw, 18}, c.th.label, ns);

    std::string meta = cd.creator;
    if (cd.plays > 0) meta += (meta.empty() ? "" : " · ") + disc::human_count(cd.plays) + " 播放";
    if (cd.count > 0) meta += " · " + std::to_string(cd.count) + " 首";
    if (meta.empty()) meta = music::source_name(cd.source);
    Style ms;
    ms.font_size = 10.5f;
    c.ui.draw_text(ellipsize(c.ui, meta, tw, 10.5f), {tx, box.y + 32, tw, 16}, c.th.label3, ms);

    if (cd.official) {
        const Rect badge{box.right() - 46, box.y + box.h * 0.5f - 9, 34, 18};
        c.ui.fill_rect(badge, c.th.tint.scaled_alpha(0.16f), 5.0f);
        Style bs;
        bs.font_size = 9.5f;
        bs.align = Align::Center;
        bs.valign = VAlign::Middle;
        c.ui.draw_text("榜", badge, c.th.tint, bs);
    }
    if (clicked) disc_open(index);
}

static void tab_discover(Ctx& c) {
    const float mx = kSidebar + 24.0f;
    const float mw = c.W - mx - 24.0f - (g_ui.lyrics_open ? kLyricsWidth : 0.0f);

    c.ui.heading("发现", {mx, 20, mw, 30});
    int site = g_ui.disc_site;
    if (c.ui.segmented({"网易云", "QQ", "酷我"}, &site, {mx, 58, 226, 28}, Style().font(11.5f))) {
        g_ui.disc_site = site;
        disc_refresh(site);
    }
    if (c.ui.button("刷新", {mx + mw - 92, 58, 92, 28}, Style().font(11.5f))) disc_refresh(g_ui.disc_site);

    const DiscSnap& d = g_disc_snap;

    // ---- 热搜 ----
    c.ui.subheading("热搜", {mx, 100, 120, 22});
    const Rect chip_box{mx, 126, mw, 92};   // 最多三行
    float used = 0.0f;
    if (d.loading && d.heats.words.empty()) {
        c.ui.spinner({mx + 10, 132, 18, 18});
        used = 34.0f;
    } else {
        discover_chips(c, chip_box, d.heats, &used);
    }

    // ---- 热门歌单 ----
    float y = 126.0f + used + 18.0f;
    c.ui.subheading("热门歌单", {mx, y, 200, 22});
    y += 28.0f;

    if (!d.err.empty()) {
        Style es;
        es.font_size = 11.0f;
        es.wrap_text();
        c.ui.draw_text(d.err, {mx, y, mw, 44}, c.th.destructive, es);
        return;
    }
    if (d.cards.empty()) {
        if (d.loading) c.ui.spinner({mx + mw * 0.5f - 11, y + 40, 22, 22});
        else c.ui.empty_state("没有拿到歌单", std::string(), {mx, y + 20, mw, 100});
        return;
    }

    const Rect list{mx, y, mw, c.H - kBarHeight - y - 12};
    if (list.h < 60) return;
    const int n = int(d.cards.size());
    c.ui.scroll(list, n, 60.0f, [&](int i, const Rect& row) { discover_card_row(c, i, row); });
}

static void queue_row(Ctx& c, const music::Track& t, int index, const Rect& box, bool is_playing) {
    const float rm_w = 26.0f;
    const float dur_w = 48.0f;
    const bool hover = box.contains(c.ui.input().mouse.x, c.ui.input().mouse.y);

    // 行内按钮先认领按压（aino 的 press 只归第一个包含指针的控件）
    const Rect rm{box.right() - rm_w - 8, box.y + box.h * 0.5f - 11, 22, 22};
    bool rm_clicked = false;
    if (hover && c.ui.item("", "", "", rm, Style().corner(6))) rm_clicked = true;

    if (is_playing) c.ui.fill_rect(box, c.th.tint.scaled_alpha(0.10f), 7.0f);
    else if (hover) c.ui.fill_rect(box, c.th.fill3.scaled_alpha(0.55f), 7.0f);
    const bool clicked = c.ui.item("", "", "", box, Style().corner(7));

    Style idx_s;
    idx_s.font_size = 11.0f;
    idx_s.align = Align::Right;
    idx_s.valign = VAlign::Middle;
    const Rect idx_box{box.x + 6, box.y, 22, box.h};
    if (is_playing) {
        draw_playing_bars(c.ui, {idx_box.x + 6, box.y + box.h * 0.5f - 5, 10, 10}, c.th.tint);
    } else {
        c.ui.draw_text(std::to_string(index + 1), idx_box, c.th.label4, idx_s);
    }

    const Rect cov{box.x + 34, box.y + (box.h - 38.0f) * 0.5f, 38, 38};
    cover_art(c, t.cover_url, t.title, cov, 6.0f);

    const float tx = cov.right() + 11;
    const float tw = box.right() - tx - dur_w - rm_w - 18;
    Style ns;
    ns.font_size = 13.0f;
    ns.font_weight = 600;
    c.ui.draw_text(ellipsize(c.ui, t.title, tw, 13.0f, 600), {tx, box.y + 9, tw, 17},
                   is_playing ? c.th.tint : c.th.label, ns);
    Style ms;
    ms.font_size = 10.5f;
    std::string sub = t.artist;
    if (!t.variants.empty()) sub += " · " + std::string(music::source_name(t.current().source));
    c.ui.draw_text(ellipsize(c.ui, sub, tw, 10.5f), {tx, box.y + 29, tw, 15}, c.th.label3, ms);

    Style ds;
    ds.font_size = 11.0f;
    ds.align = Align::Right;
    ds.valign = VAlign::Middle;
    c.ui.draw_text(fmt_time(t.duration_ms), {box.right() - dur_w - rm_w - 10, box.y, dur_w, box.h}, c.th.label3, ds);

    // hover 时才出现的移除键：给个底，否则深色下 25% 的细线图标等于看不见
    if (hover) {
        c.ui.fill_rect(rm, c.th.fill3, 6.0f);
        c.ui.system_icon(SystemIcon::Close, rm.center(12, 12), c.th.label2);
    }

    c.ui.fill_rect({box.x + 34, box.bottom() - 1, box.w - 42, 1}, c.th.separator.scaled_alpha(0.5f));

    if (rm_clicked) queue_remove(index);
    else if (clicked) start_play(index);
}

static void tab_queue(Ctx& c) {
    const float mx = kSidebar + 24.0f;
    const float mw = c.W - mx - 24.0f - (g_ui.lyrics_open ? kLyricsWidth : 0.0f);
    const int n = int(c.s.tracks.size());

    c.ui.heading("正在播放", {mx, 20, mw, 30});

    std::string info = std::to_string(n) + " 首";
    if (c.s.playing >= 0) info += " · 正在播第 " + std::to_string(c.s.playing + 1) + " 首";
    Style is;
    is.font_size = 11.0f;
    c.ui.draw_text(info, {mx, 54, 260, 18}, c.th.label3, is);

    bool loop = g_ui.loop_list;
    Style loop_s;
    loop_s.font_size = 11.0f;
    loop_s.valign = VAlign::Middle;
    c.ui.draw_text("列表循环", {mx + mw - 220, 50, 70, 20}, c.th.label2, loop_s);
    if (c.ui.toggle(&loop, {mx + mw - 144, 49, 38, 21})) {
        g_ui.loop_list = loop;
        g_ui.config_dirty = true;
    }
    if (c.ui.button("清空", {mx + mw - 92, 48, 92, 28}, Style().font(11.5f))) {
        if (n > 0) queue_clear();
    }

    if (n == 0) {
        c.ui.empty_state("队列是空的", std::string(), {mx, 110, mw, 120});
        return;
    }

    const float top = 92.0f;
    const Rect list{mx, top, mw, c.H - kBarHeight - top - 10};
    if (list.h < 50) return;
    c.ui.scroll(list, n, 56.0f,
                [&](int i, const Rect& row) { queue_row(c, c.s.tracks[size_t(i)], i, row, c.s.playing == i); });
}

static void tab_playlists(Ctx& c) {
    const float mx = kSidebar + 24.0f;
    const float mw = c.W - mx - 24.0f - (g_ui.lyrics_open ? kLyricsWidth : 0.0f);

    c.ui.heading("歌单", {mx, 20, mw, 30});
    int site = g_ui.pl_site;
    if (c.ui.segmented({"网易云", "QQ"}, &site, {mx, 58, 172, 28}, Style().font(11.5f))) g_ui.pl_site = site;

    const bool qq_side = (g_ui.pl_site == 1);
    const bool logged = qq_side ? g_qq_snap.logged : g_ne_snap.logged;
    const bool loading = qq_side ? g_qq_snap.loading : g_ne_snap.loading;
    const std::string err = qq_side ? g_qq_snap.err : g_ne_snap.err;
    const std::string nickname = qq_side ? g_qq_snap.nickname : g_ne_snap.nickname;
    const std::string uid = qq_side ? g_ui.qq_uin : g_ui.ne_uid;
    const int pl_count = int(qq_side ? g_qq_snap.playlists.size() : g_ne_snap.playlists.size());
    const char* site_name = qq_side ? "QQ 音乐" : "网易云音乐";

    if (!logged) {
        const Rect card{mx, 102, mw, 112};
        c.ui.panel(card, Style().corner(12));
        c.ui.subheading(std::string("没有连接 ") + site_name, {card.x + 20, card.y + 18, 360, 24});
        if (c.ui.button("去登录", {card.x + 20, card.y + 58, 100, 30}, Style().primary().font(12.0f))) g_ui.tab = 5;
        if (!err.empty()) c.ui.muted_text(err, {card.x + 136, card.y + 64, card.w - 156, 20}, Style().font(10.0f));
        return;
    }

    Style nm;
    nm.font_size = 15.0f;
    nm.font_weight = 600;
    c.ui.draw_text(nickname.empty() ? "已登录" : nickname, {mx, 100, mw - 220, 22}, c.th.label, nm);
    Style meta;
    meta.font_size = 10.5f;
    c.ui.draw_text((qq_side ? "QQ " : "uid ") + uid + " · " + std::to_string(pl_count) + " 个歌单",
                   {mx, 122, mw - 220, 16}, c.th.label3, meta);
    if (c.ui.button("刷新", {mx + mw - 92, 102, 92, 28}, Style().font(11.5f))) {
        if (qq_side) qq_refresh();
        else ne_refresh();
    }

    if (!err.empty()) {
        Style es;
        es.font_size = 11.0f;
        es.wrap_text();
        c.ui.draw_text(err, {mx, 148, mw, 32}, c.th.destructive, es);
        return;
    }
    if (loading) {
        c.ui.spinner({mx + mw * 0.5f - 11, 172, 22, 22});
        return;
    }
    if (pl_count == 0) {
        c.ui.empty_state("这个账号下没有歌单", std::string(), {mx, 176, mw, 110});
        return;
    }

    const Rect list{mx, 152, mw, c.H - kBarHeight - 152 - 12};
    if (list.h < 64) return;
    const int pl_site = g_ui.pl_site;
    c.ui.scroll(list, pl_count, 64, [&](int i, const Rect& row) { playlist_row(c, pl_site, i, row); });
}

// ---------- 搜索页 ----------
static void tab_search(Ctx& c) {
    const float mx = kSidebar + 24.0f;
    // 歌词面板是从右边盖过来的，列表得让出位置，否则标题会被压在面板底下
    const float right_pad = 24.0f + (g_ui.lyrics_open ? kLyricsWidth : 0.0f);
    const float mw = c.W - mx - right_pad;

    const Rect field{mx, 20, mw - 100.0f, 32};
    c.ui.search_field(&g_ui.query, field, Style().hint("搜索歌曲 / 歌手 / 专辑"));
    if (c.ui.button(c.s.searching ? "搜索中…" : "搜索", {mx + mw - 92.0f, 20, 92, 32},
                    Style().primary().font(12.5f))) {
        start_search();
    }
    if (c.ui.input().key_pressed[KEY_ENTER]) start_search();

    // 音源筛选
    std::uint32_t mask = g_ui.src_mask;
    const Rect chips{mx, 62, 236, 24};
    if (c.ui.toggle_group_multi({"网易云", "QQ", "酷我"}, &mask, chips, Style().font(11.0f))) {
        g_ui.src_mask = mask;
        if (mask != 0) start_search();
    }

    // 状态行
    Style st;
    st.font_size = 11.0f;
    st.align = Align::Right;
    std::string status;
    if (c.s.searching) status = "并发检索中…";
    else if (!c.s.tracks.empty()) {
        if (c.s.reports.empty()) {
            // 歌单模式：没有并发检索，别显示"0/3 源有结果"
            status = std::to_string(c.s.tracks.size()) + " 首 · 来自歌单";
        } else {
            int live = 0;
            for (const auto& r : c.s.reports)
                if (r.count > 0) ++live;
            status = std::to_string(c.s.tracks.size()) + " 首 · " + std::to_string(c.s.elapsed_ms) + " ms · " +
                     std::to_string(live) + "/" + std::to_string(music::kSourceCount) + " 源有结果";
        }
    }
    c.ui.draw_text(status, {mx + 250, 62, mw - 250, 24}, c.th.label3, st);

    const Rect list{mx, 96, mw, c.H - kBarHeight - 96 - 14};
    if (list.h < kRowHeight) return;

    if (c.s.tracks.empty()) {
        if (c.s.searching) {
            c.ui.spinner({list.cx() - 11, list.y + list.h * 0.34f, 22, 22});
            Style es;
            es.font_size = 11.5f;
            es.align = Align::Center;
            c.ui.draw_text("正在并发检索三个音源…", {list.x, list.y + list.h * 0.34f + 34, list.w, 18},
                           c.th.label2, es);
        } else {
            c.ui.empty_state("搜点什么", std::string(), {list.x, list.y + list.h * 0.26f, list.w, 120});
        }
        return;
    }

    if (g_ui.scroll_to_top) {
        c.ui.scroll_to(0);
        g_ui.scroll_to_top = false;
    }

    const int count = int(c.s.tracks.size());
    c.ui.scroll(list, count, kRowHeight, [&](int i, const Rect& row) {
        result_row(c, c.s.tracks[size_t(i)], i, row, track_uid(c.s.tracks[size_t(i)]) == c.s.now_uid);
    });
}

// ---------- 音源页 ----------
static void tab_sources(Ctx& c) {
    const float mx = kSidebar + 24.0f;
    const float mw = c.W - mx - 24.0f;
    c.ui.heading("音源", {mx, 20, mw, 30});

    const char* cookie_hints[3] = {"MUSIC_U=...", "qm_keyst=...", ""};
    std::string* cookies[3] = {&g_ui.settings.netease_cookie, &g_ui.settings.qq_cookie, nullptr};

    float y = 64.0f;
    for (int i = 0; i < music::kSourceCount; ++i) {
        const bool has_cookie = cookies[i] != nullptr;
        const Rect card{mx, y, mw, has_cookie ? 112.0f : 72.0f};
        c.ui.panel(card, Style().corner(12));

        const Color col = source_color(music::Source(i));
        c.ui.fill_circle(card.x + 22, card.y + 26, 4.0f, col);
        Style hs;
        hs.font_size = 14.0f;
        hs.font_weight = 600;
        c.ui.draw_text(music::source_name(music::Source(i)), {card.x + 34, card.y + 16, 200, 20}, c.th.label, hs);

        // 上次检索报告
        std::string rep = "未检索";
        for (const auto& r : c.s.reports) {
            if (int(r.source) != i) continue;
            char buf[160];
            if (r.ok)
                std::snprintf(buf, sizeof buf, "%d 条 · %d ms", r.count, r.ms);
            else
                std::snprintf(buf, sizeof buf, "失败：%s", r.err.c_str());
            rep = buf;
        }
        Style rs;
        rs.font_size = 11.0f;
        const bool bad = rep.rfind("失败", 0) == 0;
        c.ui.draw_text(rep, {card.x + 34, card.y + 38, 300, 16}, bad ? c.th.destructive : c.th.label2, rs);

        // 开关
        bool on = (g_ui.src_mask >> i) & 1u;
        if (c.ui.toggle(&on, {card.right() - 60, card.y + 18, 40, 22})) {
            if (on) g_ui.src_mask |= (1u << i);
            else g_ui.src_mask &= ~(1u << i);
            g_ui.config_dirty = true;
        }

        // Cookie
        if (has_cookie) {
            Style lbl;
            lbl.font_size = 10.5f;
            c.ui.draw_text("登录 Cookie", {card.x + 20, card.y + 62, 200, 14}, c.th.label3, lbl);

            // 已填的 Cookie 默认**只显示掩码**：整串凭据（几百个字符）铺在输入框里
            // 既会溢出卡片边界，又等于把 token 摆在屏幕上。点一下才进入编辑态。
            static bool editing[3] = {false, false, false};
            const Rect in_box{card.x + 20, card.y + 78, card.w - 40, 26};
            if (cookies[i]->empty() || editing[i]) {
                if (c.ui.input(cookies[i], in_box, Style().hint(cookie_hints[i]).font(11.5f)))
                    g_ui.config_dirty = true;
            } else {
                if (c.ui.item("", "", "", in_box, Style().corner(6))) editing[i] = true;
                c.ui.fill_rect(in_box, c.th.fill3, 6.0f);
                Style ms;
                ms.font_size = 11.0f;
                ms.valign = VAlign::Middle;
                const std::string masked = std::string(music::text::decode_entities(
                                               cookies[i]->substr(0, std::min<size_t>(cookies[i]->size(), 10)))) +
                                           "•••••••• （" + std::to_string(cookies[i]->size()) + " 字符，点击编辑）";
                c.ui.draw_text(ellipsize(c.ui, masked, in_box.w - 20, 11.0f),
                               {in_box.x + 10, in_box.y, in_box.w - 20, in_box.h}, c.th.label2, ms);
            }
        }
        y = card.bottom() + 12.0f;
    }
}

// ---------- 设置页 ----------

static void begin_login(int site) {
    if (!login::runtime_available()) {
        toast("这台机器没有 Edge WebView2 运行时，装一个再登录", 8000);
        return;
    }
    g_wait_login_site.store(site);
    login::start(site == 1 ? login::Site::QQ : login::Site::NetEase);
}

// 账号卡：site 0 = 网易云，1 = QQ
static void account_card(Ctx& c, int site, const Rect& box) {
    const bool qq_side = (site == 1);
    c.ui.panel(box, Style().corner(12));
    c.ui.subheading(qq_side ? "QQ 音乐" : "网易云音乐", {box.x + 20, box.y + 12, 240, 22});

    std::string note;
    {
        std::lock_guard<std::mutex> lk(login::state(qq_side ? login::Site::QQ : login::Site::NetEase).m);
        note = login::state(qq_side ? login::Site::QQ : login::Site::NetEase).note;
    }

    const bool logged = qq_side ? g_qq_snap.logged : g_ne_snap.logged;
    const std::string nickname = qq_side ? g_qq_snap.nickname : g_ne_snap.nickname;
    const std::string uid = qq_side ? g_ui.qq_uin : g_ui.ne_uid;
    const std::string err = qq_side ? g_qq_snap.err : g_ne_snap.err;
    const int count = int(qq_side ? g_qq_snap.playlists.size() : g_ne_snap.playlists.size());

    if (logged) {
        Style nm;
        nm.font_size = 13.5f;
        nm.font_weight = 600;
        c.ui.draw_text(nickname.empty() ? "已登录" : nickname, {box.x + 20, box.y + 40, 300, 20}, c.th.label, nm);
        c.ui.muted_text((qq_side ? "QQ " : "uid ") + uid + " · " + std::to_string(count) + " 个歌单",
                        {box.x + 20, box.y + 62, 300, 18}, Style().font(10.5f));
        if (c.ui.button("刷新歌单", {box.right() - 200, box.y + 38, 84, 28}, Style().font(11.0f))) {
            if (qq_side) qq_refresh();
            else ne_refresh();
        }
        if (c.ui.button("重新登录", {box.right() - 108, box.y + 38, 88, 28}, Style().font(11.0f)))
            begin_login(site);
    } else {
        c.ui.muted_text("未登录", {box.x + 20, box.y + 44, 200, 18}, Style().font(11.0f));
        if (c.ui.button("登录", {box.right() - 108, box.y + 38, 88, 28}, Style().primary().font(11.5f)))
            begin_login(site);
    }

    if (!err.empty()) c.ui.muted_text(err, {box.x + 20, box.y + 84, box.w - 240, 18}, Style().font(10.0f));
    else if (!note.empty()) c.ui.muted_text(note, {box.x + 20, box.y + 84, box.w - 240, 18}, Style().font(10.0f));
}

static void tab_settings(Ctx& c) {
    const float mx = kSidebar + 24.0f;
    const float mw = c.W - mx - 24.0f - (g_ui.lyrics_open ? kLyricsWidth : 0.0f);
    c.ui.heading("设置", {mx, 20, mw, 30});

    account_card(c, 0, {mx, 60, mw, 110});
    account_card(c, 1, {mx, 182, mw, 110});

    // ---------------- 外观 ----------------
    const Rect look{mx, 304, mw, 56};
    c.ui.panel(look, Style().corner(12));
    Style lbl;
    lbl.font_size = 11.5f;
    lbl.valign = VAlign::Middle;
    c.ui.draw_text("深色模式", {look.x + 20, look.y + 18, 160, 20}, c.th.label2, lbl);
    bool dark = g_ui.dark;
    if (c.ui.toggle(&dark, {look.right() - 60, look.y + 17, 40, 22})) {
        g_ui.dark = dark;
        g_ui.config_dirty = true;
    }

    // ---------------- 音质 ----------------
    const Rect qcard{mx, 372, mw, 56};
    c.ui.panel(qcard, Style().corner(12));
    c.ui.draw_text("默认音质", {qcard.x + 20, qcard.y + 18, 120, 20}, c.th.label2, lbl);
    int q = g_ui.settings.quality;
    if (c.ui.segmented({"标准", "高", "无损"}, &q, {qcard.right() - 232, qcard.y + 15, 212, 26},
                       Style().font(11.0f))) {
        g_ui.settings.quality = q;
        g_ui.config_dirty = true;
    }

    // ---------------- 播放 ----------------
    const Rect card{mx, 440, mw, 84};
    c.ui.panel(card, Style().corner(12));
    c.ui.draw_text("默认音量", {card.x + 20, card.y + 12, 120, 16}, c.th.label2, Style().font(11.5f));
    static SeekBar set_vol_bar;
    float vol_to = 0.0f;
    if (seek_bar(c, g_ui.volume, 100.0f, {card.x + 20, card.y + 34, card.w - 90, 20}, &set_vol_bar, &vol_to))
        set_volume(vol_to);
    c.ui.draw_text(std::to_string(int(g_ui.volume)), {card.right() - 64, card.y + 34, 44, 20}, c.th.label2,
                   Style().font(11.0f).right());
    char buf[256];
    std::snprintf(buf, sizeof buf, "缓存目录  %s", g_temp_dir.c_str());
    c.ui.muted_text(buf, {card.x + 20, card.y + 60, card.w - 40, 16}, Style().font(10.0f));

    // ---------------- 下载 ----------------
    const Rect dlcard{mx, 536, mw, 110};
    c.ui.panel(dlcard, Style().corner(12));
    c.ui.draw_text("下载目录", {dlcard.x + 20, dlcard.y + 10, 160, 16}, c.th.label2, Style().font(11.5f));
    if (c.ui.input(&g_ui.download_dir, {dlcard.x + 20, dlcard.y + 30, dlcard.w - 40, 28},
                   Style().hint(default_download_dir()).font(11.5f)))
        g_ui.config_dirty = true;

    // 内嵌：默认都关（会改写文件，得让用户自己决定）
    bool emb_cover = g_ui.settings.embed_cover;
    c.ui.draw_text("内嵌封面", {dlcard.x + 20, dlcard.y + 68, 80, 20}, c.th.label2, lbl);
    if (c.ui.toggle(&emb_cover, {dlcard.x + 104, dlcard.y + 67, 40, 22})) {
        g_ui.settings.embed_cover = emb_cover;
        g_ui.config_dirty = true;
    }
    bool emb_lyric = g_ui.settings.embed_lyrics;
    c.ui.draw_text("内嵌歌词", {dlcard.x + 176, dlcard.y + 68, 80, 20}, c.th.label2, lbl);
    if (c.ui.toggle(&emb_lyric, {dlcard.x + 260, dlcard.y + 67, 40, 22})) {
        g_ui.settings.embed_lyrics = emb_lyric;
        g_ui.config_dirty = true;
    }

}

// 歌词图标：三条递减的横线（像一段排版好的歌词）
static void draw_lyric_icon(aino::Ui& ui, const aino::Rect& box, const aino::Color& col) {
    const float h = 2.0f;
    const float gaps[3] = {1.0f, 0.78f, 0.55f};
    for (int i = 0; i < 3; ++i) {
        const float w = box.w * gaps[i];
        ui.fill_rect({box.x, box.y + float(i) * (h + 3.5f), w, h}, col, h * 0.5f);
    }
}

// ---------- 底部播放条（Apple Music 版式） ----------
// 结构：左「封面 + 两行信息」｜中「控制键 + 进度条」固定宽度块｜右「歌词 / 下载 / 音量」
//
// 和上一版的关键差别：中间那块是**固定 420 宽、整体居中**，不是通栏进度条。
// 通栏的问题不在实现，而在版式 —— 窗口一宽，进度条会变成一条横贯全窗的细线，
// 时间和音量被甩到两个角上，视线没有落点。Apple Music 的做法是：所有控件挤在
// 屏幕中央一小块，两侧留白，眼睛一眼就能看全。
static void player_bar(Ctx& c) {
    const float y = c.bar_y;
    c.ui.fill_rect({0, y, c.W, kBarHeight}, c.th.control_bg);
    c.ui.fill_rect({0, y, c.W, 1}, c.th.separator);

    const float pad = 20.0f;
    const float cx = c.W * 0.5f;
    const float block_w = 420.0f;
    const float block_x = cx - block_w * 0.5f;

    const bool playing = (c.s.ps == PlayState::Playing);
    const bool busy = (c.s.ps == PlayState::Resolving || c.s.ps == PlayState::Buffering);
    const float ctrl_cy = y + 31.0f;   // 控制键中线
    const float prog_cy = y + 59.0f;   // 进度条中线

    // ---------------- 左：封面 + 曲目 ----------------
    const Rect cov{pad, y + 18.0f, 52.0f, 52.0f};
    if (!c.s.now_title.empty()) {
        cover_art(c, c.s.now_cover, c.s.now_title, cov, 8.0f);
    } else {
        c.ui.fill_rect(cov, c.th.fill3, 8.0f);
        c.ui.system_icon(SystemIcon::Star, cov.center(20, 20), c.th.label3);
    }

    const float text_x = cov.right() + 12.0f;
    const float text_w = std::max(90.0f, std::min(280.0f, block_x - 40.0f - text_x));
    const float title_y = y + 24.0f;

    Style ts;
    ts.font_size = 13.0f;
    ts.font_weight = 600;
    c.ui.draw_text(ellipsize(c.ui, c.s.now_title.empty() ? "未在播放" : c.s.now_title, text_w, 13.0f, 600),
                   {text_x, title_y, text_w, 18}, c.s.now_title.empty() ? c.th.label3 : c.th.label, ts);

    // 副行：艺人 · 音质（音质放这里，省掉原来第三行，底栏才能只有 88 高）
    std::string sub;
    if (c.s.now_title.empty()) {
        sub = "从下面的列表里挑一首";
    } else {
        sub = c.s.now_artist;
        if (!c.s.now_short.empty()) sub += (sub.empty() ? "" : " · ") + c.s.now_short;
    }
    if (busy) {
        sub = c.s.ps == PlayState::Buffering
                  ? "缓冲 " + std::to_string(int(c.s.buffer * 100)) + "% · " + c.s.now_artist
                  : "解析中 · " + c.s.now_artist;
    }
    Style as;
    as.font_size = 11.0f;
    c.ui.draw_text(ellipsize(c.ui, sub, text_w, 11.0f), {text_x, title_y + 20.0f, text_w, 16}, c.th.label2, as);

    // ---------------- 中：控制键 ----------------
    const Rect prev{cx - 54.0f, ctrl_cy - 14.0f, 28.0f, 28.0f};
    const Rect play{cx - 17.0f, ctrl_cy - 17.0f, 34.0f, 34.0f};
    const Rect next{cx + 26.0f, ctrl_cy - 14.0f, 28.0f, 28.0f};

    if (c.ui.icon_button("", prev, Style().ghost())) play_step(-1);
    c.ui.system_icon(SystemIcon::Prev, prev.center(13, 13), c.th.label);
    if (c.ui.icon_button("", next, Style().ghost())) play_step(1);
    c.ui.system_icon(SystemIcon::Next, next.center(13, 13), c.th.label);

    // 播放键用纯图标 + hover 圆底（Apple Music 就是这样，没有实心蓝圆）
    if (c.ui.icon_button("", play, Style().ghost().corner(17))) toggle_pause();
    if (busy) {
        c.ui.spinner(play.center(18, 18), Style());
    } else if (playing) {
        draw_pause_bars(c.ui, play.center(12, 14), c.th.label);
    } else {
        draw_play_triangle(c.ui, play.center(12, 14).offset(1.0f, 0), c.th.label);
    }

    // ---------------- 中：进度 ----------------
    float dur = g_ui.dur_ms > 0 ? g_ui.dur_ms : 1.0f;
    static SeekBar prog_bar;
    float seek_to = 0.0f;
    const Rect prog_box{cx - 166.0f, prog_cy - 10.0f, 332.0f, 20.0f};
    if (seek_bar(c, std::min(g_ui.pos_ms, dur), dur, prog_box, &prog_bar, &seek_to)) {
        g_player.seek_ms(int(seek_to));
        g_ui.pos_ms = seek_to;
    }
    Style tm;
    tm.font_size = 10.0f;
    tm.valign = VAlign::Middle;
    tm.align = Align::Right;
    c.ui.draw_text(fmt_time((long long)g_ui.pos_ms), {cx - 210.0f, prog_cy - 8.0f, 36.0f, 16}, c.th.label3, tm);
    Style tm2 = tm;
    tm2.align = Align::Left;
    c.ui.draw_text(fmt_time((long long)g_ui.dur_ms), {cx + 174.0f, prog_cy - 8.0f, 36.0f, 16}, c.th.label3, tm2);

    // ---------------- 右：歌词 / 下载 / 音量 ----------------
    const float num_w = 24.0f, slider_w = 80.0f, spk_w = 14.0f;
    float x = c.W - pad;

    Style vn;
    vn.font_size = 10.5f;
    vn.valign = VAlign::Middle;
    vn.align = Align::Right;
    c.ui.draw_text(std::to_string(int(g_ui.volume)), {x - num_w, ctrl_cy - 8.0f, num_w, 16}, c.th.label3, vn);
    x -= num_w + 6.0f;

    static SeekBar vol_bar;
    float vol_to = 0.0f;
    if (seek_bar(c, g_ui.volume, 100.0f, {x - slider_w, ctrl_cy - 10.0f, slider_w, 20.0f}, &vol_bar, &vol_to))
        set_volume(vol_to);
    x -= slider_w + 8.0f;

    draw_speaker(c.ui, {x - spk_w, ctrl_cy - 6.5f, spk_w, 13}, c.th.label3);
    x -= spk_w + 18.0f;

    // 下载当前曲目
    const auto& in = c.ui.input();
    const Rect dl_btn{x - 28.0f, ctrl_cy - 14.0f, 28.0f, 28.0f};
    const bool dl_hover = dl_btn.contains(in.mouse.x, in.mouse.y);
    if (dl_hover) c.ui.fill_rect(dl_btn, c.th.fill3, 6.0f);
    if (dl_hover && in.released[0]) {
        if (c.s.now_uid.empty()) {
            toast("先播一首，再点下载");
        } else {
            // 锁只包住"找这一行"，别把下载（会写文件、起线程）放在锁里
            music::Track t;
            {
                std::lock_guard<std::mutex> lk(g_mu);
                for (const auto& tr : g_as.tracks) {
                    if (track_uid(tr) == c.s.now_uid) { t = tr; break; }
                }
            }
            if (!t.variants.empty()) start_download(t, g_ui.settings);
            else toast("这一首没有可下载的源");
        }
    }
    // 底栏的下载按钮：正在下载显示进度数字，完成显示对勾
    {
        int pct = -1;
        bool done = false, failed = false;
        if (auto it = g_dls_snap.find(c.s.now_uid); it != g_dls_snap.end()) {
            pct = it->second.percent;
            done = it->second.done;
            failed = it->second.failed;
        }
        if (done) {
            c.ui.system_icon(SystemIcon::Check, dl_btn.center(14, 14), c.th.tint);
        } else if (failed) {
            c.ui.system_icon(SystemIcon::Warning, dl_btn.center(14, 14), c.th.destructive);
        } else if (pct >= 0) {
            Style ps;
            ps.font_size = 9.0f;
            ps.align = Align::Center;
            ps.valign = VAlign::Middle;
            c.ui.draw_text(std::to_string(pct), {dl_btn.x, dl_btn.y, dl_btn.w, dl_btn.h}, c.th.tint, ps);
        } else {
            c.ui.system_icon(SystemIcon::Download, dl_btn.center(14, 14), c.th.label2);
        }
    }
    x -= 28.0f + 8.0f;

    // 歌词面板开关
    const Rect lyr_btn{x - 28.0f, ctrl_cy - 14.0f, 28.0f, 28.0f};
    if (c.ui.icon_button("", lyr_btn, Style().ghost())) {
        g_ui.lyrics_open = !g_ui.lyrics_open;
        if (g_ui.lyrics_open) {
            std::lock_guard<std::mutex> lk(g_mu);
            for (const auto& tr : g_as.tracks)
                if (track_uid(tr) == g_as.now_uid) { request_lyrics(tr); break; }
        }
    }
    draw_lyric_icon(c.ui, lyr_btn.center(15, 13), g_ui.lyrics_open ? c.th.tint : c.th.label2);
}

// ---------- Toast：一次性提示（下载完成 / 取流失败 / 歌单操作） ----------
// 底栏不再承载长错误文本 —— 那句"取流失败 — 网易云 会员曲…"以前挤在右下角，
// 一长就压到别的控件上。这些消息现在浮在底栏上方，几秒后自己消失。
static void draw_toast(Ctx& c) {
    if (g_ui.toast.empty()) return;
    const unsigned long long now = GetTickCount64();
    if (now >= g_toast_until.load()) {
        g_ui.toast.clear();
        return;
    }
    Style s;
    s.font_size = 11.5f;
    const float max_w = 560.0f;
    const float tw = std::min(max_w, c.ui.text_width(g_ui.toast, 11.5f));
    const float w = tw + 32.0f;
    const Rect box{c.W * 0.5f - w * 0.5f, c.bar_y - 52.0f, w, 34.0f};
    c.ui.drop_shadow(box, 8.0f, 6.0f, Color(0, 0, 0, 60), 0.5f);
    const bool dk = g_ui.dark;
    c.ui.fill_rect(box, dk ? Color(236, 236, 240, 242) : Color(28, 28, 30, 235), 8.0f);
    s.align = Align::Center;
    s.valign = VAlign::Middle;
    c.ui.draw_text(ellipsize(c.ui, g_ui.toast, max_w, 11.5f), box,
                   dk ? Color(24, 24, 26, 245) : Color(255, 255, 255, 240), s);
}

// =============================================================================
//  歌词面板（右侧抽屉，Apple Music 那种）
// =============================================================================

// 折行结果缓存：只有歌词或宽度变了才重算。60 行的歌词每字宽度都量一遍是
// 千把次 text_width，每帧都做会把帧时间顶上去。
struct LyricLayout {
    const void* owner = nullptr;
    float width = -1.0f;
    std::vector<int> src;          // 显示行 → 原行下标
    std::vector<std::string> text;
    std::vector<float> top;        // 相对内容顶部的 y
    float total = 0.0f;
};
static LyricLayout g_lyr_layout;
static float g_lyr_scroll = 0.0f;
static int g_lyr_cur = -2;

static void build_lyric_layout(aino::Ui& ui, const lyr::Doc& doc, float width) {
    g_lyr_layout.owner = &doc;
    g_lyr_layout.width = width;
    g_lyr_layout.src.clear();
    g_lyr_layout.text.clear();
    g_lyr_layout.top.clear();

    const float line_h = 23.0f;
    const float para_gap = 9.0f;
    float y = 0.0f;
    bool first = true;
    for (int i = 0; i < int(doc.lines.size()); ++i) {
        const std::string& t = doc.lines[i].text;
        if (t.empty()) continue;  // LRC 里的空行（间奏）直接跳过，不占位
        if (!first) y += para_gap;
        first = false;

        if (ui.text_width(t, 13.0f) <= width) {
            g_lyr_layout.src.push_back(i);
            g_lyr_layout.text.push_back(t);
            g_lyr_layout.top.push_back(y);
            y += line_h;
            continue;
        }
        // 超宽：按 UTF-8 字符断行（中文没有空格，硬断才是对的）
        std::string line;
        for (size_t k = 0; k < t.size();) {
            const size_t step = utf8_step(t, k);
            const std::string probe = line + t.substr(k, step);
            if (!line.empty() && ui.text_width(probe, 13.0f) > width) {
                g_lyr_layout.src.push_back(i);
                g_lyr_layout.text.push_back(line);
                g_lyr_layout.top.push_back(y);
                y += line_h;
                line = t.substr(k, step);
            } else {
                line = probe;
            }
            k += step;
        }
        if (!line.empty()) {
            g_lyr_layout.src.push_back(i);
            g_lyr_layout.text.push_back(line);
            g_lyr_layout.top.push_back(y);
            y += line_h;
        }
    }
    g_lyr_layout.total = y;
}

static void lyrics_panel(Ctx& c, const Rect& box) {
    c.ui.fill_rect(box, c.th.control_bg);
    c.ui.fill_rect({box.x, box.y, 1, box.h}, c.th.separator);

    Style h;
    h.font_size = 13.0f;
    h.font_weight = 600;
    c.ui.draw_text("歌词", {box.x + 20, box.y + 15, 120, 22}, c.th.label, h);

    const Rect close{box.right() - 38, box.y + 14, 24, 24};
    if (c.ui.icon_button("", close, Style().ghost())) g_ui.lyrics_open = false;
    c.ui.system_icon(SystemIcon::Close, close.center(11, 11), c.th.label3);

    std::shared_ptr<const lyr::Doc> doc;
    std::string uid;
    bool loading = false;
    {
        std::lock_guard<std::mutex> lk(g_lyr.m);
        doc = g_lyr.doc;
        uid = g_lyr.for_uid;
        loading = g_lyr.loading;
    }

    const Rect area{box.x + 8, box.y + 46, box.w - 16, box.h - 60};

    // 歌词属于"当前正在播的这首"才算数；切歌瞬间新旧对不上时先显示加载态
    const bool usable = doc && !uid.empty() && uid == c.s.now_uid;

    if (!usable) {
        Style s;
        s.font_size = 11.5f;
        s.align = Align::Center;
        s.wrap_text();
        std::string msg;
        if (c.s.now_uid.empty()) msg = "播放一首歌，歌词会显示在这里";
        else if (loading || !doc) msg = "正在取歌词…";
        else msg = doc->error.empty() ? "这首歌没有歌词" : doc->error;
        c.ui.draw_text(msg, {area.x + 14, area.y + area.h * 0.3f, area.w - 28, 60}, c.th.label3, s);
        return;
    }
    if (!doc->error.empty()) {
        Style s;
        s.font_size = 11.5f;
        s.align = Align::Center;
        s.wrap_text();
        c.ui.draw_text(doc->error, {area.x + 14, area.y + area.h * 0.3f, area.w - 28, 60}, c.th.label3, s);
        return;
    }

    const float text_w = area.w - 26.0f;
    if (g_lyr_layout.owner != doc.get() || std::fabs(g_lyr_layout.width - text_w) > 0.5f)
        build_lyric_layout(c.ui, *doc, text_w);

    const int cur_line = doc->synced ? lyr::current_line(doc->lines, (long long)g_ui.pos_ms) : -1;
    int disp_cur = -1;
    if (cur_line >= 0) {
        for (int i = 0; i < int(g_lyr_layout.src.size()); ++i)
            if (g_lyr_layout.src[size_t(i)] == cur_line) { disp_cur = i; break; }
    }

    const float total = g_lyr_layout.total;
    const float max_scroll = std::max(0.0f, total - area.h + 40.0f);
    float target = g_lyr_scroll;
    // 当前行放**正中间**：之前是 area.h * 0.34（贴上方），看着像没对准。
    if (disp_cur >= 0) target = g_lyr_layout.top[size_t(disp_cur)] - (area.h - 23.0f) * 0.5f;
    target = std::min(std::max(target, 0.0f), max_scroll);

    // 缓动按**时间**算，不按帧数：30fps 和 60fps 下的手感必须一致。
    // 之前写死 "每帧靠近 25%"，帧率一掉就变成"一格一格跳"，这就是卡顿感的来源。
    static double last_ms = 0.0;
    const double now_ms = double(GetTickCount64());
    float dt = last_ms > 0.0 ? float((now_ms - last_ms) / 1000.0) : 0.016f;
    last_ms = now_ms;
    if (dt > 0.06f) dt = 0.06f;   // 卡了一帧也别让歌词瞬移
    if (dt < 0.001f) dt = 0.001f;

    if (g_lyr_cur != cur_line) {
        g_lyr_cur = cur_line;
        g_anim_until.store(GetTickCount64() + 700);  // 这 700ms 走 60fps，滚完就回落
    }
    // 时间常数：切行瞬间 55ms（跟得紧），平常用 130ms（顺滑不抖）
    const float tau = (std::fabs(target - g_lyr_scroll) > area.h * 0.5f) ? 0.055f : 0.13f;
    const float k = 1.0f - std::exp(-dt / tau);
    g_lyr_scroll += (target - g_lyr_scroll) * k;
    if (std::fabs(target - g_lyr_scroll) < 0.5f) g_lyr_scroll = target;

    c.ui.push_clip(area);
    const float base_y = area.y - g_lyr_scroll;
    const int nline = int(g_lyr_layout.text.size());
    // 点歌词行的按下位置。用"按下与松手落在同一行"判定，而不是 ui.item()：
    // item 的 widget_id 含矩形位置，歌词每帧都在滚动 → id 每帧都变，
    // 点击后 active 常常清不掉，表现为"点十次能中一次"。
    static int press_line = -1;
    for (int i = 0; i < nline; ++i) {
        const float ty = base_y + g_lyr_layout.top[size_t(i)];
        // 行高取到下一行的顶部：整段行距都可点，不用瞄准文字那一条
        const float next_top = (i + 1 < nline) ? g_lyr_layout.top[size_t(i + 1)] : g_lyr_layout.total;
        const float row_h = std::max(26.0f, next_top - g_lyr_layout.top[size_t(i)]);
        if (ty + row_h < area.y || ty > area.bottom()) continue;

        const int sline = g_lyr_layout.src[size_t(i)];
        const bool active = (sline == cur_line);
        const Rect row{area.x + 4, ty - 3.0f, area.w - 8, row_h};
        const bool row_hover = row.contains(c.ui.input().mouse.x, c.ui.input().mouse.y);
        if (row_hover && !active) c.ui.fill_rect(row, c.th.fill3, 7.0f);

        const bool has_time = doc->lines[size_t(sline)].ms >= 0;
        if (row_hover && c.ui.input().pressed[0]) press_line = i;
        if (row_hover && c.ui.input().released[0] && press_line == i && has_time) {
            g_player.seek_ms(int(doc->lines[size_t(sline)].ms));
            g_ui.pos_ms = float(doc->lines[size_t(sline)].ms);
            g_lyr_cur = -2;
            press_line = -1;
        }
        Style s;
        s.font_size = active ? 13.5f : 12.5f;
        s.font_weight = active ? 600 : 400;
        s.align = Align::Center;   // 歌词居中，左对齐在抽屉里很难看
        c.ui.draw_text(g_lyr_layout.text[size_t(i)], {area.x, ty, area.w, 22},
                       active ? c.th.tint : c.th.label2, s);
    }
    if (c.ui.input().released[0]) press_line = -1;
    c.ui.pop_clip();

    // 上下渐隐：滚动时行会被 clip 硬切成半截字，用十几条渐变把边缘化掉。
    // 纯色 + alpha 叠加，不依赖任何混合模式，成本约 24 个矩形。
    {
        const int steps = 10;
        const float fh = 26.0f;
        const float sh = fh / float(steps);
        for (int i = 0; i < steps; ++i) {
            const float a = 0.92f * (1.0f - float(i) / float(steps));
            c.ui.fill_rect({area.x, area.y + float(i) * sh, area.w, sh + 0.6f},
                           c.th.control_bg.scaled_alpha(a));
            c.ui.fill_rect({area.x, area.bottom() - float(i + 1) * sh, area.w, sh + 0.6f},
                           c.th.control_bg.scaled_alpha(a));
        }
    }

    // 底部来源小字
    Style f;
    f.font_size = 9.5f;
    f.valign = VAlign::Middle;
    c.ui.draw_text(doc->origin.empty() ? std::string() : ("歌词来自 " + doc->origin),
                   {box.x + 20, box.bottom() - 26, box.w - 40, 16}, c.th.label4, f);
}

static void compose(aino::Ui& ui, Ctx& c) {
    ui.fill_rect({0, 0, c.W, c.H}, c.th.window_bg);
    sidebar(c);

    switch (g_ui.tab) {
        case 0: tab_search(c); break;
        case 1: tab_discover(c); break;
        case 2: tab_queue(c); break;
        case 3: tab_playlists(c); break;
        case 4: tab_sources(c); break;
        default: tab_settings(c); break;
    }
    if (g_ui.lyrics_open) {
        lyrics_panel(c, {c.W - kLyricsWidth, 0, kLyricsWidth, c.bar_y});
    }
    player_bar(c);
    draw_toast(c);
}

}  // namespace ui_draw

// =============================================================================
//  六、配置持久化
// =============================================================================
// 只存三件事：音量、启用的音源、两个 Cookie。
// 格式是手写 ini —— 不值得为这点东西引入解析器，也不该把 Cookie 弄成二进制。

static std::string config_path() {
    wchar_t buf[MAX_PATH] = {0};
    if (GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH) == 0) return std::string();
    const std::string dir = net::narrow(buf) + "\\aino-player\\";
    CreateDirectoryW(net::widen(dir).c_str(), nullptr);
    return dir + "config.ini";
}

static std::string read_line_value(const std::string& line, const char* key) {
    const std::string k(key);
    if (line.rfind(k + "=", 0) != 0) return std::string();
    return line.substr(k.size() + 1);
}

static void load_config() {
    const std::string path = config_path();
    if (path.empty()) return;
    std::FILE* f = _wfopen(net::widen(path).c_str(), L"rb");  // 用户名可能是中文
    if (!f) return;
    char buf[4096];
    bool has_version = false;
    while (std::fgets(buf, sizeof buf, f)) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::string v = read_line_value(line, "volume");
        if (!v.empty()) g_ui.volume = float(std::atof(v.c_str()));
        v = read_line_value(line, "sources");
        if (!v.empty()) {
            const uint32_t m = uint32_t(std::strtoul(v.c_str(), nullptr, 10)) & 0x7u;
            g_ui.src_mask = m ? m : 0x3u;   // 全关等于搜不出东西，退回默认的网易云 + QQ
        }
        v = read_line_value(line, "netease_cookie");
        if (!v.empty()) g_ui.settings.netease_cookie = v;
        v = read_line_value(line, "qq_cookie");
        if (!v.empty()) g_ui.settings.qq_cookie = v;
        v = read_line_value(line, "download_dir");
        if (!v.empty()) g_ui.download_dir = v;
        v = read_line_value(line, "qq_uin");
        if (!v.empty()) g_ui.qq_uin = v;
        v = read_line_value(line, "netease_uid");
        if (!v.empty()) g_ui.ne_uid = v;
        v = read_line_value(line, "dark");
        if (!v.empty()) g_ui.dark = (v != "0");
        v = read_line_value(line, "quality");
        if (!v.empty()) g_ui.settings.quality = std::atoi(v.c_str());
        v = read_line_value(line, "embed_cover");
        if (!v.empty()) g_ui.settings.embed_cover = (v != "0");
        v = read_line_value(line, "embed_lyrics");
        if (!v.empty()) g_ui.settings.embed_lyrics = (v != "0");
        v = read_line_value(line, "loop_list");
        if (!v.empty()) g_ui.loop_list = (v != "0");
        v = read_line_value(line, "config_version");
        if (!v.empty()) has_version = std::atoi(v.c_str()) >= 2;
    }
    std::fclose(f);

    // 老配置里三个源都是开的。这一版把酷我改成默认关（受限曲目只给 11 秒试听，
    // 留着它会让人以为"歌手没唱完"），所以迁移一次：只做一次，之后用户自己开就保留。
    if (!has_version) {
        if (g_ui.src_mask & (1u << 2)) {
            g_ui.src_mask &= ~(1u << 2);
            g_ui.config_dirty = true;
        }
        g_ui.config_dirty = true;   // 让新版本号落盘
    }
}

static void save_config() {
    const std::string path = config_path();
    if (path.empty()) return;
    std::FILE* f = _wfopen(net::widen(path).c_str(), L"wb");
    if (!f) return;
    std::fprintf(f, "# aino player 配置（Cookie 明文放在本机，介意就别填）\n");
    std::fprintf(f, "config_version=2\n");
    std::fprintf(f, "volume=%d\n", int(g_ui.volume));
    std::fprintf(f, "sources=%u\n", g_ui.src_mask);
    std::fprintf(f, "netease_cookie=%s\n", g_ui.settings.netease_cookie.c_str());
    std::fprintf(f, "qq_cookie=%s\n", g_ui.settings.qq_cookie.c_str());
    std::fprintf(f, "download_dir=%s\n", g_ui.download_dir.c_str());
    std::fprintf(f, "qq_uin=%s\n", g_ui.qq_uin.c_str());
    std::fprintf(f, "netease_uid=%s\n", g_ui.ne_uid.c_str());
    std::fprintf(f, "dark=%d\n", g_ui.dark ? 1 : 0);
    std::fprintf(f, "quality=%d\n", g_ui.settings.quality);
    std::fprintf(f, "embed_cover=%d\n", g_ui.settings.embed_cover ? 1 : 0);
    std::fprintf(f, "embed_lyrics=%d\n", g_ui.settings.embed_lyrics ? 1 : 0);
    std::fprintf(f, "loop_list=%d\n", g_ui.loop_list ? 1 : 0);
    std::fclose(f);
    g_ui.config_dirty = false;
}

// =============================================================================
//  六、主循环
// =============================================================================

int main() {
    // MTA 而不是 STA：IMFMediaEngine 只能在多线程套间里创建（STA 下 CreateInstance 直接失败）。
    // WebView2 的登录窗口在另一个线程里自己 CoInitialize 成 STA，互不影响。
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // 首个命令行参数可以作为初始关键词（便于测试和"带词启动"）
    if (const std::wstring cmd = GetCommandLineW(); !cmd.empty()) {
        const size_t q0 = cmd.find(L'"');
        size_t i = 0;
        if (q0 == 0) {
            i = cmd.find(L'"', 1);
            i = (i == std::wstring::npos) ? cmd.size() : i + 1;
        } else {
            i = cmd.find(L' ');
            if (i == std::wstring::npos) i = cmd.size();
        }
        while (i < cmd.size() && cmd[i] == L' ') ++i;
        if (i < cmd.size()) {
            std::wstring arg;
            if (cmd[i] == L'"') {
                ++i;
                while (i < cmd.size() && cmd[i] != L'"') arg += cmd[i++];
            } else {
                while (i < cmd.size() && cmd[i] != L' ') arg += cmd[i++];
            }
            if (!arg.empty()) g_ui.query = net::narrow(arg);
        }
    }

    load_config();

    // 缓存目录放在 %TEMP%/aino-player/ 下
    wchar_t tmp[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tmp);
    g_temp_dir = net::narrow(tmp) + "aino-player\\";
    CreateDirectoryW(net::widen(g_temp_dir).c_str(), nullptr);

    aino::WindowConfig cfg;
    cfg.width = 1180;
    cfg.height = 760;
    cfg.min_width = 980;
    cfg.min_height = 640;
    cfg.title = "aino player";
    cfg.center = true;
    aino::Window win(cfg);
    if (!win.ok()) {
        MessageBoxW(nullptr, L"窗口创建失败", L"aino player", MB_ICONERROR);
        return 1;
    }
    g_win = &win;
    g_covers.set_notifier([&win] { win.wake(); });
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
    // 高 DPI 下逻辑尺寸会超屏：创建后按工作区再夹一次，否则底栏会被屏幕边缘截掉
    {
        MONITORINFO mi{};
        mi.cbSize = sizeof mi;
        if (GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            const float sc = win.dpi_scale() > 0.1f ? win.dpi_scale() : 1.0f;
            const int max_w = int(float(mi.rcWork.right - mi.rcWork.left) / sc * 0.96f);
            const int max_h = int(float(mi.rcWork.bottom - mi.rcWork.top) / sc * 0.92f);
            const int w = std::min(cfg.width, max_w);
            const int h = std::min(cfg.height, max_h);
            if (w != cfg.width || h != cfg.height) win.set_size(w, h);
        }
    }
    bool theme_dark = g_ui.dark;
    win.set_theme(theme_dark ? aino::Theme::dark() : aino::Theme::light());
    win.set_anim_fps(60.0f);

    // 播放/缓冲/检索期间的刷新泵：只在需要时 wake()，其余时间主循环睡着
    std::thread([] {
        unsigned long long toast_seen = 0;
        while (!g_shutdown.load()) {
            bool busy = false;
            {
                std::lock_guard<std::mutex> lk(g_mu);
                busy = g_as.searching || g_as.ps == PlayState::Playing || g_as.ps == PlayState::Buffering ||
                       g_as.ps == PlayState::Resolving;
            }
            const unsigned long long now = GetTickCount64();
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
            Sleep(wait);
        }
    }).detach();

    Snapshot cached;
    bool autostart = true;
    const auto frame_start = std::chrono::steady_clock::now();

    win.run([&](aino::Ui& ui) {
        const auto t0 = std::chrono::steady_clock::now();
        // 主题变了要立刻换：set_theme 会在本帧生效，下一帧就是新配色
        if (g_ui.dark != theme_dark) {
            theme_dark = g_ui.dark;
            ui.set_theme(theme_dark ? aino::Theme::dark() : aino::Theme::light());
        }
        Snapshot snap = take_snapshot(cached);
        cached = snap;
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

        // 登录窗口回传（QQ / 网易云各一套）：成功就把 cookie 存起来并立刻拉歌单
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
        if (const int goto_tab = g_goto_tab.exchange(-1); goto_tab >= 0) g_ui.tab = goto_tab;

        // ---- 后台交过来的"可以播了"信号，在主线程打开 MCI ----
        if (snap.ps == PlayState::Ready && !snap.pending_path.empty()) {
            if (g_player.open(snap.pending_path)) {
                g_player.set_volume(int(g_ui.volume));
                g_player.play();
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
                    g_as.error = "MCI 打不开音频（系统缺少 Media Features？）";
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
            if (snap.ps == PlayState::Playing && !g_player.is_playing() && g_ui.dur_ms > 1000 &&
                g_ui.pos_ms > g_ui.dur_ms - 600) {
                play_step(1);
            }
        }

        if (autostart) {  // 打开就有东西看，别让首屏空着
            autostart = false;
            queue_load();          // 上次的队列先恢复
            if (g_as.tracks.empty()) start_search();
            disc_refresh(g_ui.disc_site);   // 发现页数据提前拉，切过去就是现成的
        }

        // ---- 队列落盘（延时 1.2s，连续操作只写一次）----

        ui_draw::Ctx c{ui, ui.theme(), 0, 0, 0, snap};
        c.W = ui.width();
        c.H = ui.height();
        c.bar_y = c.H - kBarHeight;
        ui_draw::compose(ui, c);

        // ---- UI 侧的小改动回写 ----
        if (g_ui.chosen_override_track >= 0) {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_ui.chosen_override_track < int(g_as.tracks.size())) {
                g_as.tracks[size_t(g_ui.chosen_override_track)].chosen = g_ui.chosen_override_index;
            }
            g_ui.chosen_override_track = -1;
            g_ui.chosen_override_index = -1;
        }

        // Cookie / 音量 / 音源开关变了就落盘，节流到 2 秒一次，别每帧写文件
        if (g_ui.config_dirty) {
            const double now_s = double(GetTickCount64()) / 1000.0;
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

        const auto t1 = std::chrono::steady_clock::now();
        const float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
        g_ui.frame_ms = g_ui.frame_ms * 0.9f + ms * 0.1f;  // 指数平滑，别让数字乱跳
        g_frame_ms_x10.store(int(g_ui.frame_ms * 10.0f));
    });

    g_shutdown.store(true);
    g_player.close();
    save_config();
    queue_save();   // 退出前把队列定死，别指望最后那一帧的延时落盘
    (void)frame_start;
    // 给后台线程一点时间看到退出标志，避免进程退出时它们还在改状态
    Sleep(120);
    return 0;
}
