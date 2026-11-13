// 仅由内存诊断脚本注入临时构建，不进入普通播放器构建。
#pragma once
#include <chrono>
#include <thread>
namespace memory_probe {
inline std::atomic<bool> finished{false};
inline std::thread ticker;
inline std::string root, base;
inline unsigned long long began = 0, reported = 0;
inline int stage = -1;
inline bool failed = false;
inline const char* phase(int value) {
    static const char* names[] = {"idle", "local_mp3_lyrics", "http_aac_lyrics", "cover_pressure", "settled"};
    return names[std::clamp(value, 0, 4)];
}
inline void initialize(aino::Window& win) {
    root = platform::env("AINO_MEMORY_ROOT"); base = platform::env("AINO_MEMORY_URL");
    if (root.empty() || base.empty()) throw std::runtime_error("missing isolated memory fixtures");
#ifdef _WIN32
    // 诊断构建强制同样的客户区像素，避免 runner 桌面工作区自动缩小窗口。
    const float scale = std::stof(platform::env("AINO_SCALE"));
    win.impl->scale = scale;
    win.impl->max_pw = 4000; win.impl->max_ph = 3000;
    auto* native = static_cast<aino::detail::Win32State*>(win.impl->native);
    RECT outer{0, 0, LONG(1180 * scale), LONG(760 * scale)};
    AdjustWindowRectEx(&outer, DWORD(GetWindowLongPtrW(native->hwnd, GWL_STYLE)), FALSE,
        DWORD(GetWindowLongPtrW(native->hwnd, GWL_EXSTYLE)));
    SetWindowPos(native->hwnd, nullptr, 0, 0, outer.right - outer.left, outer.bottom - outer.top,
        SWP_NOZORDER | SWP_NOACTIVATE);
#endif
    began = platform::ticks();
    // 空闲阶段也按相同频率采样，不依赖鼠标事件触发重绘。
    ticker = std::thread([&win] { while (!finished.load()) { win.wake(); platform::sleep_ms(100); } });
}
inline void play(bool http) {
    using namespace player;
    stop_playback();
    g_ui.lyrics_open = false; g_ui.tab = 0; g_ui.query = "内存诊断：相同歌词与音频";
    music::Track track; track.title = "相同测试曲目"; track.artist = "Aino 内存诊断"; track.duration_ms = 120000;
    music::Variant v; v.source = music::Source::Local; v.id = platform::join(root, http ? "fixture.aac" : "fixture.mp3");
    v.title = track.title; v.artist = track.artist; v.duration_ms = track.duration_ms;
    track.variants.push_back(v);
    { std::lock_guard<std::mutex> lock(g_mu); g_as.tracks = {track}; ++g_as.tracks_version; }
    if (!http) start_play(0);
    else {
        int gen;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            gen = ++g_as.gen; g_play_generation.store(gen); g_as.playing = 0;
            g_as.now_track = track; g_as.now_uid = track_uid(track); g_as.now_title = track.title; g_as.now_artist = track.artist;
            g_as.ps = PlayState::Resolving; g_as.position_ms = 0; g_as.error.clear();
        }
        audio::StreamSpec spec; spec.url = base + "/fixture.aac"; spec.duration_ms = track.duration_ms;
        // 只替换在线地址解析，HTTP、FFmpeg、环形缓冲与播放状态仍走真实实现。
        background::start([spec, v, gen] {
            std::string error;
            auto cancelled = [gen] { return g_shutdown.load() || gen != g_play_generation.load(); };
            auto stream = audio::Stream::open(spec, cancelled, error);
            if (stream) finish_stream(g_win, v, spec, std::move(stream), gen);
            else { std::lock_guard<std::mutex> lock(g_mu); g_as.ps = PlayState::Failed; g_as.error = error; }
        });
    }
    auto lyrics = std::make_shared<lyr::Doc>(); lyrics->origin = "controlled local fixture";
    for (int i = 0; i < 120; ++i) lyrics->lines.push_back({i * 1000LL, "相同歌词与窗口，不修改字号、动画或布局 " + std::to_string(i + 1)});
    { std::lock_guard<std::mutex> lock(g_lyr.m); g_lyr.for_uid = track_uid(track); g_lyr.doc = lyrics; g_lyr.loading = false; }
    g_ui.lyrics_open = true;
}
inline void step(aino::Window& win) {
    using namespace player;
    const auto now = platform::ticks(); const double elapsed = double(now - began) / 1000;
    const int next = elapsed < 4 ? 0 : elapsed < 14 ? 1 : elapsed < 28 ? 2 : elapsed < 44 ? 3 : 4;
    if (next != stage) {
        stage = next;
        if (stage == 1) play(false);
        if (stage == 2) play(true);
        if (stage == 4) stop_playback();
    }
    if (stage == 3) {
        const int current_page = std::min(7, int((elapsed - 28) / 2));
        // 模拟逐页访问 160 张封面；只保留当前页，避免诊断本身持有旧图片。
        for (int i = current_page * 20; i < current_page * 20 + 20; ++i) cover_image(base + "/cover/" + std::to_string(i));
    }
    if (now - reported >= 500) {
        reported = now;
        size_t glyph_bytes = 0; unsigned long buffer_type = 0;
#ifdef AINO_PLATFORM_X11
        glyph_bytes = static_cast<aino::detail::X11TextRenderer*>(win.impl->text)->cache_stats().glyph_bytes;
#elif defined(_WIN32)
        auto* native = static_cast<aino::detail::Win32State*>(win.impl->native);
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(native->bits, &region, sizeof(region))) buffer_type = region.Type;
#endif
        const auto cover_stats = g_covers.memory_profile_stats();
        size_t tasks; { std::lock_guard<std::mutex> lock(background::mutex); tasks = background::tasks.size(); }
        const auto error = library::quote(g_snapshot.error);
        std::printf("AINO_MEMORY {\"phase\":\"%s\",\"seconds\":%.3f,\"position_ms\":%lld,\"playing\":%s,\"state\":%d,\"error\":%s,\"scale\":%.3f,\"physical_w\":%d,\"physical_h\":%d,\"glyph_bytes\":%zu,\"cover_bytes\":%zu,\"cover_entries\":%zu,\"background_tasks\":%zu,\"window_buffer_bytes\":%zu,\"window_buffer_type\":%lu}\n",
            phase(stage), elapsed, g_player.position_ms(), g_player.is_playing() ? "true" : "false", int(g_snapshot.ps), error.c_str(),
            win.dpi_scale(), win.impl->client_w, win.impl->client_h, glyph_bytes, cover_stats.second, cover_stats.first, tasks,
            size_t(win.impl->client_w) * win.impl->client_h * 4, buffer_type);
        std::fflush(stdout);
        if (g_snapshot.ps == PlayState::Failed) failed = true;
    }
    if (elapsed >= 50) win.close();
}
inline void shutdown() { finished.store(true); if (ticker.joinable()) ticker.join(); }
inline void rendered() {
    static bool reported = false;
    if (!reported) { std::puts("AINO_FIRST_FRAME"); std::fflush(stdout); reported = true; }
}

