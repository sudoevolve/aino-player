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
        if (argc > 1 && std::string(argv[1]) == "--codec-info") {
            std::cout << audio::decoder_info() << std::endl; return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "--child-sleep") { platform::sleep_ms(3000); return 0; }
        if (argc > 3 && std::string(argv[1]) == "--source-response") {
            net::Response response;
            if (std::string(argv[2]) == "youtube") {
                const auto config = music::providers::youtube_config(2000);
                response = net::post("https://music.youtube.com/youtubei/v1/search?prettyPrint=false&key=" + net::url_encode(config.key),
                    "{\"context\":" + music::providers::youtube_context(config) + ",\"query\":\"Hello Adele\"}", music::providers::youtube_headers(config), 6000);
            } else {
                std::string error;
                const auto session = music::providers::bili_session(3000, error);
                check(!session.key.empty(), error.c_str());
                const auto query = music::providers::wbi_query({{"keyword", "Hello Adele"}, {"search_type", "video"}, {"page", "1"}, {"page_size", "3"}}, session.key, std::time(nullptr));
                response = net::get("https://api.bilibili.com/x/web-interface/wbi/search/type?" + query,
                    {"User-Agent: Mozilla/5.0", "Referer: https://www.bilibili.com/", "Cookie: " + session.cookie}, 6000);
            }
            auto* file = platform::fopen(argv[3], "wb"); check(file != nullptr, "diagnostic output file");
            std::fwrite(response.body.data(), 1, response.body.size(), file); std::fclose(file);
            std::cout << "status=" << response.status << " bytes=" << response.body.size() << " error=" << response.error << std::endl;
            return 0;
        }
        if (argc > 2 && std::string(argv[1]) == "--source-probe") {
            std::string error;
            const std::string source = argv[2];
            music::Settings settings;
            const auto tracks = source == "bilibili" ? music::search_bilibili("Hello Adele", 3, 12000, error) :
                source == "kuwo" ? music::search_kuwo("Hello Adele", 3, 12000, error) : music::search_youtube("Hello Adele", 3, 9000, error);
            std::cout << "source=" << source << " count=" << tracks.size() << " error=" << error << std::endl;
            if (tracks.empty()) return 2;
            auto stream = music::resolve(tracks.front(), settings);
            std::cout << "resolve=" << stream.ok << " reason=" << stream.reason << " detail=" << stream.detail << std::endl;
            if (!stream.ok) return 3;
            const auto probe = net::probe_audio(stream.url, stream.headers, 6000);
            std::cout << "probe=" << probe.playable << " bytes=" << probe.total_bytes << " reason=" << probe.reason << std::endl;
            if (!probe.playable) return 4;
            if (argc > 3) {
                std::atomic<bool> cancel{false};
                check(net::download(stream.url, argv[3], stream.headers, [](long long, long long) {}, cancel, error, 30000), error.c_str());
                std::string playable;
                check(audio::prepare_file(argv[3], playable, error, std::string(argv[3]) + ".pcm.wav"), error.c_str());
                const auto info = audio::inspect_mp3(playable);
                check(info.valid && info.duration_ms > 0, "live source downloaded and decoded");
                std::cout << "decoded_ms=" << info.duration_ms << " rate=" << info.sample_rate << std::endl;
            }
            return 0;
        }
        {
        using namespace music::providers;
        const auto child_start = platform::ticks();
        check(platform::run_process({argv[0], "--child-sleep"}, 100) == 124 && platform::ticks() - child_start < 2500,
            "optional source tool timeout terminates and reaps child");
        check(net::audio_prefix("\0\0\0\x20" "ftypisom", 12) && net::audio_prefix("fLaC", 4) &&
            net::audio_prefix("RIFF1234WAVE", 12) && net::audio_prefix("OggS", 4) &&
            net::audio_prefix("\x1a\x45\xdf\xa3", 4) && !net::audio_prefix("<html>login", 11), "audio container signatures reject login pages");
        static_assert(int(music::Source::Netease) == 0 && int(music::Source::QQ) == 1 && int(music::Source::Kuwo) == 2);
        static_assert(int(music::Source::Local) >= music::kSourceCount);
        check(Md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e" && Md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72", "MD5 protocol vectors");
        const auto signed_query = wbi_query({{"foo", "a!b(c)*'"}, {"bar", "空 格"}}, "key", 123);
        check(signed_query.rfind("bar=%E7%A9%BA%20%E6%A0%BC&foo=abc&wts=123&w_rid=", 0) == 0, "WBI sorting encoding and character filtering");
        check(wbi_key("bad", "keys").empty(), "invalid WBI key rejected");
        js::Value fixture;
        check(js::parse(R"({"data":{"result":[{"bvid":"BV1fixture01","title":"<em>Hello</em> &amp; world","author":"Artist","pic":"//cdn.test/a.jpg","duration":"1:02:03"}]}})", fixture), "Bilibili fixture JSON");
        auto videos = parse_bilibili_search(fixture, 2);
        check(videos.size() == 1 && videos[0].title == "Hello & world" && videos[0].duration_ms == 3723000 &&
            videos[0].cover_url == "https://cdn.test/a.jpg", "Bilibili title cover and duration parsing");
        check(js::parse(R"({"data":{"dash":{"audio":[{"baseUrl":"https://cdn.test/low","bandwidth":64000},{"base_url":"https://cdn.test/high","bandwidth":192000}]}}})", fixture), "DASH fixture");
        check(parse_bilibili_audio(fixture, 0).url == "https://cdn.test/low" && parse_bilibili_audio(fixture, 1).url == "https://cdn.test/high", "Bilibili quality selection and URL aliases");
        check(js::parse(R"({"contents":[{"musicResponsiveListItemRenderer":{"playlistItemData":{"videoId":"abcdefghijk"},"flexColumns":[{"musicResponsiveListItemFlexColumnRenderer":{"text":{"runs":[{"text":"Music title"}]}}},{"musicResponsiveListItemFlexColumnRenderer":{"text":{"runs":[{"text":"Artist"},{"text":" • "},{"text":"3:24"}]}}}],"thumbnail":{"musicThumbnailRenderer":{"thumbnail":{"thumbnails":[{"url":"https://cdn.test/music.jpg"}]}}}}},{"musicTwoRowItemRenderer":{"title":{"runs":[{"text":"Other title"}]},"navigationEndpoint":{"watchEndpoint":{"videoId":"0123456789_"}}}},{"videoRenderer":{"videoId":"abcdefghijk","title":{"simpleText":"duplicate"}}}]})", fixture), "YouTube Music fixture");
        auto music_results = parse_youtube_search(fixture, 10);
        check(music_results.size() == 2 && music_results[0].artist == "Artist" && music_results[0].duration_ms == 204000 &&
            music_results[0].cover_url == "https://cdn.test/music.jpg", "Music renderer parsing and deduplication");
        check(parse_youtube_search(fixture, 1).size() == 1, "YouTube search limit");
        check(js::parse(R"({"playabilityStatus":{"status":"OK"},"streamingData":{"adaptiveFormats":[{"mimeType":"video/mp4","url":"https://cdn.test/video","bitrate":900000},{"mimeType":"audio/webm","url":"https://cdn.test/webm","bitrate":160000},{"mimeType":"audio/mp4","url":"https://cdn.test/m4a","bitrate":128000},{"mimeType":"audio/mp4","signatureCipher":"url=untrusted&s=signature","bitrate":256000}]}})", fixture), "YouTube audio fixture");
        check(parse_youtube_audio(fixture, {}, 1).url == "https://cdn.test/m4a", "prefer direct audio MP4 and never mistake cipher or video for audio");
        check(js::parse(R"({"playabilityStatus":{"status":"LOGIN_REQUIRED","reason":"Sign in"}})", fixture) &&
            !parse_youtube_audio(fixture, {}, 1).ok, "YouTube login errors stay explicit");
        const auto page = youtube_page_config(R"(<script>ytcfg.set({"INNERTUBE_CLIENT_VERSION":"new","nested":{"quoted":"}\\\"{"}});</script>)");
        check(page["INNERTUBE_CLIENT_VERSION"].str() == "new", "homepage config handles nested objects and escaped strings");
        }
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
        if (argc > 2 && (std::string(argv[1]) == "--audio-device" || std::string(argv[1]) == "--audio-decode" ||
                         std::string(argv[1]) == "--audio-reject")) {
            std::string playable, error;
            std::string path = argv[2];
            std::string conversion = argc > 3 ? argv[3] : "";
#ifdef _WIN32
            int count = 0;
            auto args = CommandLineToArgvW(GetCommandLineW(), &count);
            check(args && count > 2, "native audio command line");
            path = platform::narrow(args[2]);
            if (count > 3) conversion = platform::narrow(args[3]);
            LocalFree(args);
#endif
            const bool prepared = audio::prepare_file(path, playable, error, conversion);
            if (std::string(argv[1]) == "--audio-reject") {
                check(!prepared && !error.empty(), "corrupt audio must report decode failure");
                check(!platform::exists(conversion + ".part.wav"), "failed conversion cleaned up");
                std::cout << "Invalid audio rejected\n"; return 0;
            }
            check(prepared, error.c_str());
            const auto info = audio::inspect_mp3(playable);
            check(info.valid && info.sample_rate > 0 && info.duration_ms >= 900, "audio decoded with duration");
            if (std::string(argv[1]) == "--audio-decode") {
                std::cout << "Audio decode checks passed\n"; return 0;
            }
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
        for (const auto& relative : {"track-1.mp3", "7/track-2.m4a", "7/track-2.m4a.pcm.wav", "7/track-3.mp3", "8/track-4.flac", "9/track-5.wav", "outside/keep.txt", "library.json",
                                    "7/ffmpeg-0123456789abcdef.exe", "8/ffmpeg-0123456789abcdef.exe.part", "9/ffmpeg-0123456789abcdef.exe"}) fixture(relative);
        const auto cleaned = cache::clear_audio(cache_root, 7, "track-2.", true, [](uint32_t pid) { return pid == 9; });
        check(cleaned.files == 4 && cleaned.errors == 0, "cache cleanup removes legacy, stale audio and abandoned decoder");
        for (const auto& relative : {"7/ffmpeg-0123456789abcdef.exe", "9/ffmpeg-0123456789abcdef.exe"})
            check(platform::exists(platform::join(cache_root, relative)), "cache cleanup preserves live decoder executables");
        check(!cache::codec_file("ffmpeg-unrelated.exe") && !cache::codec_file("ffmpeg-0123456789abcdef.txt"), "codec cleanup recognizes only owned names");
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
        for (const auto& relative : {"7/track-2.m4a", "7/track-2.m4a.pcm.wav", "7/track-6.mp3", "9/track-5.wav", "outside/keep.txt", "library.json",
                                    "7/ffmpeg-0123456789abcdef.exe", "9/ffmpeg-0123456789abcdef.exe", "7", "8", "9", "outside"})
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
