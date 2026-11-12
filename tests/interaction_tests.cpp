// 默认不创建窗口；平台窗口检查通过 --linux-backend / --ime-backend 显式启用。
#define AINO_IMPLEMENTATION
#include "../src/aino.hpp"
#include <iostream>
#include <stdexcept>

static void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static std::vector<unsigned char> scale_settings(bool little, unsigned dpi, unsigned factor) {
    std::vector<unsigned char> data(4); data[0] = little ? 0 : 1;
    auto append = [&](unsigned v, int n) { for (int i = 0; i < n; ++i) data.push_back(uint8_t(v >> (8 * (little ? i : n - 1 - i)))); };
    append(1, 4); append(2, 4);
    for (const auto& entry : {std::make_pair(std::string("Xft/DPI"), dpi), std::make_pair(std::string("Gdk/WindowScalingFactor"), factor)}) {
        append(0, 2); append(unsigned(entry.first.size()), 2);
        data.insert(data.end(), entry.first.begin(), entry.first.end());
        while (data.size() % 4) data.push_back(0);
        append(1, 4); append(entry.second, 4);
    }
    return data;
}
#ifdef AINO_PLATFORM_X11
static void linux_backend_checks() {
    using namespace aino;
    check(XInitThreads(), "Xlib threads");
    unsetenv("AINO_SCALE"); unsetenv("GDK_SCALE");
    auto* desktop = XOpenDisplay(nullptr); check(desktop != nullptr, "Xvfb display");
    const auto root = DefaultRootWindow(desktop);
    const auto selection = XInternAtom(desktop, "_XSETTINGS_S0", False);
    const auto property = XInternAtom(desktop, "_XSETTINGS_SETTINGS", False);
    const auto owner = XCreateSimpleWindow(desktop, root, 0, 0, 1, 1, 0, 0, 0);
    auto publish = [&](unsigned dpi, unsigned factor) {
        const auto bytes = scale_settings(true, dpi * 1024, factor);
        XChangeProperty(desktop, owner, property, property, 8, PropModeReplace, bytes.data(), int(bytes.size()));
        XSync(desktop, False);
    };
    publish(192, 2); XSetSelectionOwner(desktop, selection, owner, CurrentTime); XSync(desktop, False);
    {
        WindowConfig config; config.width = 220; config.height = 140; config.min_width = 120; config.min_height = 90;
        aino::Window window(config); check(window.ok(), "scaled window creation");
        auto* state = static_cast<detail::X11State*>(window.impl->native);
        check(window.dpi_scale() == 2 && state->pw == 440 && state->ph == 280, "desktop scale applies to physical window");
        XSizeHints hints{}; long supplied = 0;
        XGetWMNormalHints(state->dpy, state->win, &hints, &supplied);
        check(hints.min_width == 240 && hints.min_height == 180, "scaled minimum dimensions");
        XEvent motion{}; motion.type = MotionNotify; motion.xmotion.x = 40; motion.xmotion.y = 60;
        detail::x11_handle(state, motion); detail::x11_feed_input(state);
        check(window.impl->ui.impl->input.mouse.x == 20 && window.impl->ui.impl->input.mouse.y == 30, "scaled input coordinates");
        publish(144, 1);
        XSync(state->dpy, False); window.poll();
        check(window.dpi_scale() == 1.5f, "running window responds to desktop DPI changes");
        window.set_size(200, 120); XSync(state->dpy, False); window.poll();
        XWindowAttributes attributes{}; XGetWindowAttributes(state->dpy, state->win, &attributes);
        check(attributes.width == 300 && attributes.height == 180, "set_size accepts logical dimensions");
    }
    {
        publish(192, 2); setenv("AINO_SCALE", "3", 1);
        WindowConfig config; config.width = 220; config.height = 140; config.high_dpi = false;
        aino::Window window(config); check(window.ok(), "unscaled window creation");
        check(window.dpi_scale() == 1 && window.width() == 220 && window.height() == 140,
              "high_dpi=false ignores desktop scale and environment override");
        unsetenv("AINO_SCALE");
    }
    XDestroyWindow(desktop, owner); XCloseDisplay(desktop);
    detail::X11TextRenderer renderer; renderer.init(1, "sans", "monospace");
    const auto width = renderer.measure("字体共享 Memory", 14, 400);
    check(width > 0, "FreeType text metrics");
    for (int size = 10; size <= 40; ++size) renderer.measure("字体共享 Memory", float(size), 400);
    check(renderer.measure("字体共享 Memory", 14, 400) == width, "shared faces retain per-size glyph metrics");
    const auto shared = renderer.cache_stats();
    check(shared.faces > 0 && shared.faces < shared.fonts, "font faces are shared across sizes");
    std::string glyphs;
    for (uint32_t cp = 0x4e00; cp < 0x6e00; ++cp) {
        glyphs.push_back(char(0xe0 | (cp >> 12))); glyphs.push_back(char(0x80 | ((cp >> 6) & 63))); glyphs.push_back(char(0x80 | (cp & 63)));
    }
    renderer.measure(glyphs, 40, 400);
    check(renderer.cache_stats().glyph_bytes <= 4u * 1024u * 1024u, "glyph bitmap cache remains bounded");
    std::cout << "Linux fonts=" << shared.fonts << " shared_faces=" << shared.faces << " glyph_bytes=" << renderer.cache_stats().glyph_bytes << '\n';
    renderer.release(); check(renderer.cache_stats().faces == 0, "shared font faces released once");
    renderer.init(1, "sans", "monospace"); renderer.measure("A", 14, 400);
    const auto before_missing = renderer.cache_stats().faces;
    renderer.measure("\xf4\x8f\xbf\xbf", 14, 400);
    check(renderer.cache_stats().faces == before_missing, "missing glyph does not open unrelated fallback fonts");
    renderer.release();
    for (const auto icon : {SystemIcon::Play, SystemIcon::Pause, SystemIcon::PreviousTrack, SystemIcon::NextTrack,
                           SystemIcon::Shuffle, SystemIcon::RepeatOne, SystemIcon::RepeatAll, SystemIcon::Timer}) {
        const auto mask = detail::load_system_icon(icon, 16);
        check(mask.width > 0 && mask.height > 0 && !mask.alpha.empty(), "Linux media icons load from system themes");
    }
}
#endif
// 自绘输入与原生组件共享浮层闸门；关闭回调不得留下下一帧闸门。
static void overlay_input_checks() {
    using namespace aino;
    Ui ui; auto& input = ui.impl->input;
    const Rect trigger{350, 265, 30, 30};
    bool open = true, expect_blocked = false, masked_ok = true;
    bool close_inside = false, close_before_end = false, visible = false;
    bool inner_press = false, raw_press = false;
    int callbacks = 0, inner_clicks = 0, background_clicks = 0;
    auto sample = [&] {
        if (!expect_blocked) return;
        const auto& in = ui.input();
        masked_ok = masked_ok && !ui.pointer_available() && in.wheel == 0 &&
            !in.double_click && in.click_count == 0 && !in.dragging &&
            in.mouse_delta.x == 0 && in.mouse_delta.y == 0 &&
            in.mouse.x == input.mouse.x && in.mouse.y == input.mouse.y;
        for (int i = 0; i < 3; ++i)
            masked_ok = masked_ok && !in.down[i] && !in.pressed[i] && !in.released[i];
        check(in.key_pressed[KEY_ENTER] == input.key_pressed[KEY_ENTER] &&
              in.text_input == input.text_input, "pointer filtering preserves keyboard input");
    };
    auto draw = [&] {
        ui.begin_frame(400, 300, .016f, 1);
        sample();
        if (detail::interact(ui.impl, 8101, {0, 0, 340, 300}, true).clicked) ++background_clicks;
        visible = ui.popover("popup", &open, trigger, {200, 100}, [&](const Rect& inner) {
            ++callbacks;
            inner_press = inner_press || (ui.pointer_available() && ui.input().pressed[0]);
            if (ui.button("inner", inner)) ++inner_clicks;
            if (close_inside) open = false;
        });
        sample();
        if (detail::interact(ui.impl, 8102, {0, 0, 340, 300}, true).clicked) ++background_clicks;
        raw_press = ui.raw_input().pressed[0];
        if (close_before_end) open = false;
        ui.end_frame();
    };
    draw();
    check(ui.impl->gate_next_allow_n == 2, "popover registers only its live trigger and panel");
    const auto panel = ui.impl->gate_next_allow[1];
    input.mouse = {panel.cx(), panel.cy()};
    for (int i = 0; i < 3; ++i) input.down[i] = input.pressed[i] = true;
    input.wheel = 2; input.double_click = true; input.click_count = 2;
    input.mouse_delta = {3, 4}; input.dragging = true;
    input.key_pressed[KEY_ENTER] = true; input.text_input = "a";
    expect_blocked = true; draw();
    check(masked_ok && inner_press && raw_press,
          "custom background input is masked while the popup and raw_input retain the press");
    for (int i = 0; i < 3; ++i) { input.down[i] = false; input.released[i] = true; }
    input.dragging = false; draw();
    check(masked_ok && inner_clicks == 1 && background_clicks == 0,
          "popup button receives release without leaking to native or custom background controls");
    const int before = callbacks;
    input.mouse = {10, 10}; input.down[0] = true; input.pressed[0] = true;
    draw();
    check(!open && !visible && callbacks == before && !ui.impl->gate_next && masked_ok,
          "outside press dismisses without running the closed callback or renewing its gate");
    expect_blocked = false; input.down[0] = false; input.released[0] = true; draw();
    check(!ui.impl->gate_on && background_clicks == 0, "closing gesture does not activate background controls");
    input.down[0] = true; input.pressed[0] = true; draw();
    input.down[0] = false; input.released[0] = true; draw();
    check(background_clicks == 1, "next background click works immediately after dismissal");
    open = true; draw();
    const int before_escape = callbacks;
    input.key_pressed[KEY_ESC] = true; expect_blocked = true; draw();
    check(!open && !visible && callbacks == before_escape && !ui.impl->gate_next,
          "Escape dismisses without executing a stale popover callback");
    expect_blocked = false; open = true; close_inside = true; draw();
    check(!open && !ui.impl->gate_next, "closing inside the deferred callback leaves no stale gate");
    close_inside = false; close_before_end = true; open = true;
    const int before_cancel = callbacks; draw();
    check(callbacks == before_cancel && !ui.impl->gate_next,
          "popover cancelled before frame end does not execute or retain input");

    input.mouse = {10, 10}; input.pressed[0] = input.down[0] = true;
    ui.begin_frame(400, 300, .016f, 1);
    ui.push_clip({20, 20, 30, 30});
    check(!ui.pointer_available() && !ui.input().pressed[0] && ui.raw_input().pressed[0],
          "custom input obeys clipping without consuming the original event");
    ui.pop_clip();
    check(ui.pointer_available() && ui.input().pressed[0], "leaving clip restores visible input");
    ui.begin_measure();
    check(!ui.pointer_available() && !ui.input().pressed[0], "measurement cannot activate custom input");
    ui.end_measure();
    check(ui.input().pressed[0], "measurement preserves the real frame input");
    ui.end_frame();
}