// 同一组字体、字重和全部系统图标逐像素比较；十二秒后引入新字，覆盖冷 face 重开。
inline int assets() {
#ifdef AINO_PLATFORM_X11
    if (!XInitThreads()) return 2;
    const auto dir = platform::env("AINO_ASSET_OUTPUT");
    if (dir.empty() || !platform::make_dirs(dir)) return 2;
    auto* icons = platform::fopen(platform::join(dir, "icons.bin"), "wb");
    if (!icons) return 2;
    int ready = 0;
    for (int size : {16, 24, 32}) for (size_t i = 0; i < size_t(aino::SystemIcon::Count); ++i) {
        const auto mask = aino::detail::load_system_icon(aino::SystemIcon(i), size);
        uint32_t header[]{uint32_t(i), uint32_t(size), uint32_t(mask.width), uint32_t(mask.height)};
        std::fwrite(header, sizeof header, 1, icons);
        if (!mask.alpha.empty()) { std::fwrite(mask.alpha.data(), 1, mask.alpha.size(), icons); ++ready; }
    }
    if (std::fclose(icons) || ready < 60) return 3;
    aino::WindowConfig cfg; cfg.width = 1180; cfg.height = 760; cfg.title = "Aino asset comparison";
    aino::Window win(cfg); if (!win.ok()) return 4;
    win.set_theme(aino::Theme::light());
    const auto begin = platform::ticks();
    bool initial = false, reopened = false;
    win.run([&](aino::Ui& ui) {
        const auto elapsed = platform::ticks() - begin;
        ui.fill_rect({0, 0, float(win.width()), float(win.height())}, aino::Color::white());
        for (int row = 0; row < 4; ++row) {
            const std::string text = elapsed >= 12000 ? "中文预览：独立、唯一与音乐 ABC 123 新增汉字重开字体" :
                "中文预览：独立、唯一与音乐 ABC 123";
            ui.draw_text(text, {30, float(30 + row * 70), 1000, 60}, aino::Color::black(),
                aino::Style().font(float(12 + row * 6)).weight(row % 2 ? 700 : 400));
        }
        for (size_t i = 0; i < size_t(aino::SystemIcon::Count); ++i)
            ui.system_icon(aino::SystemIcon(i), {float(30 + i % 16 * 65), float(340 + i / 16 * 70), 32, 32}, aino::Color::black());
        const auto save = [&](const char* name) {
            auto* output = platform::fopen(platform::join(dir, name), "wb");
            if (!output) { failed = true; return; }
            const auto& canvas = win.impl->canvas;
            if (std::fwrite(canvas.px, 1, size_t(canvas.stride) * canvas.h, output) != size_t(canvas.stride) * canvas.h) failed = true;
            std::fclose(output);
        };
        if (elapsed >= 500 && !initial) { save("initial.rgba"); initial = true; }
        if (elapsed >= 12500 && !reopened) { save("reopened.rgba"); reopened = true; }
        if (elapsed >= 13000) win.close();
        win.wake();
    });
    std::printf("asset icons ready=%d initial=%d reopened=%d\n", ready, int(initial), int(reopened));
    return failed || !initial || !reopened ? 5 : 0;
#else
    return 0;
#endif
}
} // namespace memory_probe
