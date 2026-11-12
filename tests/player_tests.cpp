#include "../src/library.hpp"
#include "../src/login.h"
#include "../src/audio.hpp"
#include "../src/tag.hpp"
#include "../src/cache.hpp"
#include <iostream>
#include <stdexcept>

static void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string(argv[1]) == "--login-probe") {
            const auto site = argc > 2 && std::string(argv[2]) == "qq" ? login::Site::QQ : login::Site::NetEase;
            login::start(site, false);
            const auto deadline = platform::ticks() + 30000;
            std::string url;
            while (url.empty() && platform::ticks() < deadline) {
                { std::lock_guard<std::mutex> lock(login::state(site).m); url = login::state(site).portal_url; }
                platform::sleep_ms(10);
            }
            check(!url.empty(), "login portal startup");
            std::cout << url << std::endl;
            while (!login::state(site).done.load() && platform::ticks() < deadline) platform::sleep_ms(20);
            login::shutdown(); return 0;
        }
        if (argc > 3 && std::string(argv[1]) == "--browser-probe") {
            const auto site = argc > 2 && std::string(argv[2]) == "qq" ? login::Site::QQ : login::Site::NetEase;
            login::BrowserOptions options;
            options.executable = std::string(argv[3]) == "default" ? "" : argv[3];
            options.headless = argc <= 4 || std::string(argv[4]) != "visible";
            options.on_ready = [](const std::string& endpoint) { std::cout << endpoint << std::endl; };
            login::start_browser(site, std::move(options));
            auto& state = login::state(site);
            auto report = [&] {
                std::lock_guard<std::mutex> lock(state.m);
                std::cout << "{\"done\":" << (state.done.load() ? "true" : "false")
                          << ",\"return_requested\":" << (state.return_requested.load() ? "true" : "false")
                          << ",\"cookie\":" << library::quote(state.cookie)
                          << ",\"uid\":" << library::quote(state.uid)
                          << ",\"note\":" << library::quote(state.note) << "}" << std::endl;
            };
            for (std::string command; std::getline(std::cin, command);) {
                if (command == "status") { report(); continue; }
                if (command == "cancel") login::cancel(site);
                if (command == "forget") login::forget(site);
                const auto deadline = platform::ticks() + 10000;
                while (!state.done.load() && platform::ticks() < deadline) platform::sleep_ms(20);
                login::shutdown(); report(); return 0;
            }
            login::shutdown(); return 0;
        }
        if (argc > 2 && std::string(argv[1]) == "--audio-device") {
            std::string playable, error;
            std::string path = argv[2];
#ifdef _WIN32
            int count = 0;
            auto args = CommandLineToArgvW(GetCommandLineW(), &count);
            check(args && count > 2, "native audio command line");
            path = platform::narrow(args[2]); LocalFree(args);
#endif
            const bool prepared = audio::prepare_file(path, playable, error);
            check(prepared, error.c_str());
            const auto info = audio::inspect_mp3(playable);
            check(info.valid && info.sample_rate > 0, "audio decoded");
            audio::Player player;
            const bool opened = player.open(playable);
            check(opened, player.error().c_str());
            check(player.duration_ms() >= 900, "audio duration");
            player.set_volume(35);
            check(player.play(), "audio play");
            platform::sleep_ms(180);
            check(player.is_playing() && player.position_ms() > 50, "audio playback advances");
            player.pause();
            const auto paused = player.position_ms();
            platform::sleep_ms(100);
            check(!player.is_playing() && std::abs(player.position_ms() - paused) < 40, "audio pause");
            player.seek_ms(600); player.resume();
            platform::sleep_ms(180);
            check(player.position_ms() >= 600, "audio seek and resume");
            platform::sleep_ms(700);
            check(player.is_ended(), "audio natural end");
            player.close(); check(!player.is_playing(), "audio close");
            std::cout << "Audio device checks passed\n"; return 0;
        }
        music::Track track;
        track.title = "中文标题\t\"引号\"\n换行"; track.artist = "歌手"; track.duration_ms = 1000;
        music::Variant v; v.id = "123"; v.source = music::Source::Netease; v.title = track.title;
        track.variants.push_back(v);
        auto second = v; second.id = "mid"; second.source = music::Source::QQ;
        track.variants.push_back(second); track.chosen = 1;
        library::Library lib;
        check(lib.toggle(track), "favorite add");
        check(lib.contains(track), "favorite membership");
        const auto dir = platform::join(platform::temp_dir(), "test-" + std::to_string(platform::ticks()));
        check(platform::make_dirs(dir), "test directory");
        const auto file = platform::join(dir, "资料库.json");
        std::string err;
        check(lib.save(file, err), "library save");
        library::Library loaded;
        check(loaded.load(file, err), "library load");
        const auto& saved = loaded.playlists.front().tracks.front();
        check(saved.title == track.title && saved.chosen == 1 && saved.variants.size() == 2, "roundtrip metadata");
        check(!loaded.toggle(track) && loaded.playlists.front().tracks.empty(), "favorite remove");
        library::Playlist playlist;
        playlist.key = "netease#99"; playlist.name = "在线歌单"; playlist.remote_id = "99"; playlist.tracks.push_back(track);
        lib.upsert(playlist); lib.upsert(playlist);
        check(lib.playlists.size() == 2, "playlist deduplication");
        check(lib.save(file, err) && loaded.load(file, err), "remote playlist roundtrip");
        check(loaded.playlists[1].remote_id == "99", "remote id persistence");
        auto* broken = platform::fopen(file, "wb"); std::fputs("{broken", broken); std::fclose(broken);
        check(!loaded.load(file, err) && loaded.playlists.size() == 2, "corrupt file preserves state");
        music::Source source; std::string id;
        check(library::parse_link("https://music.163.com/#/playlist?id=123&userid=456", source, id) && id == "123" && source == music::Source::Netease, "netease url");
        check(library::parse_link("https://y.qq.com/n/ryqq/playlist/123", source, id) && source == music::Source::QQ, "qq url");
        check(library::parse_link("https://i.y.qq.com/n2/m/share/details/taoge.html?id=123&foo=bar", source, id) && id == "123", "qq mobile url");
        check(!library::parse_link("https://music.163.com.evil.test/playlist?id=1", source, id), "host confusion rejected");
        check(!library::parse_link("https://evil.test/playlist?id=1", source, id), "untrusted playlist host");
        check(!library::parse_link("https://music.163.com/playlist?id=0", source, id), "invalid playlist id");
        std::string cookie, uid;
        check(login::parse_credentials(login::Site::NetEase, "{\"cookie\":\"MUSIC_U=test; __csrf=x\"}", cookie, uid, err), "netease credentials");
        check(!login::parse_credentials(login::Site::NetEase, "{\"cookie\":\"foo=bar\"}", cookie, uid, err), "missing netease credentials");
        check(login::parse_credentials(login::Site::QQ, "{\"cookie\":\"uin=o001234; qm_keyst=test\"}", cookie, uid, err) && uid == "1234", "qq credentials");
        check(!login::parse_credentials(login::Site::QQ, "{\"cookie\":\"uin=12\\r\\nInjected: yes; qm_keyst=x\"}", cookie, uid, err), "cookie header injection rejected");
        check(!login::parse_credentials(login::Site::QQ, "not json", cookie, uid, err), "malformed credentials rejected");
        auto& login_state = login::state(login::Site::NetEase);
        { std::lock_guard<std::mutex> lock(login_state.m); login_state.cookie = "MUSIC_U=fixture"; login_state.uid = "123"; }
        login_state.return_requested.store(true);
        login::forget(login::Site::NetEase);
        { std::lock_guard<std::mutex> lock(login_state.m); check(login_state.cookie.empty() && login_state.uid.empty(), "logout clears login credentials"); }
        check(!login_state.return_requested.load(), "logout clears pending return request");
        const auto cache_root = platform::join(dir, "cache");
        for (const auto& child : {"7", "8", "9", "outside"}) check(platform::make_dirs(platform::join(cache_root, child)), "cache fixture directory");
        auto fixture = [&](const std::string& relative) {
            auto* handle = platform::fopen(platform::join(cache_root, relative), "wb");
            check(handle != nullptr, "cache fixture file"); std::fputs("fixture", handle); std::fclose(handle);
        };
        for (const auto& relative : {"track-1.mp3", "7/track-2.m4a", "7/track-2.m4a.pcm.wav", "7/track-3.mp3", "8/track-4.flac", "9/track-5.wav", "outside/keep.txt", "library.json"}) fixture(relative);
        const auto cleaned = cache::clear_audio(cache_root, 7, "track-2.", true, [](uint32_t pid) { return pid == 9; });
        check(cleaned.files == 3 && cleaned.errors == 0, "cache cleanup removes legacy, stale and unused current audio");
        for (const auto& relative : {"7/track-2.m4a", "7/track-2.m4a.pcm.wav", "9/track-5.wav", "outside/keep.txt", "library.json"})
            check(platform::exists(platform::join(cache_root, relative)), "cache cleanup preserves active playback, other processes and unrelated data");
        fixture("7/track-6.mp3");
        cache::clear_audio(cache_root, 7, {}, false, [](uint32_t) { return true; });
        check(platform::exists(platform::join(cache_root, "7/track-6.mp3")), "active writer blocks current cache cleanup");
        check(!cache::audio_file("track-not-a-number.mp3") && !cache::audio_file("track-1.json"), "cache deletion uses owned filenames only");
        std::error_code link_error;
        const auto linked = platform::join(cache_root, "10");
        platform::fs::create_directory_symlink(platform::fs::u8path(platform::join(cache_root, "outside")), platform::fs::u8path(linked), link_error);
        if (!link_error) {
            fixture("outside/track-10.mp3");
            cache::clear_audio(cache_root, 7, "track-2.", false, [](uint32_t pid) { return pid == 9; });
            check(platform::exists(platform::join(cache_root, "outside/keep.txt")), "cache cleanup does not traverse directory links");
            check(platform::exists(platform::join(cache_root, "outside/track-10.mp3")), "linked owned filenames remain untouched");
            platform::remove(platform::join(cache_root, "outside/track-10.mp3"));
            platform::remove(linked);
        }
        for (const auto& relative : {"7/track-2.m4a", "7/track-2.m4a.pcm.wav", "7/track-6.mp3", "9/track-5.wav", "outside/keep.txt", "library.json", "7", "8", "9", "outside"})
            platform::remove(platform::join(cache_root, relative));
        platform::remove(cache_root);
        const auto utf16 = tag::utf16le_bom("中😀");
        check(utf16.size() == 8 && uint8_t(utf16[2]) == 0x2d && uint8_t(utf16[3]) == 0x4e, "UTF16 tags and surrogate pairs");
        const auto missing = platform::join(dir, "missing.wav");
        check(!audio::inspect_mp3(missing).valid, "missing audio rejected");
        platform::remove(file); platform::remove(dir);
        std::cout << "All player regression checks passed\n";
        login::shutdown(); return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; login::shutdown(); return 1; }
}
