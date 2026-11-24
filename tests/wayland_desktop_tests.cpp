// 发布时由 run_wayland.py 在无 DISPLAY 的 compositor 和独立 session bus 中执行。
#include "../src/platform.hpp"
#include "../src/wayland_lyrics.hpp"
#include <iostream>
#include <stdexcept>

static void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    try {
        check(platform::env("DISPLAY").empty(), "native desktop acceptance must not have DISPLAY");
        platform::Desktop desktop;
        platform::ShortcutSettings shortcuts;
        shortcuts.enabled = false;
        desktop.set_shortcuts(shortcuts);
        desktop.start(nullptr, nullptr, 0, [] {});
        platform::WaylandLyrics lyrics;
        platform::LyricOverlaySettings settings;
        settings.enabled = true;
        bool painted = false;
        const auto paint = [&](uint8_t *pixels, int width, int height, float, void *) {
            check(width > 0 && height > 0, "lyrics receive a valid SHM canvas");
            for (size_t i = 0; i < size_t(width) * height * 4; i += 4) {
                pixels[i] = 24;
                pixels[i + 1] = 48;
                pixels[i + 2] = 96;
                pixels[i + 3] = 128;
            }
            painted = true;
        };
        const auto wait = [&](const std::function<bool()> &done) {
            const auto deadline = platform::ticks() + 5000;
            while (!done() && platform::ticks() < deadline) {
                lyrics.update(settings, "Wayland lyrics", "", {}, 1, paint);
                platform::sleep_ms(5);
            }
            check(done(), "native desktop operation completes without XWayland");
        };
        wait([&] { return lyrics.ready() && painted && desktop.media_available(); });
        if (!platform::env("AINO_REQUIRE_LAYER_SHELL").empty())
            check(lyrics.uses_layer_shell(), "layer-shell acceptance must exercise the overlay protocol");
        settings.locked = true;
        settings.width = 800;
        settings.double_line = false;
        painted = false;
        wait([&] { return painted; });
        settings.enabled = false;
        lyrics.update(settings, "", "", {}, 1, paint);
        check(!lyrics.ready(), "disabling lyrics releases the native surface");
        settings.enabled = true;
        painted = false;
        wait([&] { return lyrics.ready() && painted; });
        lyrics.close();
        desktop.stop();
        std::cout << "Pure Wayland lyrics and D-Bus media integration passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
