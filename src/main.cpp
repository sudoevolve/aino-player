#define AINO_IMPLEMENTATION
#include "aino.hpp"
#ifdef AINO_PLATFORM_X11
#include <X11/Xlib.h>
// Xlib 的遗留宏不能污染后续 JSON / C++ 业务头文件。
#undef Bool
#undef Status
#undef None
#endif

#include "business.hpp"
#include "ui.hpp"
#include <chrono>
#include <algorithm>
#include <cstdio>
int main(int argc, char** argv) {
    using namespace player;
    if (argc > 1 && std::string(argv[1]) == "--version") {
        std::printf("aino player %s\n", kVersion); return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--codec-info") {
        std::printf("%s\n", audio::decoder_info().c_str()); return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--ffmpeg-license") {
        std::printf("%s\n", audio::decoder_license().c_str()); return 0;
    }
#ifdef AINO_PLATFORM_X11
    // 后台任务调用 wake() 前，必须先初始化 Xlib 的线程支持。
    if (!XInitThreads()) { std::fprintf(stderr, "Xlib thread initialization failed\n"); return 1; }
#endif
    auto view = view_model();
    bool initial_search = argc > 1;
    bool smoke_test = argc > 1 && std::string(argv[1]) == "--smoke-test";
#ifdef AINO_PLATFORM_X11
    const bool diagnostics = argc > 1 && std::string(argv[1]) == "--diagnostics";
    smoke_test = smoke_test || diagnostics;
#endif
#ifdef _WIN32
    int count = 0;
    if (auto args = CommandLineToArgvW(GetCommandLineW(), &count)) {
        initial_search = count > 1;
        if (initial_search) view.ui.query = net::narrow(args[1]);
        LocalFree(args);
    }
#else
    if (initial_search) view.ui.query = argv[1];
#endif
    if (smoke_test) initial_search = false;
    if (initial_search) view.ui.tab = 0;
    if (!initialize(smoke_test)) return 1;
    aino::WindowConfig cfg;
    cfg.width = 1180;
    cfg.height = 760;
    cfg.min_width = 980;
    cfg.min_height = 640;
    cfg.title = "aino player";
    cfg.center = true;
    aino::Window win(cfg);
    if (!win.ok()) {
        std::fprintf(stderr, "Cannot create aino player window\n");
        return 1;
    }
#ifdef _WIN32
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
#endif
    bool theme_dark = view.ui.dark;
    win.set_theme(theme_dark ? aino::Theme::dark() : aino::Theme::light());
    win.set_anim_fps(60.0f);

    start({[&win] { win.wake(); }, [&win] { win.activate(); }});
    bool autostart = true;
    win.run([&](aino::Ui& ui) {
        const auto begin = std::chrono::steady_clock::now();
        tick();
        auto model = view_model();
        if (model.ui.dark != theme_dark) {
            theme_dark = model.ui.dark;
            ui.set_theme(theme_dark ? aino::Theme::dark() : aino::Theme::light());
        }
        if (autostart) { autostart = false; bootstrap(initial_search, smoke_test); }
        ui::draw(ui, model);
#ifdef AINO_PLATFORM_X11
        if (diagnostics) {
            const auto stats = static_cast<aino::detail::X11TextRenderer*>(win.impl->text)->cache_stats();
            std::printf("scale=%.3f logical=%dx%d physical=%dx%d fonts=%zu faces=%zu glyph_bytes=%zu\n",
                win.dpi_scale(), win.width(), win.height(), win.impl->client_w, win.impl->client_h,
                stats.fonts, stats.faces, stats.glyph_bytes);
            std::ifstream status("/proc/self/status"); std::string line;
            while (std::getline(status, line))
                if (line.rfind("VmRSS:", 0) == 0 || line.rfind("VmHWM:", 0) == 0 ||
                    line.rfind("RssAnon:", 0) == 0 || line.rfind("RssFile:", 0) == 0) std::printf("%s\n", line.c_str());
        }
#endif
        if (smoke_test) win.close();
        const float elapsed = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - begin).count();
        finish_frame(elapsed);
    });
    shutdown(smoke_test);
    return 0;
}