static void overlay_menu_lifecycle_checks() {
    using namespace aino;
    for (int kind = 0; kind < 3; ++kind) {
        Ui ui; auto& input = ui.impl->input;
        const Rect trigger{20, 20, 80, 24};
        const std::vector<std::string> items{"one", "two", "three"};
        int choice = -1, selected = 0, background = 0;
        auto draw = [&] {
            ui.begin_frame(400, 300, .016f, 1);
            if (detail::interact(ui.impl, 8201, {0, 80, 400, 220}, true).clicked) ++background;
            if (kind == 0) ui.menu("menu", items, &choice, trigger);
            else if (kind == 1) ui.select(items, &selected, trigger);
            else ui.context_menu(trigger, items, &choice);
            ui.end_frame();
        };
        auto press = [&](float x, float y, int button) {
            input.mouse = {x, y}; input.down[button] = input.pressed[button] = true; draw();
        };
        auto release = [&](int button) {
            input.down[button] = false; input.released[button] = true; draw();
        };
        auto open = [&] { const int button = kind == 2 ? 1 : 0; press(trigger.cx(), trigger.cy(), button); release(button); };
        open();
        check(ui.impl->gate_next_allow_n > 0, "open menu registers its live gate");
        const auto panel = ui.impl->gate_next_allow[ui.impl->gate_next_allow_n - 1];
        press(panel.cx(), panel.y + 4 + 24 + 12, 0); release(0);
        check((kind == 1 ? selected : choice) == 1 && !ui.impl->gate_next && background == 0,
              "selecting an item does not renew the closed menu gate or click through");
        open(); press(10, 280, 0);
        check(!ui.impl->gate_next && background == 0, "outside press closes menu without renewing its gate");
        release(0); draw();
        check(!ui.impl->gate_on && background == 0, "closed menu releases the background gate");
        open(); input.key_pressed[KEY_ESC] = true; draw();
        check(!ui.impl->gate_next, "Escape closes every menu type without a stale gate");
    }
}

