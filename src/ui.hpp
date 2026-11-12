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
        if (auto img = cover_image(url); img && img->w > 0) {
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
        if (c.ui.nav_item(navs[i], c.model.ui.tab == i, box, Style().padded(34, 0))) c.model.ui.tab = i;
        c.ui.system_icon(icons[i], {box.x + 11, box.y + 7, 16, 16}, c.model.ui.tab == i ? c.th.tint : c.th.label2);
    }

    c.ui.fill_rect({20, 270, w - 40, 1}, c.th.separator);

    Style sec;
    sec.font_size = 10.5f;
    sec.font_weight = 600;
    c.ui.draw_text("音源状态", {22, 286, 120, 16}, c.th.label2, sec);

    for (int i = 0; i < music::kSourceCount; ++i) {
        const float y = 308.0f + float(i) * 26.0f;
        const bool on = (c.model.ui.src_mask >> i) & 1u;
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
    std::snprintf(buf, sizeof buf, "帧 %.1fms · 缓存 %s", c.model.ui.frame_ms, fmt_size(c.s.cached_bytes).c_str());
    Style foot;
    foot.font_size = 10.0f;
    const float foot_y = c.bar_y - 34.0f;   // 必须在播放条上沿之上，否则被它盖住
    c.ui.draw_text("aino player · 多音源", {22, foot_y, w - 44, 14}, c.th.label4, foot);
    c.ui.draw_text(buf, {22, foot_y + 16, w - 44, 14}, c.th.label3, foot);
}

