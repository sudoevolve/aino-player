// 界面：布局、绘制与输入；业务操作统一调用 business.hpp 的动作。
#pragma once
#include "business.hpp"
#include "aino.hpp"
#include "sources.hpp"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <map>
namespace det {

constexpr float kSidebar = 216.0f;
constexpr float kBarHeight = 88.0f;
constexpr float kPanelWidth = 320.0f;
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

inline aino::Color source_color(music::Source s) {
    switch (s) {
        case music::Source::Netease: return aino::Color::hex(0xD93A3A);
        case music::Source::QQ: return aino::Color::hex(0x1F9D6B);
        case music::Source::Kuwo: return aino::Color::hex(0x3E63DD);
        case music::Source::YouTube: return aino::Color::hex(0xE33F44);
        case music::Source::Bilibili: return aino::Color::hex(0xD95783);
        default: return aino::Color::hex(0x75808A);
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

// 使用同一组矢量线条绘制轮廓和弧形声波，跨平台不依赖符号字体。
inline void draw_speaker(aino::Ui& ui, const aino::Rect& box, const aino::Color& c, float volume) {
    const float scale = std::min(box.w, box.h) / 24.0f;
    const float ox = box.x + (box.w - scale * 24) * 0.5f;
    const float oy = box.y + (box.h - scale * 24) * 0.5f;
    auto line = [&](float x0, float y0, float x1, float y1) {
        ui.draw_line(ox + x0 * scale, oy + y0 * scale, ox + x1 * scale, oy + y1 * scale, c, 1.7f * scale);
    };
    const float outline[][2] = {{2,8},{6,8},{11,4},{11,20},{6,16},{2,16},{2,8}};
    for (int i = 1; i < 7; ++i) line(outline[i-1][0], outline[i-1][1], outline[i][0], outline[i][1]);
    if (volume <= 0) {
        line(16,9,22,15); line(16,15,22,9);
        return;
    }
    for (int wave = 0; wave < (volume > 50 ? 2 : 1); ++wave) {
        const float radius = wave == 0 ? 6.0f : 10.0f;
        for (int i = 0; i < 12; ++i) {
            const float a = -0.72f + 1.44f * float(i) / 12;
            const float b = -0.72f + 1.44f * float(i + 1) / 12;
            line(11 + radius * std::cos(a), 12 + radius * std::sin(a),
                 11 + radius * std::cos(b), 12 + radius * std::sin(b));
        }
    }
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

} // namespace det
inline unsigned long long ui_ticks() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

namespace player::ui {
using namespace det;

using namespace aino;

struct Ctx {
    Ui& ui;
    const Theme& th;
    float W = 0;
    float H = 0;
    float bar_y = 0;
    const Snapshot& s;
    ViewModel& model;
};
static float right_panel_width(const Ctx& c) {
    return c.model.ui.lyrics_open || c.model.ui.queue_open ? kPanelWidth : 0;
}

struct SongMenuState {
    bool active = false;
    music::Track track;
    Rect anchor;
    Vec2 position;
};
static SongMenuState song_menu;
static void capture_song_menu(Ctx& c, const music::Track& track, const Rect& row, const Rect& viewport) {
    const auto& in = c.ui.input();
    const auto visible = row.intersect(viewport);
    if (!song_menu.active && c.ui.pointer_available() && in.pressed[1] && visible.contains(in.mouse.x, in.mouse.y) && !track.variants.empty()) {
        // 菜单不依赖虚拟列表的行索引；滚动、搜索刷新后仍操作右键时的歌曲。
        song_menu = {true, track, visible, in.mouse};
    }
}
static void draw_song_menu(Ctx& c) {
    if (!song_menu.active) return;
    const auto download = c.model.downloads.find(track_uid(song_menu.track));
    const bool downloading = download != c.model.downloads.end() && !download->second.done && !download->second.failed;
    const std::vector<std::string> items{"下一首播放", "添加到播放列表",
        c.model.library.contains(song_menu.track) ? "取消收藏" : "收藏歌曲", downloading ? "取消下载" : "下载歌曲"};
    int chosen = -1;
    const bool picked = c.ui.context_menu(song_menu.anchor, items, &chosen, Style().font(12));
    // 与原生菜单使用相同的边界，关闭后释放快照；不在按下菜单项时提前关闭。
    float width = 120;
    for (const auto& item : items) width = std::max(width, c.ui.text_width(item, 12, 500) + 36);
    const float height = 24 * float(items.size()) + 8;
    const Rect menu{std::clamp(song_menu.position.x, 4.0f, std::max(4.0f, c.W - width - 4)),
                    std::clamp(song_menu.position.y, 4.0f, std::max(4.0f, c.H - height - 4)), width, height};
    const auto& in = c.ui.raw_input();
    if (picked) {
        const auto track = song_menu.track;
        if (chosen == 0) queue_play_next(track);
        else if (chosen == 1) toast(queue_push(track) ? "已添加到播放列表：" + track.title : "播放列表中已有这首歌曲");
        else if (chosen == 2) favorite_toggle(track);
        else if (chosen == 3) { if (downloading) cancel_download(track_uid(track)); else start_download(track, c.model.ui.settings); }
    }
    if (picked || in.key_pressed[KEY_ESC] || ((in.pressed[0] || in.pressed[1]) && !menu.contains(in.mouse.x, in.mouse.y)))
        song_menu.active = false;
}

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
    const bool available = c.ui.pointer_available();
    const bool inside = available && box.outset(6.0f).contains(in.mouse.x, in.mouse.y);
    if (!available || !enabled) st->dragging = false;

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
            commit = in.released[0];
            st->dragging = false;
        }
    }

    // 松手这一帧调用方尚未写回 value，必须显示刚提交的位置，不能画回旧值。
    const float shown = std::min(max_value, std::max(0.0f, st->dragging || commit ? st->drag_pos : value));
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

    if (commit) {
        if (committed) *committed = st->drag_pos;
        c.ui.request_animation_frame(); // 补绘一次，显示调用方更新后的音量文字、图标和位置。
    }
    return commit;
}

// 有封面就画封面，没有就画渐变。图片解码与下载都在 cover::Store 里异步进行，
// 这里每帧只是一次 map 查询 —— 没就绪就先显示渐变，到货后自动换过来。
static void cover_art(Ctx& c, const std::string& url, const std::string& seed, const Rect& box, float radius) {
    if (!url.empty()) {
        if (auto img = cover_image(url); img && img->w > 0) {
            c.ui.draw_bitmap(box, img->rgba.data(), img->w, img->h, radius);
            return;
        }
    }
    procedural_cover(c, seed, box, radius);
}

// ---------- 侧栏 ----------
static void draw_brand(Ctx& c, const Rect& box) {
    const auto& icon = brand_icon();
    if (!icon.rgba.empty()) c.ui.draw_bitmap(box, icon.rgba.data(), icon.w, icon.h);
}
static void sidebar(Ctx& c) {
    const float w = kSidebar;
    c.ui.fill_rect({0, 0, w, c.H}, c.th.sidebar_bg);
    c.ui.fill_rect({w - 1, 0, 1, c.H}, c.th.separator);

    Style logo;
    logo.font_size = 19.0f;
    logo.font_weight = 700;
    draw_brand(c, {18, 17, 34, 34});
    c.ui.draw_text("aino", {60, 20, 80, 26}, c.th.label, logo);
    Style sub;
    sub.font_size = 10.5f;
    sub.font_weight = 600;
    const float lw = c.ui.text_width("aino", 19.0f, 700);
    c.ui.draw_text("PLAYER", {60 + lw + 7, 26, 80, 16}, c.th.label3, sub);

    const char* navs[7] = {"发现", "在线搜索", "歌单", "最近播放", "下载管理", "音源", "设置"};
    const int tabs[7] = {1, 0, 3, 7, 6, 4, 5};
    const SystemIcon icons[7] = {SystemIcon::Star, SystemIcon::Search, SystemIcon::People, SystemIcon::Timer, SystemIcon::Download, SystemIcon::Network, SystemIcon::Settings};
    for (int i = 0; i < 7; ++i) {
        const Rect box{12, 66.0f + float(i) * 32.0f, w - 24, 30};
        if (c.ui.nav_item(navs[i], c.model.ui.tab == tabs[i], box, Style().padded(34, 0))) c.model.ui.tab = tabs[i];
        c.ui.system_icon(icons[i], {box.x + 11, box.y + 7, 16, 16}, c.model.ui.tab == tabs[i] ? c.th.tint : c.th.label2);
    }

    c.ui.fill_rect({20, 308, w - 40, 1}, c.th.separator);

    Style sec;
    sec.font_size = 10.5f;
    sec.font_weight = 600;
    c.ui.draw_text("音源状态", {22, 324, 120, 16}, c.th.label2, sec);

    for (int i = 0; i < music::kSourceCount; ++i) {
        const float y = 346.0f + float(i) * 26.0f;
        const bool on = (c.model.ui.src_mask >> i) & 1u;
        c.ui.fill_circle(26, y + 8, 3.5f, on ? source_color(music::Source(i)) : c.th.label4);

        Style name;
        name.font_size = 12.0f;
        name.font_weight = 500;
        c.ui.draw_text(music::source_name(music::Source(i)), {38, y, 72, 16}, on ? c.th.label : c.th.label3,
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
        c.ui.draw_text(ellipsize(c.ui, tail, 76, 10.5f), {w - 94, y, 74, 16}, c.th.label3, ts);
    }

    // 自报数字：帧耗时 + 缓存（不写形容词，写数字）
    char buf[128];
    std::snprintf(buf, sizeof buf, "帧 %.1fms · 缓存 %s", c.model.ui.frame_ms, fmt_size(c.s.cached_bytes).c_str());
    Style foot;
    foot.font_size = 10.0f;
    const float foot_y = c.bar_y - 34.0f;   // 必须在播放条上沿之上，否则被它盖住
    c.ui.draw_text("aino player · 多音源", {22, foot_y, w - 44, 14}, c.th.label4, foot);
    c.ui.draw_text(buf, {22, foot_y + 16, w - 44, 14}, c.th.label3, foot);
}

// ---------- 结果行 ----------
static void result_row(Ctx& c, const music::Track& t, int index, const Rect& box, bool is_playing, bool playlist_mode = false) {
    const int nvar = int(t.variants.size());
    const float dur_w = 50.0f;
    const float dl_w = 26.0f;
    const float q_w = 26.0f;   // 「加入队列」占位，和下载按钮并排
    const float fav_w = 26.0f;
    const bool has_parts = std::any_of(t.variants.begin(), t.variants.end(), [](const auto& v) {
        return v.source == music::Source::Bilibili && v.native_id.empty();
    });
    const float parts_w = has_parts ? 46.0f : 0.0f;
    const float pill_h = 18.0f;
    const float mid_y = box.y + box.h * 0.5f;

    float pills_w = 0;
    for (const auto& v : t.variants)
        pills_w += tw_cached(c.ui, music::source_name(v.source), 10.5f) + 16.0f + 4.0f;

    const bool compact_sources = box.w - 76 - pills_w - dur_w - dl_w - q_w - fav_w - parts_w - 18 < 120;
    if (compact_sources) pills_w = 44;

    // ---- 1) 先把右侧的小控件"认领"掉，再画整行 ----
    // aino 的按下只属于**第一个**包含指针的控件（press_taken）。整行的 item 若先画，
    // 这些按钮就永远收不到点击 —— 症状是"点下载 / 点药丸，结果重新播放了整行"。
    const Rect dl{box.right() - dur_w - 6 - dl_w + 2, mid_y - 11.0f, 22.0f, 22.0f};
    const Rect qa{dl.x - q_w, mid_y - 11.0f, 22.0f, 22.0f};
    const Rect favorite{qa.x - fav_w, mid_y - 11.0f, 22.0f, 22.0f};
    const bool saved = c.model.library.contains(t);
    const bool favorite_clicked = c.ui.item("", "", "", favorite, Style().corner(6));
    const bool parts_clicked = has_parts && c.ui.button("分 P", {favorite.x - parts_w, mid_y - 12, 42, 24}, Style().font(10.5f));
    int pct = -1;
    bool dl_done = false, dl_failed = false;
    const std::string uid = track_uid(t);
    if (auto it = c.model.downloads.find(uid); it != c.model.downloads.end()) {
        pct = it->second.percent;
        dl_done = it->second.done;
        dl_failed = it->second.failed;
    }
    const bool row_hover = c.ui.pointer_available() && box.contains(c.ui.input().mouse.x, c.ui.input().mouse.y);
    const bool dl_show = row_hover || dl_done || dl_failed || pct >= 0;
    bool dl_clicked = false;
    if (dl_show && c.ui.item("", "", "", dl, Style().corner(6))) dl_clicked = true;
    bool qa_clicked = false;
    if (row_hover && c.ui.item("", "", "", qa, Style().corner(6))) qa_clicked = true;

    const int chosen = std::min<int>(t.chosen, std::max(0, nvar - 1));
    Rect pill_rect[6];
    int pill_n = 0;
    {
        float px = box.right() - dur_w - 6 - dl_w - q_w - fav_w - parts_w - pills_w;
        for (int i = 0; !compact_sources && i < nvar && i < 6; ++i) {
            const float w = tw_cached(c.ui, music::source_name(t.variants[size_t(i)].source), 10.5f) + 16.0f;
            pill_rect[pill_n++] = Rect(px, mid_y - pill_h * 0.5f, w, pill_h);
            px += w + 4.0f;
        }
    }
    int pill_clicked = -1;
    if (compact_sources) {
        std::vector<std::string> names;
        for (const auto& variant : t.variants) names.emplace_back(music::source_name(variant.source));
        const Rect source_menu{favorite.x - parts_w - 50, mid_y - 12, 44, 24};
        c.ui.menu("源", names, &pill_clicked, source_menu, Style().font(10.5f));
    }
    for (int i = 0; i < pill_n; ++i)
        if (c.ui.item("", "", "", pill_rect[i], Style().corner(9))) pill_clicked = i;

    // ---- 2) 整行 ----
    const bool row_clicked = c.ui.item("", "", "", box, Style().corner(8).padded(0, 0));
    if (is_playing) c.ui.fill_rect(box, c.th.tint.scaled_alpha(0.15f), 8.0f);

    const Rect cov{box.x + 10, box.y + 10, 44, 44};
    cover_art(c, t.cover_url, t.title, cov, 7.0f);
    if (is_playing) {
        // 垫一层暗底：封面底色偏浅时白条会"消失"（七里香那张就是白底图）
        const Rect ind{cov.right() - 17.0f, cov.y + 3.0f, 14.0f, 14.0f};
        c.ui.fill_rect(ind, Color(0, 0, 0, 120), 4.0f);
        draw_playing_bars(c.ui, {ind.x + 2.5f, ind.y + 3.0f, 9.0f, 8.0f}, Color(255, 255, 255, 240));
    }

    const float text_x = cov.right() + 12;
    const float max_text = std::max(20.0f, box.w - (text_x - box.x) - pills_w - dur_w - dl_w - q_w - fav_w - parts_w - 18);

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
    if (saved) c.ui.fill_rect(favorite, c.th.tint.scaled_alpha(0.12f), 6);
    c.ui.system_icon(SystemIcon::Star, favorite.center(13, 13), saved ? c.th.tint : c.th.label3);

    Style dur;
    dur.font_size = 11.0f;
    dur.align = Align::Right;
    c.ui.draw_text(fmt_time(t.duration_ms), {box.right() - dur_w - 6, box.y, dur_w, box.h}, c.th.label3, dur);

    c.ui.fill_rect({box.x + 10, box.bottom() - 1, box.w - 20, 1}, c.th.separator.scaled_alpha(0.6f));

    if (dl_clicked) start_download(t, c.model.ui.settings);
    if (favorite_clicked) favorite_toggle(t);
    if (parts_clicked) bilibili_open_parts(t);
    if (qa_clicked) {
        // 「＋」= 追加到队列末尾（不覆盖当前列表）
        if (queue_push(t)) toast("已加入队列：" + t.title, 2600);
        else toast("队列里已经有这首了", 2600);
    }
    if (pill_clicked >= 0 && pill_clicked != chosen) {
        if (playlist_mode) playlist_choose_source(index, pill_clicked, is_playing);
        else choose_search_source(index, pill_clicked, is_playing);
    }
    if (row_clicked) {
        if (playlist_mode) playlist_play(index);
        else play_search_result(index);
    }
}

// ---------- 歌单页（网易云 / QQ 两个账号共用一套版式） ----------
// site：0 = 网易云，1 = QQ
static void playlist_row(Ctx& c, int site, int index, const Rect& box) {
    std::string name, cover;
    int count = 0;
    if (site == 3) {
        const auto& pl = c.model.youtube.playlists[size_t(index)];
        name = pl.name; cover = pl.cover; count = pl.count;
    } else if (site == 1) {
        const qq::Playlist& pl = c.model.qq.playlists[size_t(index)];
        name = pl.name;
        cover = pl.cover;
        count = pl.count;
    } else {
        const ne::Playlist& pl = c.model.netease.playlists[size_t(index)];
        name = pl.name;
        cover = pl.cover;
        count = pl.count;
    }
    const bool sync_here = site == 3 ? c.model.youtube.writing : (c.model.sync_site == site && c.model.sync_index == index);

    // 同步按钮必须先于整行绘制：aino 的"按下"只归第一个包含指针的控件，
    // 整行先画的话这个按钮永远收不到点击（结果就是"点同步变成了打开歌单"）。
    const Rect sync_btn{box.right() - 84, box.y + box.h * 0.5f - 13, 60, 26};
    const Rect save_btn{sync_btn.x - 88, sync_btn.y, 80, 26};
    const bool save_clicked = c.ui.button("保存到本地", save_btn, Style().font(10.5f));
    const bool hover = c.ui.pointer_available() && box.contains(c.ui.input().mouse.x, c.ui.input().mouse.y);
    const bool sync_clicked = c.ui.item("", "", "", sync_btn, Style().corner(7));

    const bool row_clicked = c.ui.item("", "", "", box, Style().corner(8));

    const Rect cov{box.x + 10, box.y + 10, 44, 44};
    cover_art(c, cover, name, cov, 7.0f);

    Style ts;
    ts.font_size = 13.5f;
    ts.font_weight = 600;
    const float max_w = box.w - 78 - 188;
    c.ui.draw_text(ellipsize(c.ui, name, max_w, 13.5f, 600), {cov.right() + 12, box.y + 11, max_w, 18},
                   c.th.label, ts);
    Style ds;
    ds.font_size = 11.0f;
    c.ui.draw_text(site == 3 && !count ? "YouTube Music 歌单" : std::to_string(count) + " 首", {cov.right() + 12, box.y + 33, max_w, 16}, c.th.label2, ds);

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

    if (save_clicked) library_save_remote(site, index);
    else if (sync_clicked) playlist_sync_to(site, index);
    else if (row_clicked) {
        if (site == 3) youtube_open_playlist(index);
        else if (site == 1) qq_open_playlist(index);
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
        const bool hovered = c.ui.pointer_available() && chip.contains(c.ui.input().mouse.x, c.ui.input().mouse.y);
        // 顺序很重要：先画底色和字，最后再交给 item 认领按压。
        // 反过来（item 先、底色后）时按压会被随后绘制的填充层吃掉，点不动。
        c.ui.fill_rect(chip, hovered ? c.th.tint.scaled_alpha(0.18f) : c.th.fill3, 12.0f);
        // 前三名给一点强调色：热榜的味道全在名次上
        const Color fg = (i < 3) ? c.th.tint : c.th.label2;
        c.ui.draw_text(w, chip, fg, cs);
        const bool hot = c.ui.item("", "", "", chip, Style().corner(12));
        if (hot) {
            c.model.ui.query = w;
            c.model.ui.tab = 0;
            start_search();
        }
        x += tw + gap;
    }
    *used_h = (y - box.y) + row_h;
}

static void discover_card_row(Ctx& c, int index, const Rect& box) {
    const disc::Card& cd = c.model.discovery.cards[size_t(index)];
    const bool save_clicked = c.ui.button("保存", {box.right() - 110, box.y + box.h * 0.5f - 12, 52, 24}, Style().font(10.5f));
    const bool hover = c.ui.pointer_available() && box.contains(c.ui.input().mouse.x, c.ui.input().mouse.y);
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
    if (save_clicked) {
        library::Playlist playlist;
        playlist.source = cd.source; playlist.remote_id = cd.id;
        playlist.key = std::string(music::source_key(cd.source)) + "#" + cd.id;
        playlist.name = cd.name; playlist.cover = cd.cover;
        library_fetch(std::move(playlist));
    } else if (clicked) disc_open(index);
}

static void tab_discover(Ctx& c) {
    const float mx = kSidebar + 24.0f;
    const float mw = c.W - mx - 24.0f - right_panel_width(c);

    c.ui.heading("发现", {mx, 20, mw, 30});
    int site = c.model.ui.disc_site;
    if (c.ui.segmented({"网易云", "QQ", "酷我"}, &site, {mx, 58, 226, 28}, Style().font(11.5f))) {
        c.model.ui.disc_site = site;
        disc_refresh(site);
    }
    if (c.ui.button("刷新", {mx + mw - 92, 58, 92, 28}, Style().font(11.5f))) disc_refresh(c.model.ui.disc_site);

    const DiscSnap& d = c.model.discovery;

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

static void queue_row(Ctx& c, const music::Track& track, int index, const Rect& box, bool current) {
    const bool hover = c.ui.pointer_available() && box.contains(c.ui.input().mouse.x, c.ui.input().mouse.y);
    const Rect remove{box.right() - 28, box.y + 19, 22, 22};
    const Rect favorite{remove.x - 26, remove.y, 22, 22};
    const bool saved = c.model.library.contains(track);
    const bool save_clicked = c.ui.icon_button("", favorite, Style().ghost());
    const bool remove_clicked = hover && c.ui.icon_button("", remove, Style().ghost());
    const bool clicked = c.ui.item("", "", "", box, Style().corner(6));
    if (current) c.ui.fill_rect(box, c.th.tint.scaled_alpha(0.08f), 6);
    const Rect cover{box.x + 8, box.y + 10, 40, 40};
    cover_art(c, track.cover_url, track.title, cover, 5);
    const float x = cover.right() + 10, width = std::max(1.0f, favorite.x - x - 6);
    c.ui.draw_text(ellipsize(c.ui, track.title, width, 12, 600), {x, box.y + 10, width, 19}, current ? c.th.tint : c.th.label, Style().font(12).weight(600));
    const auto detail = track.artist + (track.duration_ms > 0 ? " · " + fmt_time(track.duration_ms) : "");
    c.ui.muted_text(ellipsize(c.ui, detail, width, 10.5f), {x, box.y + 33, width, 17}, Style().font(10.5f));
    if (saved || hover) c.ui.system_icon(SystemIcon::Star, favorite.center(13, 13), saved ? c.th.tint : c.th.label3);
    if (hover) c.ui.system_icon(SystemIcon::Close, remove.center(10, 10), c.th.label3);
    if (save_clicked) favorite_toggle(track);
    else if (remove_clicked) queue_remove(index);
    else if (clicked) start_play(index);
}
struct QueueDrag { std::string uid; int target = -1; float last_scroll = 0; };
static QueueDrag queue_drag;
static void queue_panel(Ctx& c, const Rect& box) {
    const int count = int(c.s.tracks.size());
    c.ui.muted_text(std::to_string(count) + " 首 · " + play_mode_name(c.model.ui.play_mode), {box.x + 16, box.y + 8, box.w - 92, 22}, Style().font(10.5f));
    Style clear = Style().ghost().font(10.5f); clear.disabled = count == 0;
    if (c.ui.button("清空", {box.right() - 68, box.y + 5, 52, 26}, clear)) queue_clear();
    if (!count) {
        queue_drag = {};
        c.ui.draw_text("待播清单为空", {box.x + 24, box.y + 98, box.w - 48, 24}, c.th.label2, Style().font(13).weight(600).center());
        c.ui.muted_text("右键歌曲可选择下一首播放\n或添加到播放列表", {box.x + 24, box.y + 132, box.w - 48, 56}, Style().font(11).center().wrap_text());
        return;
    }
    const Rect list{box.x + 8, box.y + 42, box.w - 16, box.h - 50};
    if (list.h < 30) return;
    const auto& drag_input = c.ui.input();
    const bool drag_available = c.ui.pointer_available();
    if (!queue_drag.uid.empty() && drag_available && drag_input.down[0] && c.ui.time() - queue_drag.last_scroll > 0.09f) {
        const float offset = c.ui.scroll_offset(list);
        const int first = std::max(0, int(offset / 60));
        if (drag_input.mouse.y < list.y + 24) c.ui.scroll_to_index(list, std::max(0, first - 1), 60, count);
        else if (drag_input.mouse.y > list.bottom() - 24) c.ui.scroll_to_index(list, first + 1, 60, count);
        queue_drag.last_scroll = c.ui.time();
    }
    c.ui.scroll(list, count, 60, [&](int index, const Rect& row) {
        const auto& track = c.s.tracks[size_t(index)];
        const Rect handle{row.x, row.y, 16, row.h};
        c.ui.item("", "", "", handle, Style().corner(4));
        c.ui.system_icon(SystemIcon::Menu, handle.center(10, 12), c.th.label3);
        c.ui.tooltip(handle, "拖动调整队列顺序");
        const auto& input = c.ui.input();
        if (c.ui.pointer_available() && handle.contains(input.mouse.x, input.mouse.y) && input.pressed[0]) queue_drag = {track_uid(track), index};
        if (!queue_drag.uid.empty() && c.ui.pointer_available() && row.contains(input.mouse.x, input.mouse.y)) queue_drag.target = index;
        if (!queue_drag.uid.empty() && queue_drag.target == index) c.ui.fill_rect({row.x, row.y, row.w, 2}, c.th.tint);
        const Rect song{row.x + 16, row.y, row.w - 16, row.h};
        capture_song_menu(c, track, song, list);
        queue_row(c, track, index, song, track_uid(track) == c.s.now_uid);
    });
    const auto& input = c.ui.input();
    if (!queue_drag.uid.empty()) {
        if (!c.ui.pointer_available() || (!input.down[0] && !input.released[0])) queue_drag = {};
        else if (input.released[0]) {
            for (int i = 0; i < count; ++i) if (track_uid(c.s.tracks[size_t(i)]) == queue_drag.uid) { queue_move(i, queue_drag.target); break; }
            queue_drag = {};
        }
        else c.ui.request_animation_frame();
    }
}


static void local_playlist_row(Ctx& c, int index, const Rect& box) {
    // 先完成绘制再执行动作；动作可能替换资料库，此后不再访问 playlist。
    const auto& playlist = c.model.library.playlists[size_t(index)];
    const Rect remove{box.right() - 36, box.y + 19, 24, 24};
    const Rect refresh{remove.x - 62, box.y + 17, 54, 28};
    bool remove_clicked = false, refresh_clicked = false;
    if (index > 0) remove_clicked = c.ui.item("", "", "", remove, Style().corner(6));
    if (playlist.online() || !playlist.local_folder.empty()) refresh_clicked = c.ui.button("更新", refresh, Style().font(10.5f));
    const bool open_clicked = c.ui.item("", "", "", box, Style().corner(8));
    const Rect cov{box.x + 10, box.y + 10, 44, 44};
    cover_art(c, playlist.cover, playlist.name, cov, 7);
    const float tx = cov.right() + 12, width = std::max(20.0f, box.right() - tx - 120);
    c.ui.draw_text(ellipsize(c.ui, playlist.name, width, 13.5f, 600), {tx, box.y + 11, width, 18}, c.th.label, Style().font(13.5f).semi_bold());
    const std::string meta = std::to_string(playlist.tracks.size()) + " 首 · " +
        (playlist.online() ? std::string(music::source_name(playlist.source)) + " · 已保存曲目快照" : (!playlist.local_folder.empty() ? "本地文件夹 · 可重新扫描" : "保存在本机"));
    c.ui.draw_text(ellipsize(c.ui, meta, width, 10.5f), {tx, box.y + 34, width, 16}, c.th.label3, Style().font(10.5f));
    if (index > 0) c.ui.system_icon(SystemIcon::Close, remove.center(12, 12), c.th.label3);
    c.ui.fill_rect({box.x + 10, box.bottom() - 1, box.w - 20, 1}, c.th.separator.scaled_alpha(0.6f));
    if (remove_clicked) {
        library_remove(size_t(index));
    } else if (refresh_clicked) {
        if (!playlist.local_folder.empty()) library_import_folder(playlist.local_folder, playlist.recursive);
        else library_fetch(playlist);
    }
    else if (open_clicked) library_open(size_t(index));
}

static void playlist_detail(Ctx& c) {
    const float mx = kSidebar + 24.0f;
    const float mw = c.W - mx - 24.0f - right_panel_width(c);
    const auto& view = c.model.playlist;
    const auto& playlist = view.playlist;
    const char* back = view.return_tab == 0 ? "返回搜索" : view.return_tab == 1 ? "返回发现" : view.return_tab == 2 ? "返回队列" : "返回歌单";
    if (c.ui.button(back, {mx, 20, 88, 28}, Style().font(11.5f))) {
        playlist_close(); return;
    }
    c.ui.heading(ellipsize(c.ui, playlist.name, mw, 22, 600), {mx, 66, mw, 32});
    const auto origin = !playlist.local_folder.empty() ? "本地文件夹" : playlist.online() ?
        std::string(music::source_name(playlist.source)) + (view.saved ? " · 已保存快照" : playlist.source == music::Source::Bilibili ? " · 视频分 P" : " · 在线歌单") : "本地歌单";
    c.ui.muted_text(origin + " · " + std::to_string(playlist.tracks.size()) + " 首", {mx, 106, mw, 18}, Style().font(11.5f));
    if (view.loading) { c.ui.spinner({mx + mw / 2 - 11, 174, 22, 22}); return; }
    if (!view.error.empty()) { c.ui.empty_state("歌单加载失败", view.error, {mx, 158, mw, 140}); return; }
    if (c.ui.button("播放全部", {mx, 142, 92, 30}, Style().primary().font(11.5f))) playlist_play(-1);
    if (!view.saved && c.ui.button("保存到本地", {mx + 104, 142, 100, 30}, Style().font(11.5f))) playlist_save_view();
    if (c.model.ui.scroll_to_top) { c.ui.scroll_to(0); c.model.ui.scroll_to_top = false; }
    float list_top = 190;
    if (view.saved && c.ui.button(c.model.ui.playlist_manage ? "完成" : "管理", {mx + mw - 68, 142, 68, 30}, Style().font(11.5f)))
        c.model.ui.playlist_manage = !c.model.ui.playlist_manage;
    if (view.saved && c.model.ui.playlist_manage) {
        auto& state = c.model.ui;
        bool changed = c.ui.search_field(&state.playlist_filter, {mx, 186, mw - 140, 28}, Style().font(11).hint("歌单内搜索标题 / 歌手 / 专辑"));
        changed |= c.ui.select({"原始顺序", "按歌名", "按歌手", "按专辑", "按时长"}, &state.playlist_sort, {mx + mw - 132, 186, 132, 28}, Style().font(11));
        if (changed) playlist_filter_rows();
        c.ui.input(&state.playlist_rename, {mx, 224, std::max(80.0f, mw - 322), 28}, Style().font(11).hint("歌单名称"));
        if (c.ui.button("重命名", {mx + mw - 314, 224, 66, 28}, Style().font(10.5f))) playlist_rename_local();
        if (c.ui.button("全选", {mx + mw - 240, 224, 50, 28}, Style().font(10.5f))) {
            for (const auto index : view.rows) state.playlist_selected.insert(track_uid(playlist.tracks[size_t(index)]));
        }
        if (c.ui.button("取消选择", {mx + mw - 182, 224, 70, 28}, Style().font(10.5f))) state.playlist_selected.clear();
        auto remove = Style().font(10.5f).destructive(); remove.disabled = state.playlist_selected.empty();
        if (c.ui.button("移除所选", {mx + mw - 104, 224, 104, 28}, remove)) playlist_remove_selected();
        c.ui.muted_text("已选 " + std::to_string(state.playlist_selected.size()) + " 首；仅修改本地快照，原文件保留", {mx, 259, mw, 18}, Style().font(10.5f));
        c.ui.input(&state.library_transfer_path, {mx, 288, mw - 94, 28}, Style().font(11).hint("导出 JSON 文件的完整路径"));
        if (c.ui.button("导出歌单", {mx + mw - 86, 288, 86, 28}, Style().font(10.5f))) library_export(true);
        list_top = 330;
    }
    if (playlist.tracks.empty()) { c.ui.empty_state("歌单是空的", "收藏歌曲或导入音乐文件夹后会显示在这里", {mx, list_top + 12, mw, 120}); return; }
    const Rect list{mx, list_top, mw, c.bar_y - list_top - 12};
    if (list.h < kRowHeight) return;
    c.ui.scroll(list, int(view.rows.size()), kRowHeight, [&](int visible, const Rect& row) {
        const int i = view.rows[size_t(visible)];
        const auto& track = playlist.tracks[size_t(i)];
        auto song = row;
        if (view.saved && c.model.ui.playlist_manage) {
            const auto uid = track_uid(track);
            bool selected = c.model.ui.playlist_selected.count(uid) != 0;
            if (c.ui.checkbox(&selected, "", {row.x, row.cy() - 10, 22, 20})) {
                if (selected) c.model.ui.playlist_selected.insert(uid); else c.model.ui.playlist_selected.erase(uid);
            }
            song.x += 28; song.w -= 28;
            if (selected) c.ui.fill_rect(song, c.th.tint.scaled_alpha(0.10f), 6);
        }
        capture_song_menu(c, track, song, list);
        result_row(c, track, i, song, track_uid(track) == c.s.now_uid, true);
    });
}

static void tab_playlists(Ctx& c) {
    if (c.model.playlist.open) { playlist_detail(c); return; }
    const float mx = kSidebar + 24.0f;
    const float mw = c.W - mx - 24.0f - right_panel_width(c);

    c.ui.heading("歌单", {mx, 20, mw, 30});
    int site = c.model.ui.pl_site;
    if (c.ui.segmented({"网易云", "QQ", "本地", "YouTube"}, &site, {mx, 58, std::min(mw, 340.0f), 28}, Style().font(11.5f))) c.model.ui.pl_site = site;

    if (c.model.ui.pl_site == 2) {
        c.ui.input(&c.model.ui.playlist_link, {mx, 102, mw - 106, 30}, Style().font(11.5f).hint("粘贴网易云 / QQ / YouTube 歌单链接"));
        if (c.ui.button(c.model.ui.importing_playlist ? "保存中…" : "保存在线歌单", {mx + mw - 100, 102, 100, 30}, Style().font(10.5f))) library_import_link();
        c.ui.input(&c.model.ui.local_name, {mx, 144, mw - 106, 30}, Style().font(11.5f).hint("输入名称，将播放队列保存为本地歌单"));
        if (c.ui.button("保存播放队列", {mx + mw - 100, 144, 100, 30}, Style().font(10.5f))) library_save_current();
        c.ui.input(&c.model.ui.local_folder, {mx, 186, mw - 190, 30}, Style().font(11.5f).hint("粘贴本地音乐文件夹路径"));
#ifdef _WIN32
        if (c.ui.button("选择文件夹", {mx + mw - 184, 186, 84, 30}, Style().font(10.5f))) {
            library_choose_folder();
        }
#endif
        if (c.ui.button(c.model.ui.importing_playlist ? "导入中…" : "导入文件夹", {mx + mw - 94, 186, 94, 30}, Style().font(10.5f)))
            library_import_folder(c.model.ui.local_folder, c.model.ui.scan_recursive);
        c.ui.toggle(&c.model.ui.scan_recursive, {mx, 226, 36, 20});
        c.ui.muted_text("包含子文件夹；只保存路径，删除歌单或清缓存不会删除原音乐。", {mx + 46, 227, mw - 46, 18}, Style().font(10.5f));
        const Rect list{mx, 260, mw, c.bar_y - 272};
        if (list.h >= 64) {
            const int count = int(c.model.library.playlists.size());
            c.ui.scroll(list, count, 64, [&](int i, const Rect& row) {
                if (i < int(c.model.library.playlists.size())) local_playlist_row(c, i, row);
            });
        }
        return;
    }

    const bool qq_side = (c.model.ui.pl_site == 1);
    const bool yt_side = c.model.ui.pl_site == 3;
    const bool logged = yt_side ? c.model.youtube.logged : qq_side ? c.model.qq.logged : c.model.netease.logged;
    const bool loading = yt_side ? c.model.youtube.loading : qq_side ? c.model.qq.loading : c.model.netease.loading;
    const std::string err = yt_side ? c.model.youtube.err : qq_side ? c.model.qq.err : c.model.netease.err;
    const std::string nickname = yt_side ? c.model.youtube.nickname : qq_side ? c.model.qq.nickname : c.model.netease.nickname;
    const std::string uid = yt_side ? "" : qq_side ? c.model.ui.qq_uin : c.model.ui.ne_uid;
    const int pl_count = int(yt_side ? c.model.youtube.playlists.size() : qq_side ? c.model.qq.playlists.size() : c.model.netease.playlists.size());
    const char* site_name = yt_side ? "YouTube Music" : qq_side ? "QQ 音乐" : "网易云音乐";

    if (!logged) {
        const Rect card{mx, 102, mw, 112};
        c.ui.panel(card, Style().corner(12));
        c.ui.subheading(std::string("没有连接 ") + site_name, {card.x + 20, card.y + 18, 360, 24});
        if (c.ui.button("去登录", {card.x + 20, card.y + 58, 100, 30}, Style().primary().font(12.0f))) c.model.ui.tab = 5;
        if (!err.empty()) c.ui.muted_text(err, {card.x + 136, card.y + 64, card.w - 156, 20}, Style().font(10.0f));
        return;
    }

    Style nm;
    nm.font_size = 15.0f;
    nm.font_weight = 600;
    c.ui.draw_text(nickname.empty() ? "已登录" : nickname, {mx, 100, mw - 220, 22}, c.th.label, nm);
    Style meta;
    meta.font_size = 10.5f;
    c.ui.draw_text((yt_side ? "YouTube Music" : (qq_side ? "QQ " : "uid ") + uid) + " · " + std::to_string(pl_count) + " 个歌单",
                   {mx, 122, mw - 220, 16}, c.th.label3, meta);
    if (c.ui.button("刷新", {mx + mw - 92, 102, 92, 28}, Style().font(11.5f))) {
        if (yt_side) youtube_refresh();
        else if (qq_side) qq_refresh();
        else ne_refresh();
    }

    if (yt_side) {
        c.ui.input(&c.model.ui.youtube_playlist_name, {mx, 150, mw - 116, 28}, Style().hint("新建 YouTube 私人歌单名称").font(11.5f));
        if (c.ui.button(c.model.youtube.writing ? "处理中…" : "创建歌单", {mx + mw - 108, 150, 108, 28}, Style().font(11.5f))) youtube_create_playlist();
    }
    if (!err.empty()) {
        Style es;
        es.font_size = 11.0f;
        es.wrap_text();
        c.ui.draw_text(err, {mx, yt_side ? 188.0f : 148.0f, mw, 48}, c.th.destructive, es);
        return;
    }
    if (loading) {
        c.ui.spinner({mx + mw * 0.5f - 11, yt_side ? 212.0f : 172.0f, 22, 22});
        return;
    }
    if (pl_count == 0) {
        c.ui.empty_state("这个账号下没有歌单", std::string(), {mx, yt_side ? 216.0f : 176.0f, mw, 110});
        return;
    }

    const float list_y = yt_side ? 190.0f : 152.0f;
    const Rect list{mx, list_y, mw, c.H - kBarHeight - list_y - 12};
    if (list.h < 64) return;
    const int pl_site = c.model.ui.pl_site;
    c.ui.scroll(list, pl_count, 64, [&](int i, const Rect& row) { playlist_row(c, pl_site, i, row); });
}

// ---------- 搜索页 ----------
static void tab_search(Ctx& c) {
    const float mx = kSidebar + 24.0f;
    // 歌词面板是从右边盖过来的，列表得让出位置，否则标题会被压在面板底下
    const float right_pad = 24.0f + right_panel_width(c);
    const float mw = c.W - mx - right_pad;

    const Rect field{mx, 20, mw - 100.0f, 32};
    c.ui.search_field(&c.model.ui.query, field, Style().hint("在线搜索歌曲 / 歌手 / 专辑"));
    if (c.ui.button(c.s.searching ? "搜索中…" : "搜索", {mx + mw - 92.0f, 20, 92, 32},
                    Style().primary().font(12.5f))) {
        start_search();
    }
    if (c.ui.pointer_available() && c.ui.input().key_pressed[KEY_ENTER]) start_search();

    // 音源筛选
    std::uint32_t mask = c.model.ui.src_mask;
    const Rect chips{mx, 62, std::min(mw, 390.0f), 24};
    if (c.ui.toggle_group_multi({"网易云", "QQ", "酷我", "YouTube", "B 站"}, &mask, chips, Style().font(11.0f))) {
        c.model.ui.src_mask = mask;
        c.model.ui.config_dirty = true;
        if (mask != 0) start_search();
    }

    // 状态行
    Style st;
    st.font_size = 11.0f;
    st.align = Align::Right;
    std::string status;
    if (c.s.searching) status = "并发检索中…";
    else if (!c.s.results.empty()) {
        if (c.s.reports.empty()) {
            // 搜索状态不借用歌单名称或播放队列。
            status = std::to_string(c.s.results.size()) + " 首 · 在线搜索";
        } else {
            int live = 0;
            for (const auto& r : c.s.reports)
                if (r.count > 0) ++live;
            status = std::to_string(c.s.results.size()) + " 首 · " + std::to_string(c.s.elapsed_ms) + " ms · " +
                     std::to_string(live) + "/" + std::to_string(music::kSourceCount) + " 源有结果";
        }
    }
    c.ui.draw_text(status, {mx, 88, mw, 18}, c.th.label3, st);

    const Rect list{mx, 114, mw, c.H - kBarHeight - 114 - 14};
    if (list.h < kRowHeight) return;

    if (c.s.results.empty()) {
        if (c.s.searching) {
            c.ui.spinner({list.cx() - 11, list.y + list.h * 0.34f, 22, 22});
            Style es;
            es.font_size = 11.5f;
            es.align = Align::Center;
            c.ui.draw_text("正在检索已开启的音源…", {list.x, list.y + list.h * 0.34f + 34, list.w, 18},
                           c.th.label2, es);
        } else {
            c.ui.empty_state("搜点什么", std::string(), {list.x, list.y + list.h * 0.26f, list.w, 120});
        }
        return;
    }

    if (c.model.ui.scroll_to_top) {
        c.ui.scroll_to(0);
        c.model.ui.scroll_to_top = false;
    }

    const int count = int(c.s.results.size());
    c.ui.scroll(list, count + 1, kRowHeight, [&](int i, const Rect& row) {
        if (i == count) {
            bool more = false;
            for (const auto& report : c.s.reports) more |= report.has_more;
            Style button = Style().font(11); button.disabled = c.s.searching || !more;
            if (c.ui.button(c.s.searching ? "正在检索…" : more ? "加载更多" : "已显示全部结果", {row.x + (row.w - 160) / 2, row.y + 12, 160, 30}, button)) search_more();
            return;
        }
        capture_song_menu(c, c.s.results[size_t(i)], row, list);
        result_row(c, c.s.results[size_t(i)], i, row, track_uid(c.s.results[size_t(i)]) == c.s.now_uid);
    });
}

// ---------- 音源页 ----------
static void tab_sources(Ctx& c) {
    const float mx = kSidebar + 24.0f;
    const float mw = c.W - mx - 24.0f - right_panel_width(c);
    c.ui.heading("音源", {mx, 20, mw, 30});

    const char* cookie_hints[music::kSourceCount] = {"MUSIC_U=...", "qm_keyst=...", "", "SAPISID=...; __Secure-3PSID=..."};
    std::string* cookies[music::kSourceCount] = {&c.model.ui.settings.netease_cookie, &c.model.ui.settings.qq_cookie, nullptr, &c.model.ui.settings.youtube_cookie};

    c.ui.scroll({mx, 64, mw, c.bar_y - 76}, music::kSourceCount, 128, [&](int i, const Rect& row) {
        const bool has_cookie = cookies[i] != nullptr;
        const Rect card{row.x, row.y, row.w, 116.0f};
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
        c.ui.draw_text(rep, {card.x + 34, card.y + 38, std::max(40.0f, card.w - 160), 16}, bad ? c.th.destructive : c.th.label2, rs);
        if (bad) {
            auto retry = Style().font(10.5f); retry.disabled = c.s.searching;
            if (c.ui.button("重试此音源", {card.right() - 100, card.y + 58, 84, 26}, retry)) retry_search_source(i);
        }

        // 开关
        bool on = (c.model.ui.src_mask >> i) & 1u;
        if (c.ui.toggle(&on, {card.right() - 60, card.y + 18, 40, 22})) {
            if (on) c.model.ui.src_mask |= (1u << i);
            else c.model.ui.src_mask &= ~(1u << i);
            c.model.ui.config_dirty = true;
        }

        // Cookie
        if (has_cookie) {
            Style lbl;
            lbl.font_size = 10.5f;
            c.ui.draw_text("登录 Cookie", {card.x + 20, card.y + 62, 200, 14}, c.th.label3, lbl);

            // 已填的 Cookie 默认**只显示掩码**：整串凭据（几百个字符）铺在输入框里
            // 既会溢出卡片边界，又等于把 token 摆在屏幕上。点一下才进入编辑态。
            static bool editing[music::kSourceCount] = {false, false, false};
            const Rect in_box{card.x + 20, card.y + 78, card.w - 40, 26};
            if (cookies[i]->empty() || editing[i]) {
                if (c.ui.input(cookies[i], in_box, Style().hint(cookie_hints[i]).font(11.5f)))
                    c.model.ui.config_dirty = true;
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
        if (!has_cookie) {
            const char* note = i == int(music::Source::YouTube) ? "YouTube Music · 公开曲目，受网络与平台验证限制" :
                i == int(music::Source::Bilibili) ? "公开视频音轨 · 分 P 可单独播放、收藏与下载 · 内置 AAC 解码" : "匿名取流，部分歌曲仅提供试听";
            c.ui.muted_text(note, {card.x + 20, card.y + 76, card.w - 40, 24}, Style().font(10.5f));
        }
    });
}

// ---------- 设置页 ----------
static void settings_group(Ctx& c, const char* title, const Rect& box) {
    c.ui.draw_text(title, {box.x + 2, box.y - 28, box.w - 4, 20}, c.th.label2, Style().font(11.5f).weight(600));
    c.ui.fill_rect(box, c.th.fill3.scaled_alpha(0.5f), 10);
}
static void settings_rule(Ctx& c, const Rect& box, float y) {
    c.ui.fill_rect({box.x + 16, y, box.w - 32, 1}, c.th.separator.scaled_alpha(0.7f));
}
static void settings_label(Ctx& c, const char* text, const Rect& row) {
    c.ui.draw_text(text, {row.x + 16, row.y, row.w - 32, row.h}, c.th.label, Style().font(12));
}
static void account_card(Ctx& c, int site, const Rect& box) {
    const bool qq = site == 1, youtube = site == 2;
    const bool logged = youtube ? c.model.youtube.logged : qq ? c.model.qq.logged : c.model.netease.logged;
    const bool connecting = c.model.login[size_t(site)].connecting;
    const auto& nickname = youtube ? c.model.youtube.nickname : qq ? c.model.qq.nickname : c.model.netease.nickname;
    const auto& error = youtube ? c.model.youtube.err : qq ? c.model.qq.err : c.model.netease.err;
    const auto& note = c.model.login[size_t(site)].note;
    const int count = int(youtube ? c.model.youtube.playlists.size() : qq ? c.model.qq.playlists.size() : c.model.netease.playlists.size());
    const Rect icon{box.x + 16, box.y + 22, 34, 34};
    const auto color = source_color(youtube ? music::Source::YouTube : qq ? music::Source::QQ : music::Source::Netease);
    c.ui.fill_rect(icon, color.scaled_alpha(0.12f), 8);
    c.ui.draw_text(youtube ? "Y" : qq ? "Q" : "N", icon, color, Style().font(16).weight(600).center());
    const float text_x = icon.right() + 12;
    const float action_w = logged ? 126.0f : 82.0f;
    const float text_w = std::max(1.0f, box.right() - action_w - text_x - 12);
    c.ui.draw_text(youtube ? "YouTube Music" : qq ? "QQ 音乐" : "网易云音乐",
        {text_x, box.y + 17, text_w, 20}, c.th.label, Style().font(12.5f).weight(600));
    const auto status = connecting ? "正在连接…" : logged ? (nickname.empty() ? "已登录" : nickname) + " · " + std::to_string(count) + " 个歌单" : "未登录";
    c.ui.muted_text(ellipsize(c.ui, status, text_w, 11), {text_x, box.y + 40, text_w, 18}, Style().font(11));
    if (logged && c.ui.button("刷新", {box.right() - 138, box.y + 27, 48, 26}, Style().ghost().font(11))) {
        if (youtube) youtube_refresh(); else if (qq) qq_refresh(); else ne_refresh();
    }
    if (c.ui.button(connecting ? "取消" : logged ? "重新登录" : "登录", {box.right() - 86, box.y + 27, 70, 26}, Style().font(11))) {
        if (connecting) cancel_login(site); else begin_login(site);
    }
    if (!error.empty() || !note.empty()) c.ui.draw_text(ellipsize(c.ui, error.empty() ? note : error, box.w - 32, 10.5f),
        {box.x + 16, box.y + 66, box.w - 32, 16}, error.empty() ? c.th.label3 : c.th.destructive, Style().font(10.5f));
}
static void storage_settings(Ctx& c, float x, float w, float y) {
    const Rect paths{x, y + 28, w, 144};
    settings_group(c, "存储位置", paths);
    auto path_row = [&](const char* name, const std::string& path, float top) {
        c.ui.draw_text(name, {x + 16, top + 12, w - 112, 20}, c.th.label, Style().font(12));
        c.ui.muted_text(ellipsize(c.ui, path, w - 112, 10.5f), {x + 16, top + 37, w - 112, 18}, Style().font(10.5f));
        if (c.ui.button("打开", {x + w - 76, top + 22, 60, 26}, Style().font(11))) open_data_directory(path);
    };
    path_row("配置、收藏与歌单", c.model.paths.config, paths.y);
    settings_rule(c, paths, paths.y + 72);
    path_row("播放缓存", c.model.paths.cache, paths.y + 72);
    const Rect data{x, y + 220, w, 264};
    settings_group(c, "管理数据", data);
    const char* names[] = {"账号登录信息", "本地收藏与歌单", "当前播放队列", "播放与封面缓存"};
    const char* details[] = {"退出账号，清除本机保存的 Cookie", "收藏、命名歌单与在线歌单快照", "清除队列及其恢复记录", "保留播放中和其他运行实例的文件"};
    const ClearAction actions[] = {ClearAction::Cookies, ClearAction::Library, ClearAction::Queue, ClearAction::Cache};
    for (int i = 0; i < 4; ++i) {
        const float top = data.y + i * 66;
        c.ui.draw_text(names[i], {x + 16, top + 10, w - 112, 20}, c.th.label, Style().font(12));
        c.ui.muted_text(ellipsize(c.ui, details[i], w - 112, 10.5f), {x + 16, top + 34, w - 112, 18}, Style().font(10.5f));
        if (c.ui.button("清除…", {x + w - 80, top + 20, 64, 26}, Style().ghost().font(11).color(c.th.destructive))) c.model.ui.clear_action = actions[i];
        if (i < 3) settings_rule(c, data, top + 66);
    }
    c.ui.muted_text("清除前会再次确认。已下载的音乐和本地音频文件会保留。", {x + 2, data.bottom() + 12, w - 4, 36}, Style().font(10.5f).wrap_text());
    c.ui.subheading("收藏与歌单备份", {x, data.bottom() + 64, w, 24});
    c.ui.input(&c.model.ui.library_transfer_path, {x, data.bottom() + 102, w, 30}, Style().font(11).hint("JSON 备份文件完整路径；恢复会合并收藏与歌单"));
    if (c.ui.button("导出全部", {x, data.bottom() + 144, 90, 28}, Style().font(11))) library_export(false);
    if (c.ui.button("合并恢复", {x + 104, data.bottom() + 144, 90, 28}, Style().font(11))) library_restore();
}

static void clear_data_dialog(Ctx& c) {
    if (c.model.ui.clear_action == ClearAction::None) return;
    const auto action = c.model.ui.clear_action;
    const char* title = "清除缓存？";
    const char* detail = "清理临时音频和内存封面。播放中、正在写入及其他运行实例使用的文件会保留。";
    if (action == ClearAction::Cookies) { title = "清除登录信息？"; detail = "退出播放器中的网易云、QQ 和 YouTube 账号，清除本机保存的 Cookie。浏览器登录、本地歌单和其他设置不受影响。"; }
    else if (action == ClearAction::Library) { title = "清除本地收藏与歌单？"; detail = "删除本机全部收藏、命名歌单和保存的在线歌单快照。此操作无法撤销，不会修改音乐平台上的歌单。"; }
    else if (action == ClearAction::Queue) { title = "清空当前列表？"; detail = "清空当前列表及其重启恢复记录。正在播放的歌曲可继续播放，收藏和保存的歌单不受影响。"; }
    bool open = true;
    if (c.ui.alert_dialog(title, detail, "确认清除", "取消", &open,
        Rect::centered(c.W * 0.5f, c.H * 0.5f, std::min(440.0f, c.W - 40), 240), Style().destructive())) clear_data(action);
    if (!open) c.model.ui.clear_action = ClearAction::None;
}

static void update_settings(Ctx& c, float x, float w, float y) {
    if (c.ui.button("返回通用", {x, y, 88, 28}, Style().ghost().font(11))) c.model.ui.update_settings = false;
    const Rect card{x, y + 48, w, 208};
    c.ui.fill_rect(card, c.th.fill3.scaled_alpha(0.5f), 10);
    draw_brand(c, {x + 20, card.y + 20, 44, 44});
    c.ui.draw_text("Aino Player", {x + 78, card.y + 20, w - 98, 24}, c.th.label, Style().font(15).weight(600));
    c.ui.muted_text(std::string("版本 ") + kVersion, {x + 78, card.y + 47, w - 98, 18}, Style().font(11));
    const auto& update = c.model.update;
    c.ui.draw_text(update.message.empty() ? "启动时自动检查 GitHub 正式版" : update.message,
        {x + 20, card.y + 87, w - 40, 48}, update.status == UpdateStatus::Failed ? c.th.destructive : c.th.label2, Style().font(11.5f).wrap_text());
    Style check = Style().font(11);
    check.disabled = update.status == UpdateStatus::Checking || update.status == UpdateStatus::Downloading ||
        update.status == UpdateStatus::Installing || update.status == UpdateStatus::Installed;
    if (c.ui.button(update.status == UpdateStatus::Checking ? "正在检查…" : "检查更新", {x + 20, card.y + 154, 88, 28}, check)) check_updates();
    if (c.ui.button("发布页", {x + 118, card.y + 154, 72, 28}, Style().ghost().font(11))) open_update(false);
    if (update.status == UpdateStatus::Available && c.ui.button(update.installer ? "下载并安装" : "下载新版", {x + w - 116, card.y + 154, 96, 28}, Style().primary().font(11))) open_update(true);
    if (update.status == UpdateStatus::Downloading) c.ui.muted_text("下载 " + std::to_string(update.progress) + "%", {x + 20, card.bottom() + 14, w - 40, 20}, Style().font(11));
    c.ui.muted_text("Windows 使用安装器覆盖升级；Linux 完成系统授权后覆盖安装 deb，无需卸载旧版。更新保留配置、收藏和歌单。", {x + 2, card.bottom() + 48, w - 4, 64}, Style().font(11).wrap_text());
}
static void about_settings(Ctx& c, float x, float w, float y) {
    draw_brand(c, {x + w / 2 - 32, y + 6, 64, 64});
    c.ui.draw_text("Aino Player", {x, y + 82, w, 28}, c.th.label, Style().font(21).weight(600).center());
    c.ui.muted_text(std::string("版本 ") + kVersion, {x, y + 116, w, 20}, Style().font(11.5f).center());
    const Rect info{x, y + 172, w, 168};
    settings_group(c, "项目信息", info);
    settings_label(c, "作者", {x, info.y, w, 56});
    c.ui.draw_text(project::author, {x + 90, info.y, w - 106, 56}, c.th.label2, Style().font(12).right());
    settings_rule(c, info, info.y + 56);
    settings_label(c, "开源协议", {x, info.y + 56, w, 56});
    if (c.ui.button(project::license, {x + w - 130, info.y + 70, 114, 28}, Style().ghost().font(11).right())) open_project(ProjectLink::License);
    settings_rule(c, info, info.y + 112);
    settings_label(c, "GitHub", {x, info.y + 112, w, 56});
    if (c.ui.button("sudoevolve/aino-player", {x + w - 208, info.y + 126, 192, 28}, Style().ghost().font(11).right())) open_project(ProjectLink::Repository);
    const float actions_x = x + (w - 200) * 0.5f;
    if (c.ui.button("反馈问题", {actions_x, info.bottom() + 18, 88, 28}, Style().ghost().font(11))) open_project(ProjectLink::Issues);
    if (c.ui.button("第三方许可", {actions_x + 100, info.bottom() + 18, 100, 28}, Style().ghost().font(11))) open_project(ProjectLink::Dependencies);
    c.ui.muted_text("aino · Apache-2.0\nminiaudio · Public Domain / MIT-0\nstb · Public Domain / MIT\nFFmpeg · LGPL-2.1-or-later", {x + 4, info.bottom() + 64, w - 8, 96}, Style().font(10.5f).center().wrap_text());
}
static void tab_downloads(Ctx& c) {
    const float x = kSidebar + 24, w = c.W - x - 24 - right_panel_width(c);
    c.ui.draw_text("下载管理", {x, 24, w, 36}, c.th.label, Style().font(25).weight(700));
    if (c.ui.button("取消全部", {x, 74, 84, 28}, Style().font(11))) cancel_all_downloads();
    if (c.ui.button("重试失败", {x + 94, 74, 84, 28}, Style().font(11))) retry_failed_downloads();
    if (c.ui.button("清理记录", {x + 188, 74, 84, 28}, Style().font(11))) clear_finished_downloads();
    c.ui.tooltip({x + 188, 74, 84, 28}, "仅清理已结束记录，保留音乐文件");
    c.ui.segmented({"全部", "等待中", "下载中", "失败", "已完成"}, &c.model.ui.download_filter, {x, 118, w, 30}, Style().font(11));
    std::vector<const std::pair<const std::string, DlItem>*> rows;
    for (const auto& entry : c.model.downloads) {
        const auto& d = entry.second; const int f = c.model.ui.download_filter;
        if (f == 1 && (!d.queued || d.done || d.failed)) continue;
        if (f == 2 && (d.queued || d.done || d.failed)) continue;
        if (f == 3 && !d.failed) continue;
        if (f == 4 && !d.done) continue;
        rows.push_back(&entry);
    }
    const Rect list{x, 164, w, c.bar_y - 180};
    if (rows.empty()) { c.ui.empty_state("暂无下载记录", "右键歌曲或点击下载按钮添加任务", list); return; }
    c.ui.scroll(list, int(rows.size()), 76, [&](int index, const Rect& row) {
        const auto& entry = *rows[size_t(index)]; const auto& d = entry.second;
        const Rect action{row.right() - 76, row.y + 24, 72, 28};
        if (c.ui.button(d.done ? "文件位置" : d.failed ? "重试" : "取消", action, Style().font(10.5f))) {
            if (d.done) reveal_download(entry.first); else if (d.failed) retry_download(entry.first); else cancel_download(entry.first);
        }
        const float tw = std::max(20.0f, row.w - 100);
        c.ui.draw_text(ellipsize(c.ui, d.name, tw, 12), {row.x + 4, row.y + 9, tw, 20}, c.th.label, Style().font(12));
        const std::string status = d.done ? "已完成" : d.failed ? d.err : d.publishing ? "正在保存" : d.queued ? "等待下载" : "正在下载 " + std::to_string(d.percent) + "%";
        c.ui.draw_text(ellipsize(c.ui, status, tw, 10.5f), {row.x + 4, row.y + 34, tw, 18}, d.failed ? c.th.destructive : c.th.label2, Style().font(10.5f));
        c.ui.tooltip({row.x, row.y + 30, tw, 23}, status);
        if (!d.done && !d.failed) { c.ui.fill_rect({row.x + 4, row.y + 60, tw, 3}, c.th.fill3, 1); c.ui.fill_rect({row.x + 4, row.y + 60, tw * std::clamp(d.percent, 0, 100) / 100.0f, 3}, c.th.tint, 1); }
        c.ui.fill_rect({row.x, row.bottom() - 1, row.w, 1}, c.th.separator.scaled_alpha(0.5f));
    });
}
static void tab_recent(Ctx& c) {
    const float x = kSidebar + 24, w = c.W - x - 24 - right_panel_width(c);
    c.ui.draw_text("最近播放", {x, 24, w, 36}, c.th.label, Style().font(25).weight(700));
    c.ui.muted_text("播放至少 5 秒后记录 · 最近 200 首 · 保存在本机", {x, 70, w - 90, 26}, Style().font(11));
    if (c.ui.button("清空记录", {x + w - 84, 68, 84, 28}, Style().font(11))) recent_clear();
    const Rect list{x, 112, w, c.bar_y - 128};
    const auto& tracks = c.model.library.recent;
    if (tracks.empty()) { c.ui.empty_state("还没有播放记录", "听过的歌曲会显示在这里", list); return; }
    c.ui.scroll(list, int(tracks.size()), 64, [&](int index, const Rect& row) {
        const auto& track = tracks[size_t(index)]; capture_song_menu(c, track, row, list);
        const bool clicked = c.ui.item("", "", "", row, Style().corner(7));
        cover_art(c, track.cover_url, track.title, {row.x + 8, row.y + 8, 46, 46}, 6);
        const float tx = row.x + 66, tw = std::max(20.0f, row.w - 80);
        c.ui.draw_text(ellipsize(c.ui, track.title, tw, 12.5f), {tx, row.y + 10, tw, 20}, c.th.label, Style().font(12.5f));
        c.ui.draw_text(ellipsize(c.ui, track.artist + " · " + track.album, tw, 10.5f), {tx, row.y + 34, tw, 18}, c.th.label2, Style().font(10.5f));
        if (clicked) recent_play(size_t(index));
    });
}
static void tab_settings(Ctx& c) {
    const float available = c.W - kSidebar - right_panel_width(c);
    const float w = std::min(680.0f, available - 48);
    const float x = kSidebar + (available - w) * 0.5f;
    c.ui.draw_text("设置", {x, 26, w, 34}, c.th.label, Style().font(25).weight(700));
    auto& selected = c.model.ui.settings_tab;
    const bool changed = c.ui.segmented({"通用", "账号", "播放", "下载", "数据", "关于"}, &selected, {x, 78, w, 32}, Style().font(11.5f));
    if (changed) c.model.ui.update_settings = false;
    const float content_height = selected == 4 ? 700.0f : selected == 5 ? 510.0f : selected == 3 ? 350.0f : selected == 2 ? 700.0f : selected == 1 ? 360.0f : c.model.ui.update_settings ? 410.0f : 520.0f;
    const Rect viewport{x, 132, w, c.bar_y - 148};
    if (changed) c.ui.scroll_to_index(viewport, 0, content_height, 1);
    c.ui.scroll(viewport, 1, content_height, [&](int, const Rect& area) {
        const float y = area.y;
        if (selected == 4) { storage_settings(c, x, w, y); return; }
        if (selected == 5) { about_settings(c, x, w, y); return; }
        if (selected == 1) {
            const Rect group{x, y + 28, w, 276};
            settings_group(c, "音乐账户", group);
            for (int i = 0; i < 3; ++i) {
                account_card(c, i, {x, group.y + i * 92, w, 92});
                if (i < 2) settings_rule(c, group, group.y + (i + 1) * 92);
            }
            c.ui.muted_text("在系统默认浏览器中完成官网登录。", {x + 2, group.bottom() + 14, w - 4, 20}, Style().font(11));
            return;
        }
        if (selected == 3) {
            const Rect directory{x, y + 28, w, 104};
            settings_group(c, "存储位置", directory);
            settings_label(c, "下载目录", {x, directory.y + 4, w, 36});
            if (c.ui.input(&c.model.ui.download_dir, {x + 16, directory.y + 48, w - 32, 32}, Style().hint(default_download_dir()).font(11))) c.model.ui.config_dirty = true;
            const Rect metadata{x, y + 180, w, 124};
            settings_group(c, "写入音乐文件", metadata);
            settings_label(c, "内嵌封面", {x, metadata.y, w, 62});
            if (c.ui.toggle(&c.model.ui.settings.embed_cover, {x + w - 54, metadata.y + 20, 38, 22})) c.model.ui.config_dirty = true;
            settings_rule(c, metadata, metadata.y + 62);
            settings_label(c, "内嵌歌词", {x, metadata.y + 62, w, 62});
            if (c.ui.toggle(&c.model.ui.settings.embed_lyrics, {x + w - 54, metadata.y + 82, 38, 22})) c.model.ui.config_dirty = true;
            return;
        }
        if (selected == 2) {
            const Rect group{x, y + 28, w, 192};
            settings_group(c, "播放", group);
            const bool compact = w < 450;
            settings_label(c, "音质", {x, group.y, w, 64});
            if (c.ui.segmented({"标准", "高", "无损"}, &c.model.ui.settings.quality, {x + w - 196, group.y + 18, 180, 28}, Style().font(11))) c.model.ui.config_dirty = true;
            settings_rule(c, group, group.y + 64);
            settings_label(c, "音量", {x, group.y + 64, w, 64});
            const float slider_w = compact ? 140.0f : 200.0f;
            static SeekBar volume;
            float value = 0;
            if (seek_bar(c, c.model.ui.volume, 100, {x + w - slider_w - 56, group.y + 86, slider_w, 20}, &volume, &value)) set_volume(value);
            c.ui.draw_text(std::to_string(int(c.model.ui.volume)), {x + w - 46, group.y + 64, 30, 64}, c.th.label2, Style().font(11).right());
            settings_rule(c, group, group.y + 128);
            settings_label(c, "播放模式", {x, group.y + 128, w, 64});
            int mode = int(c.model.ui.play_mode);
            if (c.ui.select({"顺序播放", "随机播放", "列表循环", "单曲循环"}, &mode, {x + w - 130, group.y + 146, 114, 28}, Style().font(11))) set_play_mode(PlayMode(mode));
            const float dy = group.bottom() + 24;
            c.ui.draw_text("输出设备", {x + 2, dy, w - 110, 24}, c.th.label2, Style().font(12).weight(600));
            Style refresh_style = Style().font(11); refresh_style.disabled = c.model.ui.device_loading;
            if (c.ui.button(c.model.ui.device_loading ? "读取中…" : "刷新设备", {x + w - 100, dy, 98, 26}, refresh_style)) refresh_output_devices();
            std::vector<std::string> names{"跟随系统默认"}, ids{""}; int device = 0;
            for (const auto& entry : c.model.ui.output_devices) { ids.push_back(entry.id); names.push_back(entry.name + (entry.is_default ? "（默认）" : "")); if (entry.id == c.model.ui.output_device) device = int(ids.size()) - 1; }
            if (!c.model.ui.output_device.empty() && device == 0) { ids.push_back(c.model.ui.output_device); names.push_back("已保存的设备（请刷新确认）"); device = int(ids.size()) - 1; }
            if (c.ui.select(names, &device, {x, dy + 40, w, 30}, Style().font(11))) select_output_device(ids[size_t(device)]);
            c.ui.muted_text(c.model.ui.device_error.empty() ? "切换时保留播放位置与暂停状态；设备不可用时回退默认输出。" : c.model.ui.device_error, {x, dy + 82, w, 44}, Style().font(10.5f).wrap_text());
            c.ui.draw_text("音频解码", {x + 2, dy + 144, w - 4, 20}, c.th.label2, Style().font(11.5f).weight(600));
            c.ui.muted_text(audio::decoder_info(), {x + 2, dy + 174, w - 4, 80}, Style().font(10.5f).wrap_text());
            return;
        }
        if (c.model.ui.update_settings) { update_settings(c, x, w, y); return; }
        const Rect appearance{x, y + 28, w, 62};
        settings_group(c, "外观", appearance);
        settings_label(c, "深色外观", appearance);
        if (c.ui.toggle(&c.model.ui.dark, {x + w - 54, appearance.y + 20, 38, 22})) c.model.ui.config_dirty = true;
        const Rect version{x, y + 138, w, 72};
        settings_group(c, "软件更新", version);
        c.ui.draw_text("Aino Player", {x + 16, version.y + 13, w - 140, 20}, c.th.label, Style().font(12));
        c.ui.muted_text(std::string("版本 ") + kVersion, {x + 16, version.y + 38, w - 140, 18}, Style().font(11));
        if (c.ui.button(c.model.update.status == UpdateStatus::Available ? "发现新版" : "检查更新", {x + w - 104, version.y + 22, 88, 28}, Style().font(11))) c.model.ui.update_settings = true;
        c.ui.muted_text("启动时自动检查新版本。", {x + 2, version.bottom() + 14, w - 4, 20}, Style().font(10.5f));
        const Rect desktop{x, version.bottom() + 70, w, 124};
        settings_group(c, "后台播放", desktop);
        settings_label(c, "显示系统托盘", {x, desktop.y, w, 62});
        if (c.ui.toggle(&c.model.ui.tray_enabled, {x + w - 54, desktop.y + 20, 38, 22})) c.model.ui.config_dirty = true;
        settings_rule(c, desktop, desktop.y + 62);
        settings_label(c, "关闭窗口后继续播放", {x, desktop.y + 62, w, 62});
        if (c.ui.toggle(&c.model.ui.close_to_tray, {x + w - 54, desktop.y + 82, 38, 22})) c.model.ui.config_dirty = true;
        c.ui.muted_text("无可用托盘时关闭窗口仍会退出，可从系统媒体面板控制播放。", {x + 2, desktop.bottom() + 14, w - 4, 42}, Style().font(10.5f).wrap_text());
    });
}


static void draw_lyric_icon(aino::Ui& ui, const aino::Rect& box, const aino::Color& col) {
    const float h = 2.0f;
    const float gaps[3] = {1.0f, 0.78f, 0.55f};
    for (int i = 0; i < 3; ++i) {
        const float w = box.w * gaps[i];
        ui.fill_rect({box.x, box.y + float(i) * (h + 3.5f), w, h}, col, h * 0.5f);
    }
}

static SystemIcon play_mode_icon(PlayMode mode) {
    switch (mode) {
        case PlayMode::Shuffle: return SystemIcon::Shuffle;
        case PlayMode::RepeatAll: return SystemIcon::RepeatAll;
        case PlayMode::RepeatOne: return SystemIcon::RepeatOne;
        default: return SystemIcon::List;
    }
}

static std::string sleep_timer_status(const UiState& state) {
    if (state.sleep_after_track) return "当前歌曲结束后停止";
    if (state.sleep_deadline) return "剩余 " + fmt_time(static_cast<long long>(state.sleep_remaining_seconds) * 1000);
    return "未启用";
}

static void playback_options(Ctx& c, const Rect& mode, const Rect& timer) {
    auto* state = &c.model.ui;
    const Style trigger = Style().ghost().font(12).indicator(false);
    MenuOptions modes; modes.selected = int(state->play_mode);
    int picked = -1;
    if (c.ui.menu("", {"顺序播放", "随机播放", "列表循环", "单曲循环"}, &picked, mode, trigger, modes))
        set_play_mode(PlayMode(picked));
    const bool timed = state->sleep_after_track || state->sleep_deadline;
    MenuOptions timers; timers.disabled = timed ? std::vector<int>{0} : std::vector<int>{0, 8};
    if (state->sleep_after_track) timers.selected = 6;
    picked = -1;
    if (c.ui.menu("", {"睡眠定时 · " + sleep_timer_status(*state), "", "15 分钟后停止", "30 分钟后停止",
            "60 分钟后停止", "90 分钟后停止", "播完当前歌曲", "", "取消定时"}, &picked, timer, trigger, timers)) {
        constexpr int minutes[] = {15, 30, 60, 90};
        if (picked >= 2 && picked <= 5) set_sleep_timer(minutes[picked - 2]);
        else if (picked == 6) set_sleep_timer(0, true);
        else if (picked == 8) set_sleep_timer(0);
    }
    c.ui.system_icon(play_mode_icon(state->play_mode), mode.center(20, 20),
        state->play_mode == PlayMode::Sequential ? c.th.label2 : c.th.tint);
    c.ui.system_icon(SystemIcon::Timer, timer.center(20, 20), timed ? c.th.tint : c.th.label2);
    c.ui.tooltip(mode, std::string("播放模式：") + play_mode_name(state->play_mode));
    c.ui.tooltip(timer, "睡眠定时：" + sleep_timer_status(*state));
}

// 三组互不重叠：播放控制、曲目信息与进度、音量及侧栏工具。
static void player_bar(Ctx& c) {
    const float y = c.bar_y, cy = y + 40;
    c.ui.fill_rect({0, y, c.W, kBarHeight}, c.th.control_bg);
    c.ui.fill_rect({0, y, c.W, 1}, c.th.separator.scaled_alpha(0.7f));
    const bool playing = c.s.ps == PlayState::Playing;
    const bool busy = c.s.ps == PlayState::Resolving || c.s.ps == PlayState::Buffering;
    const Rect mode{26, cy - 15, 30, 30}, prev{70, cy - 16, 32, 32};
    const Rect play{111, cy - 20, 40, 40}, next{160, cy - 16, 32, 32}, timer{204, cy - 15, 30, 30};
    playback_options(c, mode, timer);
    if (c.ui.icon_button("", prev, Style().ghost())) play_step(-1);
    c.ui.system_icon(SystemIcon::PreviousTrack, prev.center(20, 20), c.th.label2);
    if (c.ui.icon_button("", play, Style().ghost().corner(20))) toggle_pause();
    if (busy) c.ui.spinner(play.center(19, 19));
    else c.ui.system_icon(playing ? SystemIcon::Pause : SystemIcon::Play, play.center(23, 23), c.th.label);
    if (c.ui.icon_button("", next, Style().ghost())) play_step(1);
    c.ui.system_icon(SystemIcon::NextTrack, next.center(20, 20), c.th.label2);
    c.ui.tooltip(prev, "上一首"); c.ui.tooltip(play, playing ? "暂停" : "播放"); c.ui.tooltip(next, "下一首");
    const bool timed = c.model.ui.sleep_after_track || c.model.ui.sleep_deadline;
    if (timed) {
        const auto seconds = c.model.ui.sleep_remaining_seconds;
        const auto label = c.model.ui.sleep_after_track ? std::string("本首结束") : std::to_string(seconds / 60) + ":" + (seconds % 60 < 10 ? "0" : "") + std::to_string(seconds % 60);
        c.ui.draw_text(label, {timer.cx() - 32, y + 65, 64, 14}, c.th.tint, Style().font(9).center());
    }

    const float width = std::min(560.0f, c.W - 560.0f), x = (c.W - width) * 0.5f;
    const Rect song{x, y + 9, width, 70};
    c.ui.fill_rect(song, c.th.fill3.scaled_alpha(0.45f), 8);
    const Rect cover{x + 12, y + 20, 44, 44};
    if (!c.s.now_title.empty()) cover_art(c, c.s.now_cover, c.s.now_title, cover, 5);
    else draw_brand(c, cover);
    const float tx = cover.right() + 12, tw = song.right() - tx - 16;
    const auto title = c.s.now_title.empty() ? "Aino Player" : c.s.now_title;
    c.ui.draw_text(ellipsize(c.ui, title, tw, 12.5f, 600), {tx, y + 14, tw, 19}, c.s.now_title.empty() ? c.th.label2 : c.th.label, Style().font(12.5f).weight(600));
    std::string detail = c.s.now_artist;
    if (busy) detail = c.s.ps == PlayState::Buffering ? "缓冲 " + std::to_string(int(c.s.buffer * 100)) + "%" : "正在解析…";
    else if (!c.s.now_short.empty()) detail += (detail.empty() ? "" : " · ") + c.s.now_short;
    if (detail.empty()) detail = "选择一首歌曲开始播放";
    c.ui.muted_text(ellipsize(c.ui, detail, tw, 10.5f), {tx, y + 35, tw, 16}, Style().font(10.5f));
    const float duration = std::max(1.0f, c.model.ui.dur_ms);
    const Rect progress{tx + 39, y + 54, std::max(1.0f, tw - 78), 18};
    static SeekBar seek;
    float value = 0;
    if (seek_bar(c, std::min(c.model.ui.pos_ms, duration), c.model.ui.dur_ms, progress, &seek, &value)) { seek_playback(int(value)); c.model.ui.pos_ms = value; }
    c.ui.draw_text(fmt_time((long long)c.model.ui.pos_ms), {tx, y + 55, 32, 16}, c.th.label3, Style().font(9.5f));
    c.ui.draw_text(fmt_time((long long)c.model.ui.dur_ms), {song.right() - 48, y + 55, 32, 16}, c.th.label3, Style().font(9.5f).right());

    const float tools = c.W - 272;
    const Rect speaker{tools, cy - 12, 20, 24};
    draw_speaker(c.ui, speaker, c.th.label2, c.model.ui.volume);
    c.ui.tooltip(speaker, "音量 " + std::to_string(int(c.model.ui.volume)) + "%");
    static SeekBar volume;
    if (seek_bar(c, c.model.ui.volume, 100, {tools + 26, cy - 10, 72, 20}, &volume, &value)) set_volume(value);
    c.ui.fill_rect({tools + 111, cy - 10, 1, 20}, c.th.separator.scaled_alpha(0.7f));
    const Rect download{tools + 124, cy - 14, 28, 28}, favorite{tools + 156, cy - 14, 28, 28};
    const Rect lyrics{tools + 188, cy - 14, 28, 28}, queue{tools + 220, cy - 14, 28, 28};
    Style track_action = Style().ghost(); track_action.disabled = c.s.now_uid.empty();
    if (c.ui.icon_button("", download, track_action)) download_current();
    const auto dl = c.model.downloads.find(c.s.now_uid);
    if (dl != c.model.downloads.end() && !dl->second.done && !dl->second.failed)
        c.ui.draw_text(std::to_string(dl->second.percent), download, c.th.tint, Style().font(9).center());
    else c.ui.system_icon(dl != c.model.downloads.end() && dl->second.done ? SystemIcon::Check : dl != c.model.downloads.end() && dl->second.failed ? SystemIcon::Warning : SystemIcon::Download,
        download.center(15, 15), dl != c.model.downloads.end() && dl->second.failed ? c.th.destructive : c.th.label2);
    const bool saved = !c.s.now_track.variants.empty() && c.model.library.contains(c.s.now_track);
    if (c.ui.icon_button("", favorite, track_action)) favorite_toggle(c.s.now_track);
    if (saved) c.ui.fill_rect(favorite, c.th.tint.scaled_alpha(0.1f), 6);
    c.ui.system_icon(SystemIcon::Star, favorite.center(16, 16), saved ? c.th.tint : c.th.label2);
    if (c.ui.icon_button("", lyrics, Style().ghost())) {
        c.model.ui.lyrics_open = !c.model.ui.lyrics_open; c.model.ui.queue_open = false;
        if (c.model.ui.lyrics_open) request_current_lyrics();
    }
    if (c.model.ui.lyrics_open) c.ui.fill_rect(lyrics, c.th.tint.scaled_alpha(0.1f), 6);
    draw_lyric_icon(c.ui, lyrics.center(16, 13), c.model.ui.lyrics_open ? c.th.tint : c.th.label2);
    if (c.ui.icon_button("", queue, Style().ghost())) { c.model.ui.queue_open = !c.model.ui.queue_open; c.model.ui.lyrics_open = false; }
    if (c.model.ui.queue_open) c.ui.fill_rect(queue, c.th.tint.scaled_alpha(0.1f), 6);
    c.ui.system_icon(SystemIcon::List, queue.center(17, 17), c.model.ui.queue_open ? c.th.tint : c.th.label2);
    c.ui.tooltip(download, "下载当前歌曲"); c.ui.tooltip(favorite, saved ? "取消收藏" : "收藏歌曲");
    c.ui.tooltip(lyrics, "歌词"); c.ui.tooltip(queue, "待播清单");
}


static void draw_toast(Ctx& c) {
    if (c.model.ui.toast.empty()) return;
    const unsigned long long now = ui_ticks();
    if (now >= c.model.toast_until) {
        c.model.ui.toast.clear();
        return;
    }
    Style s;
    s.font_size = 11.5f;
    const float max_w = 560.0f;
    const float tw = std::min(max_w, c.ui.text_width(c.model.ui.toast, 11.5f));
    const float w = tw + 32.0f;
    const Rect box{c.W * 0.5f - w * 0.5f, c.bar_y - 52.0f, w, 34.0f};
    c.ui.drop_shadow(box, 8.0f, 6.0f, Color(0, 0, 0, 60), 0.5f);
    const bool dk = c.model.ui.dark;
    c.ui.fill_rect(box, dk ? Color(236, 236, 240, 242) : Color(28, 28, 30, 235), 8.0f);
    s.align = Align::Center;
    s.valign = VAlign::Middle;
    c.ui.draw_text(ellipsize(c.ui, c.model.ui.toast, max_w, 11.5f), box,
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
    const auto& doc = c.model.lyrics.doc;
    const auto& uid = c.model.lyrics.uid;
    const bool loading = c.model.lyrics.loading;

    const Rect tools_button{box.right() - 64, box.bottom() - 28, 54, 24};
    if (c.ui.button(c.model.ui.lyric_tools ? "收起" : "校准", tools_button, Style().ghost().font(10))) c.model.ui.lyric_tools = !c.model.ui.lyric_tools;
    const float extra = c.model.ui.lyric_tools ? 126.0f : 0.0f;
    const Rect area{box.x + 8, box.y + 4, box.w - 16, std::max(20.0f, box.h - 36 - extra)};
    const int offset = lyric_offset();
    if (c.model.ui.lyric_tools) {
        const float x = box.x + 16, y = box.bottom() - 150, w = box.w - 32;
        char label[80]; std::snprintf(label, sizeof label, "歌词偏移 %+.1f 秒（正数延后）", offset / 1000.0);
        c.ui.muted_text(label, {x, y, w, 20}, Style().font(10));
        if (c.ui.button("提前", {x, y + 26, 62, 25}, Style().font(10))) change_lyric_offset(-500);
        if (c.ui.button("延后", {x + 70, y + 26, 62, 25}, Style().font(10))) change_lyric_offset(500);
        if (c.ui.button("归零", {x + 140, y + 26, 62, 25}, Style().font(10))) change_lyric_offset(0, true);
        c.ui.input(&c.model.ui.lyric_file_path, {x, y + 60, w - 66, 27}, Style().font(10).hint("UTF-8 LRC 文件完整路径"));
        if (c.ui.button("选择…", {x + w - 60, y + 60, 60, 27}, Style().font(10))) choose_local_lyrics();
        if (c.ui.button("使用本地歌词", {x, y + 94, 110, 25}, Style().font(10))) use_local_lyrics();
        if (c.ui.button("恢复默认", {x + 120, y + 94, 90, 25}, Style().font(10))) use_local_lyrics(true);
    }

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

    const int cur_line = doc->synced ? lyr::current_line(doc->lines, (long long)c.model.ui.pos_ms - offset) : -1;
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
    const double now_ms = double(ui_ticks());
    float dt = last_ms > 0.0 ? float((now_ms - last_ms) / 1000.0) : 0.016f;
    last_ms = now_ms;
    if (dt > 0.06f) dt = 0.06f;   // 卡了一帧也别让歌词瞬移
    if (dt < 0.001f) dt = 0.001f;

    if (g_lyr_cur != cur_line) {
        g_lyr_cur = cur_line;
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
    if (!c.ui.pointer_available()) press_line = -1;
    for (int i = 0; i < nline; ++i) {
        const float ty = base_y + g_lyr_layout.top[size_t(i)];
        // 行高取到下一行的顶部：整段行距都可点，不用瞄准文字那一条
        const float next_top = (i + 1 < nline) ? g_lyr_layout.top[size_t(i + 1)] : g_lyr_layout.total;
        const float row_h = std::max(26.0f, next_top - g_lyr_layout.top[size_t(i)]);
        if (ty + row_h < area.y || ty > area.bottom()) continue;

        const int sline = g_lyr_layout.src[size_t(i)];
        const bool active = (sline == cur_line);
        const Rect row{area.x + 4, ty - 3.0f, area.w - 8, row_h};
        const bool row_hover = c.ui.pointer_available() && row.contains(c.ui.input().mouse.x, c.ui.input().mouse.y);
        if (row_hover && !active) c.ui.fill_rect(row, c.th.fill3, 7.0f);

        const bool has_time = doc->lines[size_t(sline)].ms >= 0;
        if (row_hover && c.ui.input().pressed[0]) press_line = i;
        if (row_hover && c.ui.input().released[0] && press_line == i && has_time) {
            seek_playback(int(std::max(0LL, doc->lines[size_t(sline)].ms + offset)));
            c.model.ui.pos_ms = float(std::max(0LL, doc->lines[size_t(sline)].ms + offset));
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
                   {box.x + 20, box.bottom() - 26, box.w - 98, 16}, c.th.label4, f);
}

static void listening_panel(Ctx& c, const Rect& box) {
    c.ui.fill_rect(box, c.th.control_bg);
    c.ui.fill_rect({box.x, box.y, 1, box.h}, c.th.separator.scaled_alpha(0.7f));
    int selected = c.model.ui.lyrics_open ? 1 : 0;
    if (c.ui.segmented({"待播清单", "歌词"}, &selected, {box.x + 16, box.y + 16, box.w - 68, 30}, Style().font(11.5f))) {
        c.model.ui.lyrics_open = selected == 1; c.model.ui.queue_open = selected == 0;
        if (selected == 1) request_current_lyrics();
    }
    const Rect close{box.right() - 40, box.y + 18, 24, 26};
    if (c.ui.icon_button("", close, Style().ghost())) { c.model.ui.lyrics_open = false; c.model.ui.queue_open = false; }
    c.ui.system_icon(SystemIcon::Close, close.center(10, 10), c.th.label3);
    const Rect content{box.x, box.y + 62, box.w, box.h - 62};
    if (selected == 1) lyrics_panel(c, content); else queue_panel(c, content);
}

static void compose(aino::Ui& ui, Ctx& c) {
    if (c.model.ui.tab == 2) { c.model.ui.tab = 1; c.model.ui.queue_open = true; c.model.ui.lyrics_open = false; }
    ui.fill_rect({0, 0, c.W, c.H}, c.th.window_bg);
    sidebar(c);

    switch (c.model.ui.tab) {
        case 0: tab_search(c); break;
        case 1: tab_discover(c); break;
        case 3: tab_playlists(c); break;
        case 4: tab_sources(c); break;
        case 6: tab_downloads(c); break;
        case 7: tab_recent(c); break;
        default: tab_settings(c); break;
    }
    if (right_panel_width(c) > 0) {
        listening_panel(c, {c.W - kPanelWidth, 0, kPanelWidth, c.bar_y});
    }
    player_bar(c);
    draw_song_menu(c);
    draw_toast(c);
    clear_data_dialog(c);
}

void draw(aino::Ui& ui, ViewModel& model) {
    Ctx context{ui, ui.theme(), ui.width(), ui.height(), ui.height() - kBarHeight, model.playback, model};
    compose(ui, context);
}
} // namespace player::ui
