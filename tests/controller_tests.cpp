// 独立编译业务，不创建窗口，不加载 aino，也不请求音乐网站。
#include "../src/business.hpp"
#include "../src/platform.hpp"
#include "../src/login.h"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <type_traits>

static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    try {
        using namespace player;
        static_assert(std::is_const_v<std::remove_reference_t<decltype(ViewModel::library)>>);
        static_assert(std::is_const_v<std::remove_reference_t<decltype(ViewModel::playback)>>);
        check(initialize(false), "initialize isolated business state");
        auto model = view_model();
        check(model.ui.tab == 1 && model.playback.tracks.empty(), "default Discover and empty list");
        static std::atomic<int> wakes{0}, activations{0};
        start({[&] { ++wakes; }, [&] { ++activations; }});
        if (argc > 2 && std::string(argv[1]) == "--cover-stress") {
            const std::string base = argv[2];
            const auto deadline = platform::ticks() + 15000;
            bool ready = false;
            while (!ready && platform::ticks() < deadline) {
                ready = true;
                for (int i = 0; i < 64; ++i) {
                    const auto image = cover_image(base + "/" + std::to_string(i));
                    if (!image) ready = false;
                    else check(image->w > 0 && image->w <= 192 && image->h <= 192, "bounded decoded cover dimensions");
                }
                platform::sleep_ms(10);
            }
            check(ready, "bounded cover workers drain all queued images");
            cover_image(base + "/old");
            g_covers.clear();
            const auto clear_deadline = platform::ticks() + 3000;
            while (!cover_image(base + "/new") && platform::ticks() < clear_deadline) platform::sleep_ms(10);
            check(cover_image(base + "/new") != nullptr, "clear during load accepts a new generation");
            shutdown(true); return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "--update-probe") {
            check_updates(false);
            const auto deadline = platform::ticks() + 20000;
            while (model.update.status == UpdateStatus::Checking && platform::ticks() < deadline) { tick(); platform::sleep_ms(25); }
            std::cout << "{\"version\":" << library::quote(model.update.version)
                      << ",\"asset_url\":" << library::quote(model.update.asset_url)
                      << ",\"message\":" << library::quote(model.update.message) << "}\n";
            const auto status = model.update.status;
            shutdown(true);
            return status == UpdateStatus::Current || status == UpdateStatus::Available || status == UpdateStatus::NoRelease ? 0 : 1;
        }

        std::array<unsigned, 3> version{};
        check(updates::version_parts("v1.2.10", version) && version[2] == 10, "numeric release version parsing");
        for (const auto* bad : {"1.2", "1.2.3.4", "v1.2.3-rc1", "1.2.3;evil", "01.2.3", "1.9999999.0"})
            check(!updates::version_parts(bad, version), "invalid and prerelease versions rejected");
        auto response = [](const char* tag, const char* suffix) {
            net::Response value; value.status = 200;
            const std::string name = std::string("aino-player-") + tag + suffix;
            value.body = "{\"tag_name\":" + library::quote(tag) + ",\"draft\":false,\"prerelease\":false,\"html_url\":" +
                library::quote(std::string(updates::kReleases) + "/tag/" + tag) + ",\"assets\":[{\"name\":" + library::quote(name) +
                ",\"browser_download_url\":" + library::quote(std::string(updates::kReleases) + "/download/" + tag + "/" + name) + "}]}";
            return value;
        };
        for (const auto* suffix : {"-windows-x64.exe", "_amd64.deb"}) {
            auto parsed = updates::parse_release(response("v1.10.0", suffix), "1.9.9", suffix);
            check(parsed.status == UpdateStatus::Available && !parsed.asset_url.empty(), "numeric version and matching platform asset");
            check(updates::parse_release(response("v1.10.0", suffix), "1.10.0", suffix).status == UpdateStatus::Current, "same version is current");
            check(updates::parse_release(response("v1.9.0", suffix), "1.10.0", suffix).status == UpdateStatus::Current, "no downgrade prompt");
        }
        auto malformed = response("v2.0.0", "_amd64.deb");
        malformed.body.replace(malformed.body.find("github.com"), 10, "evil.test");
        check(updates::parse_release(malformed, "1.0.0", "_amd64.deb").status == UpdateStatus::Failed, "foreign release host rejected");
        malformed = response("v2.0.0", "_amd64.deb");
        malformed.body.replace(malformed.body.find("browser_download_url") + 23, 0, "evil");
        const auto untrusted = updates::parse_release(malformed, "1.0.0", "_amd64.deb");
        check(untrusted.asset_url.empty(), "untrusted download URL not opened");
        malformed = response("v2.0.0-rc1", "_amd64.deb");
        check(updates::parse_release(malformed, "1.0.0", "_amd64.deb").status == UpdateStatus::Failed, "prerelease tag excluded");
        for (const auto status : {403, 429, 500}) {
            net::Response failed; failed.status = status;
            check(updates::parse_release(failed, "1.0.0", "").status == UpdateStatus::Failed, "HTTP failures do not claim up-to-date");
        }
        net::Response absent; absent.status = 404;
        check(updates::parse_release(absent, "1.0.0", "").status == UpdateStatus::NoRelease, "no release distinguished from current");
        absent.error = "offline";
        check(updates::parse_release(absent, "1.0.0", "").status == UpdateStatus::Failed, "transport errors reported");

        auto installers = response("v2.0.0", "-windows-x64.exe");
        js::Value installer_json; check(js::parse(installers.body, installer_json), "release fixture JSON");
        const std::string setup_name = "aino-player-v2.0.0-setup-windows-x64.exe";
        const auto setup_url = std::string(updates::kReleases) + "/download/v2.0.0/" + setup_name;
        const auto extra_asset = ",{\"name\":" + library::quote(setup_name) + ",\"browser_download_url\":" + library::quote(setup_url) +
            ",\"size\":123456,\"digest\":\"sha256:" + std::string(64, 'a') + "\"}";
        installers.body.insert(installers.body.rfind(']'), extra_asset);
        const auto setup = updates::parse_release(installers, "1.0.0", "windows-x64.exe");
        check(setup.installer && setup.asset_url == setup_url && setup.asset_size == 123456 && setup.digest.size() == 71,
            "installer preferred to portable with integrity metadata");
        installers.body = response("v2.0.0", "-setup-windows-x64.exe").body;
        const auto portable_json = response("v2.0.0", "-windows-x64.exe").body;
        const auto asset_start = portable_json.find("[{\"") + 1;
        installers.body.insert(installers.body.rfind(']'), "," + portable_json.substr(asset_start, portable_json.rfind(']') - asset_start));
        check(updates::parse_release(installers, "1.0.0", "windows-x64.exe").installer, "installer choice independent of API ordering");

        music::Track nav_a, nav_b, nav_c;
        auto navigation_track = [](const char* name) {
            music::Track value; value.title = name;
            music::Variant variant; variant.source = music::Source::Local; variant.id = name;
            value.variants.push_back(variant); return value;
        };
        nav_a = navigation_track("a"); nav_b = navigation_track("b"); nav_c = navigation_track("c");
        queue_push(nav_a); queue_push(nav_b); queue_push(nav_c);
        auto mark_current = [&](const music::Track& value, int index) {
            std::lock_guard<std::mutex> lock(g_mu); g_as.now_uid = track_uid(value); g_as.playing = index;
        };
        mark_current(nav_a, 0); set_play_mode(PlayMode::RepeatOne);
        check(next_queue_index(1, true) == 0 && next_queue_index(1, false) == 1, "repeat one only affects natural end");
        queue_play_next(nav_c); tick();
        check(model.playback.tracks.size() == 3 && model.playback.playing == 0 &&
              track_uid(model.playback.tracks[1]) == track_uid(nav_c), "play next moves without duplicating or interrupting current");
        check(next_queue_index(1, true) == 1, "explicit next overrides repeat one");
        set_play_mode(PlayMode::Sequential); mark_current(nav_b, 2);
        check(next_queue_index(1, true) == -1, "sequential mode stops at queue end");
        set_play_mode(PlayMode::RepeatAll);
        check(next_queue_index(1, true) == 0, "repeat all wraps at queue end");
        set_play_mode(PlayMode::Shuffle); mark_current(nav_a, 0);
        const int shuffled = next_queue_index(1, true);
        check(shuffled != 0, "shuffle skips current when alternatives exist");
        mark_current(shuffled == 1 ? nav_c : nav_b, shuffled);
        const int shuffled_again = next_queue_index(1, true);
        check(shuffled_again != 0 && shuffled_again != shuffled, "shuffle visits remaining songs without repetition");
        { std::lock_guard<std::mutex> lock(g_mu);
          g_shuffle_history = {track_uid(nav_a), track_uid(nav_c)}; g_shuffle_cursor = 1; }
        mark_current(nav_c, 1);
        check(next_queue_index(-1, false) == 0 && next_queue_index(1, false) == 1, "shuffle previous and forward follow actual playback history");
        queue_play_next(nav_a);
        const int explicit_next = next_queue_index(1, true);
        { std::lock_guard<std::mutex> lock(g_mu);
          check(explicit_next >= 0 && track_uid(g_as.tracks[size_t(explicit_next)]) == track_uid(nav_a), "explicit next overrides shuffle"); }
        set_sleep_timer(15);
        check(model.ui.sleep_remaining_seconds == 900 && model.ui.sleep_deadline && !model.ui.sleep_after_track, "timer preset armed");
        set_sleep_timer(0); check(!model.ui.sleep_deadline && !g_sleep_deadline.load(), "timer cancellation");
        int cancelled_generation;
        { std::lock_guard<std::mutex> lock(g_mu); g_as.ps = PlayState::Resolving; cancelled_generation = g_as.gen; }
        set_sleep_timer(0, true); check(model.ui.sleep_after_track, "stop after track is armed");
        set_sleep_timer(15); model.ui.sleep_deadline = platform::ticks() - 1; tick();
        check(model.playback.ps == PlayState::Idle && model.playback.gen > cancelled_generation &&
              !model.ui.sleep_deadline && !model.ui.sleep_after_track && model.playback.pending_path.empty(),
              "expired timer stops pending playback and invalidates old work");
        queue_clear(); set_play_mode(PlayMode::RepeatAll);
        { std::lock_guard<std::mutex> lock(g_mu); g_as.now_uid.clear(); }
        tick();

        music::Track track;
        music::Track part1, part2;
        music::Variant part_variant; part_variant.source = music::Source::Bilibili; part_variant.id = "BV1fixture01";
        part_variant.native_id = "101"; part1.title = "First"; part1.variants = {part_variant};
        part_variant.native_id = "202"; part2.title = "Second"; part2.variants = {part_variant};
        check(queue_push(part1) && queue_push(part2) && !queue_push(part2), "queue preserves different parts while rejecting exact duplicates");
        check(queue_save(), "multipart queue save"); queue_load(); tick();
        check(model.playback.tracks.size() == 2 && model.playback.tracks[1].current().native_id == "202" &&
            track_uid(model.playback.tracks[0]) != track_uid(model.playback.tracks[1]), "queue reload retains part identity and current-song highlighting");
        queue_clear(); tick();
        track.title = "业务分层回归";
        music::Variant first; first.id = "fixture";
        music::Variant second = first; second.source = music::Source::QQ;
        track.variants = {first, second};
        check(queue_push(track) && !queue_push(track), "queue actions deduplicate");
        choose_source(0, 1, false);
        tick();
        check(model.playback.tracks.size() == 1 && model.playback.tracks[0].chosen == 1, "source selection publishes a new snapshot");
        favorite_toggle(track);
        check(model.library.contains(track), "favorite action updates library");
        model.ui.local_name = "测试快照";
        library_save_current();
        check(model.library.playlists.size() == 2, "save named playlist action");
        library_remove(1);
        check(model.library.playlists.size() == 1 && model.library.contains(track), "remove playlist preserves favorites");

        const auto music_folder = platform::join(model.paths.config, "music 中文");
        platform::make_dirs(platform::join(music_folder, "disc"));
        auto write_fixture = [](const std::string& path, const char* contents) {
            auto* f = platform::fopen(path, "wb"); check(f != nullptr, "create local fixture");
            std::fputs(contents, f); std::fclose(f);
        };
        const auto original = platform::join(music_folder, "歌手 - 歌名.MP3");
        write_fixture(original, "fixture");
        write_fixture(platform::join(music_folder, "disc/other.flac"), "fixture");
        write_fixture(platform::join(music_folder, "ignore.txt"), "fixture");
        std::string scan_error;
        auto scanned = library::scan_folder(music_folder, false, scan_error);
        check(scan_error.empty() && scanned.tracks.size() == 1 && scanned.tracks[0].title == "歌名", "folder scanner filters and parses names");
        check(scanned.tracks[0].current().source == music::Source::Local, "local source separated from network sources");
        write_fixture(platform::join(music_folder, "歌手 - 歌名.lrc"), "\xef\xbb\xbf[00:01.00]本地歌词");
        const auto local_lyrics = lyr::fetch(scanned.tracks[0], model.ui.settings);
        check(local_lyrics.synced && local_lyrics.lines.size() == 1 && local_lyrics.lines[0].ms == 1000, "local BOM LRC is read offline");
        auto cancelled = library::scan_folder(music_folder, true, scan_error, [] { return true; });
        check(!scan_error.empty() && cancelled.tracks.empty(), "scan cancellation");
        library_import_folder(music_folder, true);
        const auto scan_deadline = platform::ticks() + 3000;
        while (model.ui.importing_playlist && platform::ticks() < scan_deadline) { tick(); platform::sleep_ms(10); }
        check(!model.ui.importing_playlist && model.library.playlists.size() == 2, "asynchronous folder import");
        check(model.library.playlists[1].tracks.size() == 2 && !model.library.playlists[1].local_folder.empty(), "recursive folder playlist");
        model.ui.query = "独立的在线搜索词";
        { std::lock_guard<std::mutex> lock(g_mu); g_as.results = {track}; ++g_as.results_version; }
        tick();
        library_open(1); tick();
        check(model.ui.tab == 3 && model.ui.query == "独立的在线搜索词" && model.playlist.open &&
            model.playlist.playlist.tracks.size() == 2, "folder opens a dedicated detail view without changing search text");
        check(model.playback.tracks.size() == 1 && model.playback.results.size() == 1, "browsing playlist preserves queue and search results");
        music::Settings offline_search;
        for (auto& enabled : offline_search.enable) enabled = false;
        const int search_generation = ++g_search_gen;
        do_search(g_win, "fixture", offline_search, search_generation); tick();
        check(model.playback.tracks.size() == 1 && model.playback.results.empty() && model.playlist.playlist.tracks.size() == 2,
            "online search does not overwrite the playing queue or playlist detail");
        playlist_close();
        check(!model.playlist.open && model.ui.tab == 3 && model.ui.query == "独立的在线搜索词", "return from playlist keeps search state");
        queue_clear();
        for (const auto& local : model.library.playlists[1].tracks) queue_push(local);
        tick();
        const auto queued_local_id = model.playback.tracks[0].current().id;
        check(queue_save(), "local queue save");
        queue_load(); tick();
        check(model.playback.tracks[0].current().id == queued_local_id, "local queue path round trip");
        library::Library restored_local;
        scan_error.clear();
        check(restored_local.load(library_path(), scan_error) && restored_local.playlists[1].tracks.size() == 2 &&
            restored_local.playlists[1].recursive, "local playlist persistence");
        library_remove(1);
        check(platform::exists(original), "removing playlist preserves music files");
        music::Track missing = scanned.tracks[0]; missing.variants[0].id += ".missing";
        int local_gen;
        { std::lock_guard<std::mutex> lock(g_mu); local_gen = ++g_as.gen; }
        do_play(g_win, missing, model.ui.settings, local_gen); tick();
        check(model.playback.ps == PlayState::Failed && model.playback.error.find("移动或删除") != std::string::npos,
            "missing local file fails offline without online substitution");
        { std::lock_guard<std::mutex> lock(g_mu); g_as.results = {track}; ++g_as.results_version; }
        tick();
        auto incoming = track; incoming.variants[0].id = "new-search-result";
        { std::lock_guard<std::mutex> lock(g_mu); g_as.results = {incoming, track}; ++g_as.results_version; }
        choose_search_source(0, 1, false);
        {
            std::lock_guard<std::mutex> lock(g_mu);
            check(g_as.results[0].chosen == 0 && g_as.results[1].chosen == 1,
                "source selection follows displayed song identity after background results reorder");
            g_as.results = {missing}; ++g_as.results_version;
        }
        tick();
        auto next_result = missing; next_result.variants[0].id += ".next";
        { std::lock_guard<std::mutex> lock(g_mu); g_as.results = {next_result}; ++g_as.results_version; }
        play_search_result(0);
        {
            std::lock_guard<std::mutex> lock(g_mu);
            check(g_as.tracks.size() == 1 && g_as.tracks[0].current().id == missing.current().id &&
                g_as.results[0].current().id == next_result.current().id,
                "search click queues the displayed song while preserving incoming results");
        }
        { std::lock_guard<std::mutex> lock(g_mu); g_as.results = {track}; ++g_as.results_version; }

        model.ui.volume = 43;
        model.ui.dark = true;
        set_play_mode(PlayMode::Shuffle);
        model.ui.download_dir = platform::join(model.paths.config, "downloads");
        platform::make_dirs(model.ui.download_dir);
        const auto downloaded = platform::join(model.ui.download_dir, "song.mp3");
        auto* file = platform::fopen(downloaded, "wb");
        check(file != nullptr, "download fixture"); std::fputs("keep", file); std::fclose(file);
        model.ui.settings.netease_cookie = "MUSIC_U=fixture";
        model.ui.settings.qq_cookie = "uin=123; qm_keyst=fixture";
        model.ui.qq_uin = "123"; model.ui.ne_uid = "456";
        check(config::save(model.ui), "persist fake credentials");
        auto& login_state = login::state(login::Site::NetEase);
        { std::lock_guard<std::mutex> lock(login_state.m); login_state.cookie = "MUSIC_U=fixture"; }
        login_state.return_requested.store(true);
        tick();
        check(activations.load() == 1, "login return activates host without UI dependency");
        clear_data(ClearAction::Cookies);
        UiState restored;
        config::load(restored);
        check(restored.settings.netease_cookie.empty() && restored.settings.qq_cookie.empty() && restored.qq_uin.empty() && restored.ne_uid.empty(), "logout persists cleared credentials");
        check(restored.volume == 43 && restored.dark && restored.download_dir == model.ui.download_dir, "logout preserves unrelated settings");
        check(restored.play_mode == PlayMode::Shuffle && !restored.sleep_deadline && !restored.sleep_after_track, "play mode persists without restoring a timer");
        check(model.library.contains(track), "logout preserves local library");
        clear_data(ClearAction::Queue);
        tick();
        check(model.playback.tracks.empty() && model.playback.results.size() == 1, "clear queue preserves independent search results");
        check(model.library.contains(track), "clear queue preserves favorites");
        const auto cached = platform::join(model.paths.current_cache, "track-99.mp3");
        file = platform::fopen(cached, "wb");
        check(file != nullptr, "cache fixture"); std::fputs("remove", file); std::fclose(file);
        clear_data(ClearAction::Cache);
        check(!platform::exists(cached) && platform::exists(downloaded) && platform::exists(original), "cache clear preserves downloaded songs");
        clear_data(ClearAction::Library);
        library::Library library;
        std::string error;
        check(library.load(platform::join(model.paths.config, "library.json"), error) && library.playlists.size() == 1 && library.playlists[0].tracks.empty(), "cleared library persists");
        shutdown(false);
        std::cout << "Headless controller, persistence and host callback checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        player::shutdown(true);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