// ---------- 结果行 ----------
static void result_row(Ctx& c, const music::Track& t, int index, const Rect& box, bool is_playing) {
    const bool selected = (c.model.ui.sel == index);
    const int nvar = int(t.variants.size());
    const float dur_w = 50.0f;
    const float dl_w = 26.0f;
    const float q_w = 26.0f;   // 「加入队列」占位，和下载按钮并排
    const float fav_w = 26.0f;
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
    const Rect favorite{qa.x - fav_w, mid_y - 11.0f, 22.0f, 22.0f};
    const bool saved = c.model.library.contains(t);
    const bool favorite_clicked = c.ui.item("", "", "", favorite, Style().corner(6));
    int pct = -1;
    bool dl_done = false, dl_failed = false;
    const std::string uid = track_uid(t);
    if (auto it = c.model.downloads.find(uid); it != c.model.downloads.end()) {
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
        float px = box.right() - dur_w - 6 - dl_w - q_w - fav_w - pills_w;
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
    const float max_text = box.w - (text_x - box.x) - pills_w - dur_w - dl_w - q_w - fav_w - 18;

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
    if (qa_clicked) {
        // 「＋」= 追加到队列末尾（不覆盖当前列表）
        if (queue_push(t)) toast("已加入队列：" + t.title, 2600);
        else toast("队列里已经有这首了", 2600);
    }
    if (pill_clicked >= 0 && pill_clicked != chosen) choose_source(index, pill_clicked, is_playing);
    if (row_clicked) start_play(index);
}

// ---------- 歌单页（网易云 / QQ 两个账号共用一套版式） ----------
// site：0 = 网易云，1 = QQ
static void playlist_row(Ctx& c, int site, int index, const Rect& box) {
    std::string name, cover;
    int count = 0;
    if (site == 1) {
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
    const bool sync_here = (c.model.sync_site == site && c.model.sync_index == index);

    // 同步按钮必须先于整行绘制：aino 的"按下"只归第一个包含指针的控件，
    // 整行先画的话这个按钮永远收不到点击（结果就是"点同步变成了打开歌单"）。
    const Rect sync_btn{box.right() - 84, box.y + box.h * 0.5f - 13, 60, 26};
    const Rect save_btn{sync_btn.x - 88, sync_btn.y, 80, 26};
    const bool save_clicked = c.ui.button("保存到本地", save_btn, Style().font(10.5f));
    const bool hover = box.contains(c.ui.input().mouse.x, c.ui.input().mouse.y);
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

    if (save_clicked) library_save_remote(site, index);
    else if (sync_clicked) playlist_sync_to(site, index);
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
    const float mw = c.W - mx - 24.0f - (c.model.ui.lyrics_open ? kLyricsWidth : 0.0f);

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

static void queue_row(Ctx& c, const music::Track& t, int index, const Rect& box, bool is_playing) {
    const float rm_w = 26.0f;
    const float dur_w = 48.0f;
    const bool hover = box.contains(c.ui.input().mouse.x, c.ui.input().mouse.y);

    // 行内按钮先认领按压（aino 的 press 只归第一个包含指针的控件）
    const Rect rm{box.right() - rm_w - 8, box.y + box.h * 0.5f - 11, 22, 22};
    const Rect favorite{rm.x - 27, rm.y, 22, 22};
    const bool favorite_clicked = c.ui.item("", "", "", favorite, Style().corner(6));
    const bool saved = c.model.library.contains(t);
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
    const float tw = box.right() - tx - dur_w - rm_w - 45;
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
    c.ui.draw_text(fmt_time(t.duration_ms), {box.right() - dur_w - rm_w - 37, box.y, dur_w, box.h}, c.th.label3, ds);
    c.ui.system_icon(SystemIcon::Star, favorite.center(13, 13), saved ? c.th.tint : c.th.label3);

    // hover 时才出现的移除键：给个底，否则深色下 25% 的细线图标等于看不见
    if (hover) {
        c.ui.fill_rect(rm, c.th.fill3, 6.0f);
        c.ui.system_icon(SystemIcon::Close, rm.center(12, 12), c.th.label2);
    }

    c.ui.fill_rect({box.x + 34, box.bottom() - 1, box.w - 42, 1}, c.th.separator.scaled_alpha(0.5f));

    if (favorite_clicked) favorite_toggle(t);
    else if (rm_clicked) queue_remove(index);
    else if (clicked) start_play(index);
}

static void tab_queue(Ctx& c) {
    const float mx = kSidebar + 24.0f;
    const float mw = c.W - mx - 24.0f - (c.model.ui.lyrics_open ? kLyricsWidth : 0.0f);
    const int n = int(c.s.tracks.size());

    c.ui.heading("正在播放", {mx, 20, mw, 30});

    std::string info = std::to_string(n) + " 首";
    if (c.s.playing >= 0) info += " · 正在播第 " + std::to_string(c.s.playing + 1) + " 首";
    Style is;
    is.font_size = 11.0f;
    c.ui.draw_text(info, {mx, 54, 260, 18}, c.th.label3, is);

    bool loop = c.model.ui.loop_list;
    Style loop_s;
    loop_s.font_size = 11.0f;
    loop_s.valign = VAlign::Middle;
    c.ui.draw_text("列表循环", {mx + mw - 220, 50, 70, 20}, c.th.label2, loop_s);
    if (c.ui.toggle(&loop, {mx + mw - 144, 49, 38, 21})) {
        c.model.ui.loop_list = loop;
        c.model.ui.config_dirty = true;
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

static void local_playlist_row(Ctx& c, int index, const Rect& box) {
    // 点击按钮后只在本帧末更新容器，避免绘制引用在替换资料库时失效。
    const auto playlist = c.model.library.playlists[size_t(index)];
    const Rect remove{box.right() - 36, box.y + 19, 24, 24};
    const Rect refresh{remove.x - 62, box.y + 17, 54, 28};
    bool remove_clicked = false, refresh_clicked = false;
    if (index > 0) remove_clicked = c.ui.item("", "", "", remove, Style().corner(6));
    if (playlist.online()) refresh_clicked = c.ui.button("更新", refresh, Style().font(10.5f));
    const bool open_clicked = c.ui.item("", "", "", box, Style().corner(8));
    const Rect cov{box.x + 10, box.y + 10, 44, 44};
    cover_art(c, playlist.cover, playlist.name, cov, 7);
    const float tx = cov.right() + 12, width = std::max(20.0f, box.right() - tx - 120);
    c.ui.draw_text(ellipsize(c.ui, playlist.name, width, 13.5f, 600), {tx, box.y + 11, width, 18}, c.th.label, Style().font(13.5f).semi_bold());
    const std::string meta = std::to_string(playlist.tracks.size()) + " 首 · " +
        (playlist.online() ? std::string(music::source_name(playlist.source)) + " · 已保存曲目快照" : "保存在本机");
    c.ui.draw_text(ellipsize(c.ui, meta, width, 10.5f), {tx, box.y + 34, width, 16}, c.th.label3, Style().font(10.5f));
    if (index > 0) c.ui.system_icon(SystemIcon::Close, remove.center(12, 12), c.th.label3);
    c.ui.fill_rect({box.x + 10, box.bottom() - 1, box.w - 20, 1}, c.th.separator.scaled_alpha(0.6f));
    if (remove_clicked) {
        library_remove(size_t(index));
    } else if (refresh_clicked) library_fetch(playlist);
    else if (open_clicked) library_open(size_t(index));
}

static void tab_playlists(Ctx& c) {
    const float mx = kSidebar + 24.0f;
    const float mw = c.W - mx - 24.0f - (c.model.ui.lyrics_open ? kLyricsWidth : 0.0f);

    c.ui.heading("歌单", {mx, 20, mw, 30});
    int site = c.model.ui.pl_site;
    if (c.ui.segmented({"网易云", "QQ", "本地"}, &site, {mx, 58, 240, 28}, Style().font(11.5f))) c.model.ui.pl_site = site;

    if (c.model.ui.pl_site == 2) {
        c.ui.input(&c.model.ui.playlist_link, {mx, 102, mw - 106, 30}, Style().font(11.5f).hint("粘贴网易云 / QQ 公开歌单链接"));
        if (c.ui.button(c.model.ui.importing_playlist ? "保存中…" : "保存在线歌单", {mx + mw - 100, 102, 100, 30}, Style().font(10.5f))) library_import_link();
        c.ui.input(&c.model.ui.local_name, {mx, 144, mw - 106, 30}, Style().font(11.5f).hint("输入名称，将当前列表保存为本地歌单"));
        if (c.ui.button("保存当前列表", {mx + mw - 100, 144, 100, 30}, Style().font(10.5f))) library_save_current();
        c.ui.muted_text("歌曲旁的星标加入本地收藏；在线歌单的更新按钮会重新获取曲目。", {mx, 183, mw, 18}, Style().font(10.5f));
        const Rect list{mx, 212, mw, c.bar_y - 224};
        if (list.h >= 64) {
            const int count = int(c.model.library.playlists.size());
            c.ui.scroll(list, count, 64, [&](int i, const Rect& row) {
                if (i < int(c.model.library.playlists.size())) local_playlist_row(c, i, row);
            });
        }
        return;
    }

    const bool qq_side = (c.model.ui.pl_site == 1);
    const bool logged = qq_side ? c.model.qq.logged : c.model.netease.logged;
    const bool loading = qq_side ? c.model.qq.loading : c.model.netease.loading;
    const std::string err = qq_side ? c.model.qq.err : c.model.netease.err;
    const std::string nickname = qq_side ? c.model.qq.nickname : c.model.netease.nickname;
    const std::string uid = qq_side ? c.model.ui.qq_uin : c.model.ui.ne_uid;
    const int pl_count = int(qq_side ? c.model.qq.playlists.size() : c.model.netease.playlists.size());
    const char* site_name = qq_side ? "QQ 音乐" : "网易云音乐";

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
    const int pl_site = c.model.ui.pl_site;
    c.ui.scroll(list, pl_count, 64, [&](int i, const Rect& row) { playlist_row(c, pl_site, i, row); });
}

// ---------- 搜索页 ----------
static void tab_search(Ctx& c) {
    const float mx = kSidebar + 24.0f;
    // 歌词面板是从右边盖过来的，列表得让出位置，否则标题会被压在面板底下
    const float right_pad = 24.0f + (c.model.ui.lyrics_open ? kLyricsWidth : 0.0f);
    const float mw = c.W - mx - right_pad;

    const Rect field{mx, 20, mw - 100.0f, 32};
    c.ui.search_field(&c.model.ui.query, field, Style().hint("搜索歌曲 / 歌手 / 专辑"));
    if (c.ui.button(c.s.searching ? "搜索中…" : "搜索", {mx + mw - 92.0f, 20, 92, 32},
                    Style().primary().font(12.5f))) {
        start_search();
    }
    if (c.ui.input().key_pressed[KEY_ENTER]) start_search();

    // 音源筛选
    std::uint32_t mask = c.model.ui.src_mask;
    const Rect chips{mx, 62, 236, 24};
    if (c.ui.toggle_group_multi({"网易云", "QQ", "酷我"}, &mask, chips, Style().font(11.0f))) {
        c.model.ui.src_mask = mask;
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

    if (c.model.ui.scroll_to_top) {
        c.ui.scroll_to(0);
        c.model.ui.scroll_to_top = false;
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
    std::string* cookies[3] = {&c.model.ui.settings.netease_cookie, &c.model.ui.settings.qq_cookie, nullptr};

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
            static bool editing[3] = {false, false, false};
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
        y = card.bottom() + 12.0f;
    }
}

// ---------- 设置页 ----------

// 账号卡：site 0 = 网易云，1 = QQ
static void account_card(Ctx& c, int site, const Rect& box) {
    const bool qq_side = (site == 1);
    c.ui.panel(box, Style().corner(12));
    c.ui.subheading(qq_side ? "QQ 音乐" : "网易云音乐", {box.x + 20, box.y + 12, 240, 22});

    std::string note;
    const bool connecting = c.model.login[size_t(site)].connecting;
    note = c.model.login[size_t(site)].note;

    const bool logged = qq_side ? c.model.qq.logged : c.model.netease.logged;
    const std::string nickname = qq_side ? c.model.qq.nickname : c.model.netease.nickname;
    const std::string uid = qq_side ? c.model.ui.qq_uin : c.model.ui.ne_uid;
    const std::string err = qq_side ? c.model.qq.err : c.model.netease.err;
    const int count = int(qq_side ? c.model.qq.playlists.size() : c.model.netease.playlists.size());

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
        if (c.ui.button(connecting ? "取消连接" : "重新登录", {box.right() - 108, box.y + 38, 88, 28}, Style().font(11.0f))) {
            if (connecting) cancel_login(site); else begin_login(site);
        }
    } else {
        c.ui.muted_text("未登录", {box.x + 20, box.y + 44, 200, 18}, Style().font(11.0f));
        if (c.ui.button(connecting ? "取消连接" : "官网登录", {box.right() - 108, box.y + 38, 88, 28}, Style().primary().font(11.5f))) {
            if (connecting) cancel_login(site); else begin_login(site);
        }
    }

    if (!err.empty()) c.ui.muted_text(err, {box.x + 20, box.y + 84, box.w - 240, 18}, Style().font(10.0f));
    else if (!note.empty()) c.ui.muted_text(note, {box.x + 20, box.y + 84, box.w - 240, 18}, Style().font(10.0f));
}

static void storage_settings(Ctx& c, float mx, float mw) {
    c.ui.heading("数据与缓存", {mx, 20, mw - 120, 30});
    if (c.ui.button("返回设置", {mx + mw - 104, 21, 104, 28}, Style().font(11.5f))) c.model.ui.storage_settings = false;
    const auto config = c.model.paths.config;
    const auto cache_root = c.model.paths.cache;
    auto path_card = [&](const char* title, const std::string& path, float y) {
        const Rect box{mx, y, mw, 60};
        c.ui.panel(box, Style().corner(12));
        c.ui.draw_text(title, {mx + 20, y + 10, mw - 150, 18}, c.th.label, Style().font(12.0f));
        c.ui.muted_text(ellipsize(c.ui, path.empty() ? "目录不可用" : path, mw - 150, 10.5f), {mx + 20, y + 33, mw - 150, 16}, Style().font(10.5f));
        if (c.ui.button("打开目录", {box.right() - 112, y + 17, 92, 28}, Style().font(11.0f)))
            open_data_directory(path);
    };
    path_card("配置、收藏与歌单目录", config, 60);
    path_card("临时音频缓存目录", cache_root, 132);
    c.ui.muted_text("以下操作仅影响本机播放器，已下载的音乐文件会保留。", {mx, 204, mw, 18}, Style().font(11.0f));
    auto action_row = [&](const char* title, const char* detail, const char* button, ClearAction action, float y) {
        const Rect box{mx, y, mw, 64};
        c.ui.panel(box, Style().corner(12));
        const float width = std::max(1.0f, mw - 180);
        c.ui.draw_text(title, {mx + 20, y + 10, width, 20}, c.th.label, Style().font(12.0f));
        c.ui.muted_text(ellipsize(c.ui, detail, width, 10.5f), {mx + 20, y + 36, width, 16}, Style().font(10.5f));
        if (c.ui.button(button, {box.right() - 136, y + 18, 116, 28}, Style().destructive().font(11.0f))) c.model.ui.clear_action = action;
    };
    action_row("账号登录信息", "config.ini 中的 Cookie 与账号 ID；其他设置保留", "清除登录信息", ClearAction::Cookies, 232);
    action_row("本地收藏与保存的歌单", "library.json；包括本地收藏、命名歌单及在线歌单快照", "清除本地歌单", ClearAction::Library, 308);
    action_row("当前播放列表", "queue.txt；清除重启恢复的列表，当前歌曲可继续播放", "清空当前列表", ClearAction::Queue, 384);
    action_row("播放与封面缓存", "保留播放中、正在写入及其他运行实例使用的缓存", "清除缓存", ClearAction::Cache, 460);
}

static void clear_data_dialog(Ctx& c) {
    if (c.model.ui.clear_action == ClearAction::None) return;
    const auto action = c.model.ui.clear_action;
    const char* title = "清除缓存？";
    const char* detail = "清理临时音频和内存封面。播放中、正在写入及其他运行实例使用的文件会保留。";
    if (action == ClearAction::Cookies) { title = "清除登录信息？"; detail = "退出播放器中的网易云和 QQ 账号，清除本机保存的 Cookie。浏览器登录、本地歌单和其他设置不受影响。"; }
    else if (action == ClearAction::Library) { title = "清除本地收藏与歌单？"; detail = "删除本机全部收藏、命名歌单和保存的在线歌单快照。此操作无法撤销，不会修改音乐平台上的歌单。"; }
    else if (action == ClearAction::Queue) { title = "清空当前列表？"; detail = "清空当前列表及其重启恢复记录。正在播放的歌曲可继续播放，收藏和保存的歌单不受影响。"; }
    bool open = true;
    if (c.ui.alert_dialog(title, detail, "确认清除", "取消", &open,
        Rect::centered(c.W * 0.5f, c.H * 0.5f, std::min(440.0f, c.W - 40), 240), Style().destructive())) clear_data(action);
    if (!open) c.model.ui.clear_action = ClearAction::None;
}

static void update_settings(Ctx& c, float mx, float mw) {
    c.ui.heading("版本更新", {mx, 20, mw - 120, 30});
    if (c.ui.button("返回设置", {mx + mw - 104, 21, 104, 28}, Style().font(11.5f))) c.model.ui.update_settings = false;
    const Rect card{mx, 70, mw, 202};
    c.ui.panel(card, Style().corner(12));
    c.ui.subheading(std::string("aino player v") + kVersion, {mx + 20, 88, mw - 40, 24});
    c.ui.muted_text("启动时自动检查 GitHub 的最新正式版", {mx + 20, 124, mw - 40, 20}, Style().font(11.5f));
    const auto& update = c.model.update;
    c.ui.draw_text(ellipsize(c.ui, update.message.empty() ? "尚未检查更新" : update.message, mw - 40, 12),
        {mx + 20, 160, mw - 40, 22}, update.status == UpdateStatus::Failed ? c.th.destructive : c.th.label, Style().font(12));
    Style check_style = Style().primary().font(11.5f);
    check_style.disabled = update.status == UpdateStatus::Checking;
    if (c.ui.button(update.status == UpdateStatus::Checking ? "正在检查…" : "检查更新", {mx + 20, 210, 104, 30}, check_style)) check_updates();
    if (c.ui.button("GitHub 发布页", {mx + 138, 210, 120, 30}, Style().font(11.5f))) open_update(false);
    if (update.status == UpdateStatus::Available) {
        if (c.ui.button("下载新版", {mx + 272, 210, 104, 30}, Style().primary().font(11.5f))) open_update(true);
        c.ui.muted_text("最新版本 " + update.version, {mx + 20, 290, mw - 40, 20}, Style().font(12));
    }
    Style info = Style().font(11.5f); info.wrap_text();
    c.ui.draw_text("下载会交给系统默认浏览器。Windows 下载新版 exe 后替换旧程序；Linux 下载 deb 后通过软件安装器升级，原有配置与收藏继续保留。", {mx + 20, 330, mw - 40, 92}, c.th.label3, info);
}

static void tab_settings(Ctx& c) {
    const float mx = kSidebar + 24.0f;
    const float mw = c.W - mx - 24.0f - (c.model.ui.lyrics_open ? kLyricsWidth : 0.0f);
    if (c.model.ui.storage_settings) { storage_settings(c, mx, mw); return; }
    if (c.model.ui.update_settings) { update_settings(c, mx, mw); return; }
    c.ui.heading("设置", {mx, 20, mw, 30});
    if (c.ui.button(c.model.update.status == UpdateStatus::Available ? "发现新版" : "版本更新", {mx + mw - 224, 21, 100, 28}, Style().font(11.5f))) c.model.ui.update_settings = true;
    if (c.ui.button("数据与缓存", {mx + mw - 112, 21, 112, 28}, Style().font(11.5f))) c.model.ui.storage_settings = true;

    account_card(c, 0, {mx, 60, mw, 110});
    account_card(c, 1, {mx, 182, mw, 110});

    // ---------------- 外观 ----------------
    const Rect look{mx, 304, mw, 56};
    c.ui.panel(look, Style().corner(12));
    Style lbl;
    lbl.font_size = 11.5f;
    lbl.valign = VAlign::Middle;
    c.ui.draw_text("深色模式", {look.x + 20, look.y + 18, 160, 20}, c.th.label2, lbl);
    bool dark = c.model.ui.dark;
    if (c.ui.toggle(&dark, {look.right() - 60, look.y + 17, 40, 22})) {
        c.model.ui.dark = dark;
        c.model.ui.config_dirty = true;
    }

    // ---------------- 音质 ----------------
    const Rect qcard{mx, 372, mw, 56};
    c.ui.panel(qcard, Style().corner(12));
    c.ui.draw_text("默认音质", {qcard.x + 20, qcard.y + 18, 120, 20}, c.th.label2, lbl);
    int q = c.model.ui.settings.quality;
    if (c.ui.segmented({"标准", "高", "无损"}, &q, {qcard.right() - 232, qcard.y + 15, 212, 26},
                       Style().font(11.0f))) {
        c.model.ui.settings.quality = q;
        c.model.ui.config_dirty = true;
    }

    // ---------------- 播放 ----------------
    const Rect card{mx, 440, mw, 84};
    c.ui.panel(card, Style().corner(12));
    c.ui.draw_text("默认音量", {card.x + 20, card.y + 12, 120, 16}, c.th.label2, Style().font(11.5f));
    static SeekBar set_vol_bar;
    float vol_to = 0.0f;
    if (seek_bar(c, c.model.ui.volume, 100.0f, {card.x + 20, card.y + 34, card.w - 90, 20}, &set_vol_bar, &vol_to))
        set_volume(vol_to);
    c.ui.draw_text(std::to_string(int(c.model.ui.volume)), {card.right() - 64, card.y + 34, 44, 20}, c.th.label2,
                   Style().font(11.0f).right());
    char buf[256];
    std::snprintf(buf, sizeof buf, "缓存目录  %s", c.model.paths.current_cache.c_str());
    c.ui.muted_text(buf, {card.x + 20, card.y + 60, card.w - 40, 16}, Style().font(10.0f));

    // ---------------- 下载 ----------------
    const Rect dlcard{mx, 536, mw, 110};
    c.ui.panel(dlcard, Style().corner(12));
    c.ui.draw_text("下载目录", {dlcard.x + 20, dlcard.y + 10, 160, 16}, c.th.label2, Style().font(11.5f));
    if (c.ui.input(&c.model.ui.download_dir, {dlcard.x + 20, dlcard.y + 30, dlcard.w - 40, 28},
                   Style().hint(default_download_dir()).font(11.5f)))
        c.model.ui.config_dirty = true;

    // 内嵌：默认都关（会改写文件，得让用户自己决定）
    bool emb_cover = c.model.ui.settings.embed_cover;
    c.ui.draw_text("内嵌封面", {dlcard.x + 20, dlcard.y + 68, 80, 20}, c.th.label2, lbl);
    if (c.ui.toggle(&emb_cover, {dlcard.x + 104, dlcard.y + 67, 40, 22})) {
        c.model.ui.settings.embed_cover = emb_cover;
        c.model.ui.config_dirty = true;
    }
    bool emb_lyric = c.model.ui.settings.embed_lyrics;
    c.ui.draw_text("内嵌歌词", {dlcard.x + 176, dlcard.y + 68, 80, 20}, c.th.label2, lbl);
    if (c.ui.toggle(&emb_lyric, {dlcard.x + 260, dlcard.y + 67, 40, 22})) {
        c.model.ui.settings.embed_lyrics = emb_lyric;
        c.model.ui.config_dirty = true;
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
// 结构：左「封面 + 两行信息」｜中「控制键 + 进度条」固定宽度块｜右「收藏 / 歌词 / 下载 / 音量」
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
    float dur = c.model.ui.dur_ms > 0 ? c.model.ui.dur_ms : 1.0f;
    static SeekBar prog_bar;
    float seek_to = 0.0f;
    const Rect prog_box{cx - 166.0f, prog_cy - 10.0f, 332.0f, 20.0f};
    if (seek_bar(c, std::min(c.model.ui.pos_ms, dur), dur, prog_box, &prog_bar, &seek_to)) {
        seek_playback(int(seek_to));
        c.model.ui.pos_ms = seek_to;
    }
    Style tm;
    tm.font_size = 10.0f;
    tm.valign = VAlign::Middle;
    tm.align = Align::Right;
    c.ui.draw_text(fmt_time((long long)c.model.ui.pos_ms), {cx - 210.0f, prog_cy - 8.0f, 36.0f, 16}, c.th.label3, tm);
    Style tm2 = tm;
    tm2.align = Align::Left;
    c.ui.draw_text(fmt_time((long long)c.model.ui.dur_ms), {cx + 174.0f, prog_cy - 8.0f, 36.0f, 16}, c.th.label3, tm2);

    // ---------------- 右：收藏 / 歌词 / 下载 / 音量 ----------------
    const float num_w = 24.0f, slider_w = 80.0f, spk_w = 18.0f;
    float x = c.W - pad;

    Style vn;
    vn.font_size = 10.5f;
    vn.valign = VAlign::Middle;
    vn.align = Align::Right;
    c.ui.draw_text(std::to_string(int(c.model.ui.volume)), {x - num_w, ctrl_cy - 8.0f, num_w, 16}, c.th.label3, vn);
    x -= num_w + 6.0f;

    static SeekBar vol_bar;
    float vol_to = 0.0f;
    if (seek_bar(c, c.model.ui.volume, 100.0f, {x - slider_w, ctrl_cy - 10.0f, slider_w, 20.0f}, &vol_bar, &vol_to))
        set_volume(vol_to);
    x -= slider_w + 8.0f;

    draw_speaker(c.ui, {x - spk_w, ctrl_cy - 9.0f, spk_w, 18}, c.th.label2, c.model.ui.volume);
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
            download_current();
        }
    }
    // 底栏的下载按钮：正在下载显示进度数字，完成显示对勾
    {
        int pct = -1;
        bool done = false, failed = false;
        if (auto it = c.model.downloads.find(c.s.now_uid); it != c.model.downloads.end()) {
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
        c.model.ui.lyrics_open = !c.model.ui.lyrics_open;
        if (c.model.ui.lyrics_open) request_current_lyrics();
    }
    draw_lyric_icon(c.ui, lyr_btn.center(15, 13), c.model.ui.lyrics_open ? c.th.tint : c.th.label2);

    // 独立保存当前曲目的快照，避免搜索结果变化后收藏到别的歌曲。
    const Rect fav_btn{lyr_btn.x - 36.0f, lyr_btn.y, 28.0f, 28.0f};
    const bool can_favorite = !c.s.now_uid.empty() && !c.s.now_track.variants.empty();
    const bool saved = can_favorite && c.model.library.contains(c.s.now_track);
    Style favorite_style = Style().ghost();
    favorite_style.disabled = !can_favorite;
    if (c.ui.icon_button("", fav_btn, favorite_style)) favorite_toggle(c.s.now_track);
    if (saved) c.ui.fill_rect(fav_btn, c.th.tint.scaled_alpha(0.12f), 6);
    c.ui.system_icon(SystemIcon::Star, fav_btn.center(16, 16), saved ? c.th.tint : (can_favorite ? c.th.label2 : c.th.label4));
}

// ---------- Toast：一次性提示（下载完成 / 取流失败 / 歌单操作） ----------
// 底栏不再承载长错误文本 —— 那句"取流失败 — 网易云 会员曲…"以前挤在右下角，
// 一长就压到别的控件上。这些消息现在浮在底栏上方，几秒后自己消失。
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
    c.ui.fill_rect(box, c.th.control_bg);
    c.ui.fill_rect({box.x, box.y, 1, box.h}, c.th.separator);

    Style h;
    h.font_size = 13.0f;
    h.font_weight = 600;
    c.ui.draw_text("歌词", {box.x + 20, box.y + 15, 120, 22}, c.th.label, h);

    const Rect close{box.right() - 38, box.y + 14, 24, 24};
    if (c.ui.icon_button("", close, Style().ghost())) c.model.ui.lyrics_open = false;
    c.ui.system_icon(SystemIcon::Close, close.center(11, 11), c.th.label3);

    const auto& doc = c.model.lyrics.doc;
    const auto& uid = c.model.lyrics.uid;
    const bool loading = c.model.lyrics.loading;

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

    const int cur_line = doc->synced ? lyr::current_line(doc->lines, (long long)c.model.ui.pos_ms) : -1;
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
            seek_playback(int(doc->lines[size_t(sline)].ms));
            c.model.ui.pos_ms = float(doc->lines[size_t(sline)].ms);
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

    switch (c.model.ui.tab) {
        case 0: tab_search(c); break;
        case 1: tab_discover(c); break;
        case 2: tab_queue(c); break;
        case 3: tab_playlists(c); break;
        case 4: tab_sources(c); break;
        default: tab_settings(c); break;
    }
    if (c.model.ui.lyrics_open) {
        lyrics_panel(c, {c.W - kLyricsWidth, 0, kLyricsWidth, c.bar_y});
    }
    player_bar(c);
    draw_toast(c);
    clear_data_dialog(c);
}

void draw(aino::Ui& ui, ViewModel& model) {
    Ctx context{ui, ui.theme(), ui.width(), ui.height(), ui.height() - kBarHeight, model.playback, model};
    compose(ui, context);
}
} // namespace player::ui
