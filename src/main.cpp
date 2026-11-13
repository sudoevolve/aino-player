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
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // 崩溃时也能看到进度
    if (argc > 1 && std::string(argv[1]) == "--version") {
        std::printf("aino player %s\n", kVersion); return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--codec-info") {
        std::printf("%s\n", audio::decoder_info().c_str()); return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--ffmpeg-license") {
        std::printf("%s\n", audio::decoder_license().c_str()); return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--search-test") {
        // 检索链路自检：provider + 内置源各报一行
        prov::Host::instance().load_dir(prov::default_dir());
        const std::string kw = argc > 2 ? argv[2] : "周杰伦";
        music::Settings st;
        for (music::Source src : music::kPlatforms) st.enable[int(src)] = true;
        const auto r = music::run_search(kw, st, 5, 12000);
        std::printf("search \"%s\" -> %zu tracks in %d ms\n", kw.c_str(), r.tracks.size(), r.elapsed_ms);
        for (const auto& rep : r.reports) {
            if (!st.enable[int(rep.source)]) continue;
            std::printf("  %-6s ok=%d n=%d %dms %s\n", music::source_name(rep.source), int(rep.ok), rep.count,
                        rep.ms, rep.err.c_str());
        }
        for (size_t i = 0; i < r.tracks.size() && i < 5; ++i)
            std::printf("  · %-30s %-16s [%s]\n", r.tracks[i].title.c_str(), r.tracks[i].artist.c_str(),
                        music::source_name(r.tracks[i].current().source));
        return r.tracks.empty() ? 1 : 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--prov-test") {
        // 渠道自检：列 provider / 音源 → 用一首歌分别试两条取流渠道
        const std::string pdir = argc > 2 ? argv[2] : prov::default_dir();
        const int pn = prov::Host::instance().load_dir(pdir);
        std::printf("provider dir: %s\nloaded %d: ", pdir.c_str(), pn);
        for (const auto& k : prov::Host::instance().keys()) std::printf("%s ", k.c_str());
        std::printf("\n");
        for (const auto& f : prov::Host::instance().failures())
            std::printf("  fail %s: %s\n", f.first.c_str(), f.second.c_str());
        std::printf("lx sources: %d\n", lx::Engine::instance().load_dir(lx::default_dir()));

        if (argc <= 3) return pn > 0 ? 0 : 1;
        // 参数：--prov-test <providers目录> <网易云歌曲ID>
        music::Variant v;
        v.source = music::Source::Netease;
        v.id = argv[3];
        v.title = "test";
        v.artist = "test";
        v.duration_ms = 240000;
        music::Settings st;
        st.quality = 1;

        for (music::Lane lane : {music::Lane::JsSource, music::Lane::Builtin}) {
            music::ResolveResult rr;
            std::string why;
            const bool ok = lane == music::Lane::JsSource ? music::resolve_via_source(v, st.quality, rr, &why)
                                                          : music::resolve_via_builtin(v, st, rr, &why);
            if (ok) std::printf("[%-12s] ok  %-22s %s\n", music::lane_full_name(lane), rr.detail.c_str(), rr.url.c_str());
            else std::printf("[%-12s] 失败 %s\n", music::lane_full_name(lane), why.c_str());
        }
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--lx-test") {
        // 不依赖窗口的自检：加载音源 → 走 aino 自己的 music::resolve()
        const std::string dir = argc > 2 ? argv[2] : lx::default_dir();
        const int loaded = lx::Engine::instance().load_dir(dir);
        std::printf("lx dir: %s\nloaded %d source(s)\n", dir.c_str(), loaded);
        for (const auto& name : lx::Engine::instance().names()) std::printf("  ok  %s\n", name.c_str());
        for (const auto& f : lx::Engine::instance().failures())
            std::printf("  fail %s: %s\n", f.first.c_str(), f.second.c_str());
        if (argc <= 3) return loaded > 0 ? 0 : 1;
        music::Variant v;
        v.source = music::Source::Netease;
        v.id = argv[3];
        v.title = "test";
        v.artist = "test";
        v.duration_ms = 215000;
        music::Settings st;
        st.quality = 1;
        std::printf("resolve wy/%s ...\n", v.id.c_str());
        const auto rr = music::resolve(v, st);
        std::printf("ok=%d reason=%s detail=%s\nurl=%s\n", int(rr.ok), rr.reason.c_str(),
                    rr.detail.c_str(), rr.url.c_str());
        return rr.ok ? 0 : 1;
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
    // provider（搜索脚本）放后台加载：纯本地解析、不联网，不影响首帧。
    // 音源脚本【启动时绝不自动加载】——加载什么时候发生由用户按按钮决定
    //（在线获取 / 导入音源 / 重新加载），不然按钮就没有存在的意义。
    background::start([] { prov::Host::instance().ensure_loaded(); });

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
    platform::Desktop desktop;
    aino::WindowConfig cfg;
    cfg.width = 1180;
    cfg.height = 760;
    cfg.min_width = 980;
    cfg.min_height = 640;
    cfg.title = "aino player";
    cfg.center = true;
    cfg.close_requested = [&] {
        if (smoke_test || startup_ui.close_behavior == 2) return true;
        if (startup_ui.close_behavior == 1 && startup_ui.tray_enabled && desktop.tray_available()) {
            desktop.hide(); startup_ui.window_hidden = desktop.hidden(); return !desktop.hidden();
        }
        startup_ui.close_dialog = true; startup_ui.close_remember = false;
        if (g_host.wake) g_host.wake();
        return false;
    };
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
    if (!smoke_test) {
#ifdef _WIN32
        desktop.start(window_handle, nullptr, 0, [&win] { win.wake(); });
#else
        desktop.start(nullptr, native->dpy, native->win, [&win] { win.wake(); });
#endif
        const auto& image = brand_icon(); desktop.set_icon(image.w, image.h, image.rgba);
    }
    bool theme_dark = startup_ui.dark;
    win.set_theme(theme_dark ? aino::Theme::dark() : aino::Theme::light());
    win.set_anim_fps(60.0f);

    start({[&win] { win.wake(); }, [&win, &desktop] { desktop.show(); win.activate(); }, [&win] { win.close(); }, [&win]() -> void* {
#ifdef _WIN32
        return static_cast<aino::detail::Win32State*>(win.impl->native)->hwnd;
#else
        return nullptr;
#endif
    }, [&desktop] { desktop.hide(); return desktop.hidden(); }});
    bool autostart = true;
    win.run([&](aino::Ui& ui) {
        const auto begin = std::chrono::steady_clock::now();
        for (const auto& event : desktop.events()) {
            if (event.action == platform::MediaAction::Show) { desktop.show(); win.activate(); }
            else if (event.action == platform::MediaAction::Quit) win.close();
            else media_action(event);
        }
        startup_ui.window_hidden = desktop.hidden();
        startup_ui.media_available = desktop.media_available(); startup_ui.tray_available = desktop.tray_available();
        tick();
        auto model = view_model();
        if (model.ui.dark != theme_dark) {
            theme_dark = model.ui.dark;
            ui.set_theme(theme_dark ? aino::Theme::dark() : aino::Theme::light());
        }
        if (autostart) { autostart = false; bootstrap(initial_search, smoke_test); }
        if (!smoke_test) {
            const auto& playing = model.playback;
            platform::MediaState state;
            state.uid = playing.now_uid; state.title = playing.now_title; state.artist = playing.now_artist;
            state.album = playing.now_track.album; state.artwork = playing.now_cover;
            state.position_ms = static_cast<long long>(model.ui.pos_ms); state.duration_ms = static_cast<long long>(model.ui.dur_ms);
            state.playing = playing.ps == PlayState::Playing; state.paused = playing.ps == PlayState::Paused;
            state.volume = int(model.ui.volume); state.shuffle = model.ui.play_mode == PlayMode::Shuffle;
            state.loop = model.ui.play_mode == PlayMode::RepeatOne ? 2 : model.ui.play_mode == PlayMode::RepeatAll ? 1 : 0;
            state.can_seek = !state.uid.empty() && state.duration_ms > 0;
            desktop.update(state, model.ui.tray_enabled);
        }
        if (!desktop.hidden()) ui::draw(ui, model);
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
    desktop.stop();
    shutdown(smoke_test);
    return 0;
}