static void menu_checks() {
    using namespace aino;
    for (const bool timer : {false, true}) {
        Ui ui; auto& input = ui.impl->input;
        const Rect trigger{350, 265, 30, 30};
        const std::vector<std::string> items = timer
            ? std::vector<std::string>{"Timer off", "", "15 min", "30 min", "60 min", "90 min", "After track", "", "Cancel"}
            : std::vector<std::string>{"Sequential", "Shuffle", "Repeat all", "Repeat one"};
        MenuOptions options; options.selected = timer ? 6 : 2;
        if (timer) options.disabled = {0, 8};
        int choice = -1, background_clicks = 0; bool pointer = true;
        const Style style = Style().ghost().font(12).indicator(false);
        auto draw = [&] {
            ui.begin_frame(400, 300, .016f, 1);
            pointer = ui.pointer_available();
            if (detail::interact(ui.impl, 900, {0, 0, 340, 300}, true).clicked) ++background_clicks;
            ui.menu("", items, &choice, trigger, style, options);
            ui.end_frame();
        };
        auto press = [&](float x, float y) { input.mouse = {x, y}; input.down[0] = true; input.pressed[0] = true; draw(); };
        auto release = [&] { input.down[0] = false; input.released[0] = true; draw(); };
        auto open = [&] { press(trigger.cx(), trigger.cy()); release(); };
        open();
        check(ui.impl->gate_next_allow_n == 2, "menu registers trigger and overlay input regions");
        const auto panel = ui.impl->gate_next_allow[1];
        for (const auto& label : items) check(panel.w >= ui.text_width(label, 12, 500) + 40,
              "checked menu reserves enough width for its check column and complete labels");
        check(panel.h == (timer ? 194 : 104) && panel.right() <= 396 && panel.y >= 4,
              "timer and playback menus use compact rows and stay inside the window");
        if (timer) {
            press(panel.cx(), panel.y + 16); release();
            check(choice == -1 && !pointer && background_clicks == 0, "disabled status does not activate or click through");
            press(panel.cx(), panel.y + 32); release();
            check(choice == -1 && background_clicks == 0, "separator does not activate or click through");
            press(panel.cx(), panel.bottom() - 16); release();
            check(choice == -1 && background_clicks == 0, "disabled cancel remains blocked");
        }
        const float row_y = timer ? panel.y + 4 + 24 + 9 + 12 : panel.y + 4 + 24 + 12;
        press(panel.cx(), row_y); release();
        check(choice == (timer ? 2 : 1) && background_clicks == 0 && !pointer,
              "menu selection activates only the selected row and blocks raw mouse readers");
        draw();
        press(10, 10); release();
        check(background_clicks == 1 && pointer, "normal input resumes after closing menu");
        choice = -1; open();
        press(10, 10); release();
        check(background_clicks == 1 && choice == -1, "outside click only dismisses menu");
        draw(); open();
        input.key_pressed[KEY_ESC] = true; draw(); draw();
        check(!ui.impl->gate_on && choice == -1, "Escape dismisses menu and releases overlay gate");
    }
}
#ifdef _WIN32
static void ime_backend_checks() {
    using namespace aino;
    Window window(320,200,"IME candidate position"); check(window.ok(),"IME probe window");
    auto* state = static_cast<detail::Win32State*>(window.impl->native);
    auto* ui = window.impl->ui.impl;
    ui->ime_owner = true;
    detail::win_set_ime_pos(state,48,32,54);
    struct Position { DWORD size,index; POINT point; UINT height; RECT document; } position{};
    position.size=sizeof position;
    check(detail::win_proc(state->hwnd,WM_IME_REQUEST,0x0006,reinterpret_cast<LPARAM>(&position)) == TRUE,"IME cursor position request handled");
    POINT expected{48,32}; ClientToScreen(state->hwnd,&expected);
    check(position.point.x==expected.x && position.point.y==expected.y && position.height==22,"IME receives screen coordinates and caret height");
    bool drawn=false;
    window.impl->frame_cb_ptr = [&] { drawn=true; };
    ui->frame_open=true; window.render(); ui->frame_open=false;
    check(!drawn && window.impl->need_frame,"native nested render defers drawing until the open frame ends");
}
#endif
int main(int argc, char** argv) {
    try {
        using namespace aino::detail;
        for (const bool little : {false, true}) {
            const auto bytes = scale_settings(little, 144 * 1024, 1);
            const auto parsed = parse_xsettings_scale(bytes.data(), bytes.size());
            check(parsed.dpi == 144 && parsed.factor == 1, "XSETTINGS endian and padding");
            for (size_t n = 0; n < bytes.size(); ++n) check(parse_xsettings_scale(bytes.data(), n).dpi == 0, "truncated settings rejected without partial scale");
        }
        check(desktop_scale("1.25", nullptr, {192, 2}, 96) == 1.25f, "explicit fractional scale override");
        check(desktop_scale("nan", "2", {}, 96) == 2, "invalid override falls back to GDK_SCALE");
        check(desktop_scale("8", nullptr, {}, 144) == 1.5f, "resource DPI fallback and override bounds");
        check(desktop_scale(nullptr, nullptr, {192, 2}, 96) == 2, "desktop settings are not multiplied twice");
        check(desktop_scale(nullptr, nullptr, {}, 0) == 1 && parse_scale_number("1,5") == 0, "safe default and locale independent scale");
#ifdef AINO_PLATFORM_X11
        if (argc > 1 && std::string(argv[1]) == "--linux-backend") linux_backend_checks();
#endif
#ifdef _WIN32
        if (argc > 1 && std::string(argv[1]) == "--ime-backend") ime_backend_checks();
#endif
        aino::Ui ui;
        auto& input = ui.impl->input;
        const aino::Rect box{0, 0, 100, 30};
        auto frame = [&] { ui.begin_frame(400, 300, .016f, 1); };
        auto click = [&](uint64_t id) {
            input.mouse = {10, 10}; input.down[0] = true; input.pressed[0] = true;
            frame(); check(aino::detail::interact(ui.impl, id, box, true).pressed, "next button must accept press"); ui.end_frame();
            input.down[0] = false; input.released[0] = true;
            frame(); check(aino::detail::interact(ui.impl, id, box, true).clicked, "next button must accept click"); ui.end_frame();
        };
        for (const bool release_delivered : {true, false}) {
            input.mouse = {10, 10}; input.down[0] = true; input.pressed[0] = true;
            frame(); check(aino::detail::interact(ui.impl, 1, box, true).pressed, "fixture starts active control"); ui.end_frame();
            input.down[0] = false; input.released[0] = release_delivered;
            frame();
            // 原按钮已被刷新掉，另一个控件不能替原按钮清理 active。
            aino::detail::interact(ui.impl, 2, {100, 100, 100, 30}, true);
            ui.end_frame();
            check(ui.impl->active == 0, "removed control releases capture even without a delivered mouse-up");
            click(3);
        }
        // 正常跨帧拖动不能被帧末清理打断。
        input.down[0] = true; input.pressed[0] = true;
        frame(); aino::detail::interact(ui.impl, 4, box, true); ui.end_frame();
        frame(); check(aino::detail::interact(ui.impl, 4, box, true).held, "drag remains held while mouse is down"); ui.end_frame();
        input.down[0] = false; input.released[0] = true;
        frame(); check(aino::detail::interact(ui.impl, 4, box, true).clicked, "drag releases normally"); ui.end_frame();
        click(5);
        menu_checks();
        overlay_input_checks();
        overlay_menu_lifecycle_checks();
        std::cout << "UI capture lifecycle regression checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
