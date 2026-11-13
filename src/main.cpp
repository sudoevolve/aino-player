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
    if (const int code = aino::system_icon_worker_entry(argc, argv); code >= 0) return code;
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
#ifdef _WIN32
    // UI 线程先建立 STA，避免首帧 WIC 图标解码或音频初始化先选定 MTA，影响 TSF。
    // 此对象晚于窗口及业务音频资源销毁，最后再释放本线程的 COM 初始化引用。
    struct UiApartment {
        HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ~UiApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    } ui_apartment;
    if (FAILED(ui_apartment.result)) {
        std::fprintf(stderr, "Cannot initialize UI COM apartment: 0x%08lx\n", static_cast<unsigned long>(ui_apartment.result));
        return 1;
    }
#endif
#ifdef AINO_PLATFORM_X11
    // 后台任务调用 wake() 前，必须先初始化 Xlib 的线程支持。
    if (!XInitThreads()) { std::fprintf(stderr, "Xlib thread initialization failed\n"); return 1; }
#endif
    auto& startup_ui = view_model().ui;
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
        if (initial_search) startup_ui.query = net::narrow(args[1]);
        LocalFree(args);
    }
#else
    if (initial_search) startup_ui.query = argv[1];
#endif
    if (smoke_test) initial_search = false;
    if (initial_search) startup_ui.tab = 0;
    if (!initialize(smoke_test)) return 1;
#ifdef _WIN32
    SetCurrentProcessExplicitAppUserModelID(L"sudoevolve.AinoPlayer");
#endif
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
    const auto window_handle = static_cast<aino::detail::Win32State*>(win.impl->native)->hwnd;
    for (const auto size : {ICON_BIG, ICON_SMALL}) {
        const int metric = size == ICON_BIG ? SM_CXICON : SM_CXSMICON;
        const auto icon = LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON,
                                    GetSystemMetrics(metric), GetSystemMetrics(metric), LR_SHARED);
        if (icon) SendMessageW(window_handle, WM_SETICON, size, reinterpret_cast<LPARAM>(icon));
    }
#elif defined(AINO_PLATFORM_X11)
    auto* native = static_cast<aino::detail::X11State*>(win.impl->native);
    XClassHint class_hint{const_cast<char*>("aino_player"), const_cast<char*>("aino-player")};
    XSetClassHint(native->dpy, native->win, &class_hint);
    const auto& icon = brand_icon();
    if (icon.w > 0 && icon.h > 0) {
        std::vector<unsigned long> pixels{static_cast<unsigned long>(icon.w), static_cast<unsigned long>(icon.h)};
        for (size_t i = 0; i < icon.rgba.size(); i += 4)
            pixels.push_back((static_cast<unsigned long>(icon.rgba[i + 3]) << 24) |
                (static_cast<unsigned long>(icon.rgba[i]) << 16) | (static_cast<unsigned long>(icon.rgba[i + 1]) << 8) | icon.rgba[i + 2]);
        XChangeProperty(native->dpy, native->win, XInternAtom(native->dpy, "_NET_WM_ICON", False), XA_CARDINAL,
                        32, PropModeReplace, reinterpret_cast<unsigned char*>(pixels.data()), int(pixels.size()));
    }
#endif
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
    bool theme_dark = startup_ui.dark;
    win.set_theme(theme_dark ? aino::Theme::dark() : aino::Theme::light());
    win.set_anim_fps(60.0f);

    start({[&win] { win.wake(); }, [&win] { win.activate(); }, [&win] { win.close(); }, [&win]() -> void* {
#ifdef _WIN32
        return static_cast<aino::detail::Win32State*>(win.impl->native)->hwnd;
#else
        return nullptr;
#endif
    }});
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
            std::ifstream mappings("/proc/self/smaps_rollup");
            while (std::getline(mappings, line))
                if (line.rfind("Pss:", 0) == 0 || line.rfind("Private_Clean:", 0) == 0 ||
                    line.rfind("Private_Dirty:", 0) == 0 || line.rfind("Private_Hugetlb:", 0) == 0)
                    std::printf("%s\n", line.c_str());
        }
#endif
        if (smoke_test) win.close();
        const float elapsed = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - begin).count();
        finish_frame(elapsed);
    });
    shutdown(smoke_test);
    return 0;
}
