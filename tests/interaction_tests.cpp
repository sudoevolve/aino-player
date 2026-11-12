// 不创建窗口，直接重现控件消失、坐标变化和失去鼠标捕获后的输入生命周期。
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
}
#endif
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
        std::cout << "UI capture lifecycle regression checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
