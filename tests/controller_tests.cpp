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

        music::Track track;
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

        model.ui.volume = 43;
        model.ui.dark = true;
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
        check(model.library.contains(track), "logout preserves local library");
        clear_data(ClearAction::Queue);
        tick();
        check(model.playback.tracks.empty(), "clear queue publishes empty snapshot");
        check(model.library.contains(track), "clear queue preserves favorites");
        const auto cached = platform::join(model.paths.current_cache, "track-99.mp3");
        file = platform::fopen(cached, "wb");
        check(file != nullptr, "cache fixture"); std::fputs("remove", file); std::fclose(file);
        clear_data(ClearAction::Cache);
        check(!platform::exists(cached) && platform::exists(downloaded), "cache clear preserves downloaded songs");
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
