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
        if (argc > 2 && std::string(argv[1]) == "--bilibili-parts-probe") {
            std::string error;
            auto parts = music::bilibili_parts(argv[2], error);
            check(parts.tracks.size() >= 2, error.empty() ? "live video needs at least two parts" : error.c_str());
            std::string previous;
            for (size_t i = 0; i < 2; ++i) {
                const auto& part = parts.tracks[i];
                const auto stream = music::resolve_bilibili(part.current(), {});
                check(stream.ok, stream.reason.c_str());
                const auto probe = net::probe_audio(stream.url, stream.headers, 6000);
                check(probe.playable, probe.reason.c_str());
                check(stream.url != previous, "different selected cids resolve to different audio"); previous = stream.url;
                std::cout << "part=" << i + 1 << " cid=" << part.current().native_id << " duration_ms=" << part.duration_ms
                          << " bytes=" << probe.total_bytes << '\n';
                if (i == 1 && argc > 3) {
                    std::atomic<bool> cancel{false};
                    check(net::download(stream.url, argv[3], stream.headers, [](long long, long long) {}, cancel, error, 30000), error.c_str());
                    std::string playable;
                    check(audio::prepare_file(argv[3], playable, error, std::string(argv[3]) + ".pcm.wav"), error.c_str());
                    const auto info = audio::inspect_mp3(playable);
                    check(info.valid && std::abs(info.duration_ms - part.duration_ms) < 3000, "selected second part decoded at expected duration");
                    std::cout << "decoded_second_part_ms=" << info.duration_ms << '\n';
                }
            }
            return 0;
        }
        if (argc > 3 && std::string(argv[1]) == "--source-response") {
            net::Response response;
            {
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
            const auto tracks = music::search_bilibili("Hello Adele", 3, 12000, error);
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
        static_assert(int(music::Source::Netease) == 0 && int(music::Source::QQ) == 1 &&
            int(music::Source::Bilibili) == 4 && int(music::Source::Local) == 5,
            "source ids keep their historical values so old playlists do not switch platforms");
        static_assert(int(music::Source::Local) < music::kSourceCount, "Local must stay a valid array index");
        static_assert(music::kPlatformCount == 6 && int(music::Source::Kuwo) == 2 && int(music::Source::Kugou) == 6 &&
            int(music::Source::Migu) == 7, "platform IDs remain stable when restoring and extending catalogs");
        check(Md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e" && Md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72", "MD5 protocol vectors");
        const auto signed_query = wbi_query({{"foo", "a!b(c)*'"}, {"bar", "空 格"}}, "key", 123);
        check(signed_query.rfind("bar=%E7%A9%BA%20%E6%A0%BC&foo=abc&wts=123&w_rid=", 0) == 0, "WBI sorting encoding and character filtering");
        check(wbi_key("bad", "keys").empty(), "invalid WBI key rejected");
        js::Value fixture;
        check(js::parse(R"({"code":200,"data":[{"code":200,"url":"https://cdn.test/song.mp3","br":128000,"type":"mp3"}]})", fixture), "NetEase playback fixture");
        auto netease_audio = music::parse_netease_audio(fixture);
        check(netease_audio.ok && netease_audio.url == "https://cdn.test/song.mp3" && netease_audio.detail == "128kbps mp3", "NetEase authenticated playback address");
        check(js::parse(R"({"code":200,"data":[{"code":-110,"url":null,"br":0}]})", fixture), "NetEase refused address fixture");
        netease_audio = music::parse_netease_audio(fixture);
        check(!netease_audio.ok && netease_audio.reason.find("-110") != std::string::npos, "NetEase preserves service refusal instead of guessing membership");
        check(js::parse(R"({"code":301,"data":[]})", fixture) && !music::parse_netease_audio(fixture).ok, "NetEase rejects expired session response");
        check(js::parse(R"({"code":200,"data":[]})", fixture) && !music::parse_netease_audio(fixture).ok, "NetEase rejects missing audio data");
        check(js::parse(R"({"code":200,"data":[{"code":200,"url":"https://cdn.test/song.flac","br":0,"type":"flac"}]})", fixture) && music::parse_netease_audio(fixture).ok, "NetEase accepts address when bitrate metadata is unknown");
        check(js::parse(R"({"code":200,"data":[{"code":200,"url":"file:///untrusted"}]})", fixture) && !music::parse_netease_audio(fixture).ok, "NetEase only accepts network playback addresses");
        check(js::parse(R"({"id":12,"name":"Song","ar":[{"id":1,"name":"AC/DC"},{"id":2,"name":"Guest"}],"al":{"id":22,"name":"Album","picUrl":"https://cdn.test/cover"},"dt":120000})", fixture), "catalog song fixture");
        const auto catalog_song = music::catalog_track(music::Source::Netease, fixture);
        check(catalog_song.current().artists.size() == 2 && catalog_song.current().artists[0].name == "AC/DC" &&
            catalog_song.current().artists[1].id == "2" && catalog_song.current().album_id == "22", "catalog retains exact artist and album identities");
        js::Value catalog_saved; music::Track catalog_restored;
        check(js::parse(library::encode_track(catalog_song), catalog_saved) && library::decode_track(catalog_saved, catalog_restored) &&
            catalog_restored.current().artists[0].id == "1" && catalog_restored.current().album_id == "22", "catalog metadata survives library and queue persistence");
        check(music::track_artists(catalog_restored)[0].name == "AC/DC", "structured artist names are not split on punctuation");
        {
            music::Track track; music::Variant v; v.source = music::Source::Kugou;
            v.id = "audio-id"; v.native_id = "mp3-hash"; v.album_audio_id = "mix-song-id";
            v.title = "Hash fixture"; v.quality_hashes = {{"128k", "mp3-hash"}, {"flac", "flac-hash"}};
            track.variants.push_back(v);
            js::Value saved; music::Track restored;
            check(js::parse(library::encode_track(track), saved) && library::decode_track(saved, restored) &&
                restored.current().album_audio_id == "mix-song-id" && restored.current().quality_hashes.at("flac") == "flac-hash",
                "Kugou quality identity survives library and queue serialization");
            js::Value payload;
            check(js::parse(music::lx_music_info(restored.current(), "kg"), payload) &&
                payload["songmid"].str() == "audio-id" && payload["hash"].str() == "mp3-hash" &&
                payload["albumAudioId"].str() == "mix-song-id" && payload["_types"]["flac"]["hash"].str() == "flac-hash",
                "LX receives distinct Audioid, MixSongID and quality hash");
        }
        check(js::parse(R"({"mid":"songMid","name":"QQ Song","singer":[{"mid":"artistMid","name":"Singer"}],"album":{"mid":"albumMid","name":"Album"},"interval":90})", fixture), "QQ catalog fixture");
        const auto qq_catalog_song = music::catalog_track(music::Source::QQ, fixture);
        check(qq_catalog_song.current().artists[0].id == "artistMid" && qq_catalog_song.current().album_id == "albumMid" &&
            qq_catalog_song.duration_ms == 90000, "QQ catalog keeps mids and duration units");
        check(music::valid_catalog_id("UC_artist-123") && !music::valid_catalog_id("../../artist") &&
            !music::numeric_catalog_id("12&offset=0"), "catalog IDs cannot inject paths or query parameters");
        check(js::parse(R"({"data":{"result":[{"bvid":"BV1fixture01","title":"<em>Hello</em> &amp; world","author":"Artist","pic":"//cdn.test/a.jpg","duration":"1:02:03"}]}})", fixture), "Bilibili fixture JSON");
        auto videos = parse_bilibili_search(fixture, 2);
        check(videos.size() == 1 && videos[0].title == "Hello & world" && videos[0].duration_ms == 3723000 &&
            videos[0].cover_url == "https://cdn.test/a.jpg", "Bilibili title cover and duration parsing");
        check(js::parse(R"({"data":{"dash":{"audio":[{"baseUrl":"https://cdn.test/low","bandwidth":64000},{"base_url":"https://cdn.test/high","bandwidth":192000}]}}})", fixture), "DASH fixture");
        check(parse_bilibili_audio(fixture, 0).url == "https://cdn.test/low" && parse_bilibili_audio(fixture, 1).url == "https://cdn.test/high", "Bilibili quality selection and URL aliases");
        check(js::parse(R"({"code":0,"data":{"title":"Video collection","pic":"https://cdn.test/cover.jpg","owner":{"name":"Author"},"pages":[{"page":1,"cid":101,"part":"First","duration":60},{"page":2,"cid":202,"part":"Second","duration":90},{"page":3,"cid":202,"part":"Duplicate"},{"page":4,"cid":0}]}})", fixture), "multipart fixture");
        std::string parts_error;
        const auto parts = parse_bilibili_parts(fixture, "BV1fixture01", parts_error);
        check(parts_error.empty() && parts.tracks.size() == 2 && parts.tracks[1].duration_ms == 90000 &&
            parts.tracks[1].title == "P2 · Second" && parts.tracks[1].current().native_id == "202", "multipart metadata and cid deduplication");
        check(!library::same_track(parts.tracks[0], parts.tracks[1]) &&
            music::variant_uid(parts.tracks[0].current()) != music::variant_uid(parts.tracks[1].current()), "same BV different cid has independent identity");
        check(bili_playurl(parts.tracks[1].current(), "202").find("&cid=202&") != std::string::npos &&
            bili_playurl(parts.tracks[1].current(), "202&cid=101").empty(), "selected part request uses validated cid");
        library::Library part_library;
        check(part_library.toggle(parts.tracks[0]) && part_library.toggle(parts.tracks[1]) &&
            part_library.playlists.front().tracks.size() == 2, "multipart favorites do not overwrite each other");
        music::Track restored_part; js::Value saved_part;
        check(js::parse(library::encode_track(parts.tracks[1]), saved_part) && library::decode_track(saved_part, restored_part) &&
            music::same_variant(restored_part.current(), parts.tracks[1].current()), "multipart cid persistence");
        auto qq_a = parts.tracks[0].current(), qq_b = qq_a; qq_a.source = qq_b.source = music::Source::QQ; qq_b.native_id = "other";
        check(music::same_variant(qq_a, qq_b), "other sources retain existing identity semantics");
        auto default_part = parts.tracks[0].current(); default_part.native_id.clear();
        check(!music::same_variant(default_part, parts.tracks[1].current()), "legacy default video does not match another part");
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
        if (argc > 2 && std::string(argv[1]) == "--audio-stream") {
            audio::StreamSpec spec; spec.local_path = argv[2]; spec.duration_ms = 1000;
#ifdef _WIN32
            int count = 0; auto args = CommandLineToArgvW(GetCommandLineW(), &count);
            check(args && count > 2, "native stream file argument"); spec.local_path = platform::narrow(args[2]); LocalFree(args);
#endif
            std::string error; std::atomic<bool> cancelled{false};
            for (const int offset : {0, 350}) {
                spec.start_ms = offset;
                auto stream = audio::Stream::open(spec, [&] { return cancelled.load(); }, error);
                check(stream && !stream->failed(), error.c_str());
                const auto began = platform::ticks(); stream->cancel(); stream.reset();
                check(platform::ticks() - began < 2500, "cancel reaps stream with a full or partially full ring");
            }
            cancelled.store(true);
            check(!audio::Stream::open(spec, [&] { return cancelled.load(); }, error) && !error.empty(), "cancelled stream cannot publish media");
            std::cout << "Stream prebuffer, seek rebuild and cancellation checks passed\n"; return 0;
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
        {
            lx::Engine::Health health;
            health.state = 3; health.quality = "320k"; health.checked_revision = 20;
            lx::Engine::Timing timing;
            timing.verified = true; timing.verified_revision = 10; timing.load_ms = 80;
            auto score = lx::Engine::selection_score("320k", &health, &timing, nullptr, 1000);
            check(score.availability == 2, "failed recheck overrides historical source success");
            timing.verified_revision = 21;
            check(lx::Engine::selection_score("320k", &health, &timing, nullptr, 1000).availability == 0,
                "new audio verification recovers a previously failed source");
            timing.failures = 1; timing.failed_at = 900;
            check(lx::Engine::selection_score("320k", nullptr, &timing, nullptr, 1000).availability == 2,
                "one recent playback failure ranks behind healthy sources regardless of old speed");
            check(lx::Engine::selection_score("320k", nullptr, &timing, nullptr, 120000).availability == 2,
                "failed quality does not regain priority when a cooldown expires");
            health.state = 2; health.load_ms = 135;
            score = lx::Engine::selection_score("flac", &health, nullptr, nullptr, 1000);
            check(score.availability == 1 && score.latency_ms == 135 && score.estimated,
                "lower quality success estimates latency without verifying an unmeasured quality");
        }
        library::Library lib;
        {
            check(music::Settings{}.source_smart_switch, "smart routing defaults on");
            const lx::Engine::FixedSource broad{"broad.js", "Broad", 3, 5, 600};
            const lx::Engine::FixedSource single{"single.js", "Single", 1, 1, 50};
            const lx::Engine::FixedSource faster{"fast.js", "Fast", 3, 5, 200};
            check(lx::Engine::fixed_source_better(broad, single), "fixed source prefers verified platform coverage");
            check(lx::Engine::fixed_source_better(faster, broad), "equal coverage prefers lower loading latency");
        }
        {
            prov::Provider provider;
            std::string error;
            const std::string code = "globalThis.provider={key:'pages',name:'Pages',search(){return []},"
                "board(id,page,limit){return {items:[{id:String(page),title:'Song'}],more:page===0,total:2}}}";
            check(provider.load("pages.js", code, error), "paged provider fixture loads");
            std::vector<prov::Song> songs;
            bool more = false; int total = -1;
            check(provider.board("playlist:1", 0, 100, songs, error, &more, &total) && more && total == 2 && songs.size() == 1,
                "provider preserves explicit pagination metadata on a short page");
            check(provider.board("playlist:1", 1, 100, songs, error, &more, &total) && !more && songs.front().id == "1",
                "provider passes subsequent page and explicit end to the caller");
            check(provider.board("playlist:1", 1, 100, songs, error), "legacy board callers accept paged objects");
        }
        check(lib.toggle(track), "favorite add");
        check(lib.contains(track), "favorite membership");
        {
            library::Library copy = lib;
            const auto& original_tracks = static_cast<const std::vector<music::Track>&>(lib.playlists.front().tracks);
            const auto& copied_tracks = static_cast<const std::vector<music::Track>&>(copy.playlists.front().tracks);
            check(original_tracks.data() == copied_tracks.data(), "unchanged library versions share track storage");
            copy.playlists.front().tracks.front().title = "Changed";
            check(lib.playlists.front().tracks.front().title == track.title &&
                copy.playlists.front().tracks.front().title != track.title, "editing a library copy preserves the published version");
            library::FavoriteIndex index(original_tracks);
            music::Track alternative; alternative.variants.push_back(second);
            check(index.contains(alternative), "favorite index matches any platform variant");
            alternative.variants.front().source = music::Source::Bilibili;
            check(!index.contains(alternative), "favorite index preserves platform identity");
        }
        const auto dir = platform::join(platform::temp_dir(), "test-" + std::to_string(platform::ticks()));
        check(platform::make_dirs(dir), "test directory");
        const auto file = platform::join(dir, "资料库.json");
        std::string err;
        lib.remember(track); lib.remember(track);
        lib.lyric_preferences["test-song"] = {1250, "中文歌词.lrc"};
        check(lib.save(file, err), "library save");
        library::Library loaded;
        check(loaded.load(file, err), "library load");
        const auto& saved = loaded.playlists.front().tracks.front();
        check(saved.title == track.title && saved.chosen == 1 && saved.variants.size() == 2, "roundtrip metadata");
        check(loaded.recent.size() == 1 && loaded.recent.front().chosen == 1, "recent history deduplicates and preserves variants");
        check(loaded.lyric_preferences.at("test-song").offset_ms == 1250 &&
            loaded.lyric_preferences.at("test-song").path == "中文歌词.lrc", "lyric preferences persist with history");
        check(!loaded.toggle(track) && loaded.playlists.front().tracks.empty(), "favorite remove");
        library::Playlist playlist;
        playlist.key = "netease#99"; playlist.name = "在线歌单"; playlist.remote_id = "99"; playlist.tracks.push_back(track);
        lib.upsert(playlist); lib.upsert(playlist);
        check(lib.playlists.size() == 2, "playlist deduplication");
        check(lib.save(file, err) && loaded.load(file, err), "remote playlist roundtrip");
        check(loaded.playlists[1].remote_id == "99", "remote id persistence");
        auto* broken = platform::fopen(file, "wb"); std::fputs("{broken", broken); std::fclose(broken);
        check(!loaded.load(file, err) && loaded.playlists.size() == 2, "corrupt file preserves state");
        auto write_library = [&](const char* text) { auto* output = platform::fopen(file, "wb"); check(output != nullptr, "open library fixture"); std::fputs(text, output); std::fclose(output); };
        write_library("{\"version\":1,\"playlists\":[],\"recent\":{}}");
        check(!loaded.load(file, err) && loaded.recent.size() == 1, "invalid optional records preserve prior library");
        write_library("{\"version\":1,\"playlists\":[],\"lyrics\":[{\"uid\":\"large-offset\",\"offset\":1e100,\"path\":\"\"}]}");
        check(loaded.load(file, err) && loaded.lyric_preferences.at("large-offset").offset_ms == 60000, "lyric offset clamps before integer conversion");
        write_library("{\"version\":1,\"playlists\":[]}");
        check(loaded.load(file, err) && loaded.recent.empty() && loaded.lyric_preferences.empty(), "legacy library without optional records remains readable");
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
                                    "7/ffmpeg-0123456789abcdef.exe", "8/ffmpeg-0123456789abcdef.exe.part", "9/ffmpeg-0123456789abcdef.exe",
                                    "7/aino-update-v2.0.0.exe", "8/aino-update-v2.0.0.exe", "9/aino-update-v2.0.0.exe", "8/unrelated.exe"}) fixture(relative);
        const auto cleaned = cache::clear_audio(cache_root, 7, "track-2.", true, [](uint32_t pid) { return pid == 9; });
        check(cleaned.files == 5 && cleaned.errors == 0, "cache cleanup removes legacy, stale audio and abandoned decoder/installer");
        for (const auto& relative : {"7/ffmpeg-0123456789abcdef.exe", "9/ffmpeg-0123456789abcdef.exe"})
            check(platform::exists(platform::join(cache_root, relative)), "cache cleanup preserves live decoder executables");
        check(!cache::codec_file("ffmpeg-unrelated.exe") && !cache::codec_file("ffmpeg-0123456789abcdef.txt"), "codec cleanup recognizes only owned names");
        check(cache::update_file("aino-update-v2.0.0.exe") && cache::update_file("aino-update-v2.0.0.deb") &&
              !cache::update_file("aino-update-v2.0.0.deb.extra") && !cache::update_file("aino-update-v2..0.exe") &&
              !cache::update_file("aino-update-v2.0.0.exe.extra"), "installer cleanup recognizes only owned names");
        for (const auto& relative : {"7/aino-update-v2.0.0.exe", "9/aino-update-v2.0.0.exe", "8/unrelated.exe"})
            check(platform::exists(platform::join(cache_root, relative)), "installer cleanup preserves live instances and unknown files");
        for (const auto& relative : {"7/track-2.m4a", "7/track-2.m4a.pcm.wav", "9/track-5.wav", "outside/keep.txt", "library.json"})
            check(platform::exists(platform::join(cache_root, relative)), "cache cleanup preserves active playback, other processes and unrelated data");
        fixture("7/track-6.mp3");
        cache::clear_audio(cache_root, 7, {}, false, [](uint32_t) { return true; });
        check(platform::exists(platform::join(cache_root, "7/track-6.mp3")), "active writer blocks current cache cleanup");
        check(!cache::audio_file("track-not-a-number.mp3") && !cache::audio_file("track-1.json"), "cache deletion uses owned filenames only");
        check(cache::audio_file("preload-42.m4a") && cache::audio_file("preload-42.pcm.wav") &&
              !cache::audio_file("preload-owned-by-user.mp3"), "preload cache cleanup recognizes only generated filenames");
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
        {
            auto write_metadata = [&](const std::string& name, const std::string& bytes) {
                const auto path = platform::join(dir, name);
                auto* output = platform::fopen(path, "wb"); check(output != nullptr, "metadata fixture file");
                check(std::fwrite(bytes.data(),1,bytes.size(),output) == bytes.size(), "metadata fixture write"); std::fclose(output);
                return path;
            };
            const auto title = tag::id3_frame("TIT2", std::string(1,char(1)) + tag::utf16le_bom("中文标题"));
            const auto artist = tag::id3_frame("TPE1", std::string(1,char(3)) + "Artist");
            const auto image = tag::id3_frame("APIC", std::string(1,char(0)) + std::string("image/png\0\x03\0",12) + "PNG-data");
            const auto size = title.size() + artist.size() + image.size();
            std::string id3("ID3\x03\0\0",6);
            for (int shift : {21,14,7,0}) id3 += char(size >> shift & 127);
            id3 += title + artist + image;
            const auto id3_path = write_metadata("tagged.mp3", id3);
            const auto metadata = tag::read_metadata(id3_path);
            check(metadata.title == "中文标题" && metadata.artist == "Artist" && metadata.cover == "PNG-data", "ID3 text encoding and embedded front cover");
            check(tag::read_metadata(id3_path,false).cover.empty(), "cover extraction can be disabled");
            {
                std::string error;
                auto scanned = library::scan_folder(dir, false, error);
                check(error.empty() && scanned.files.count(id3_path), "incremental scan records file stamp");
                for (auto& track : scanned.tracks) if (track.current().id == id3_path) track.title = "cached metadata";
                auto unchanged = library::scan_folder(dir, false, error, {}, &scanned);
                auto found = std::find_if(unchanged.tracks.begin(), unchanged.tracks.end(), [&](const auto& track) { return track.current().id == id3_path; });
                check(error.empty() && found != unchanged.tracks.end() && found->title == "cached metadata", "unchanged scan reuses metadata");
                write_metadata("tagged.mp3", id3 + "changed-size");
                auto changed = library::scan_folder(dir, false, error, {}, &scanned);
                auto updated = std::find_if(changed.tracks.begin(), changed.tracks.end(), [&](const auto& track) { return track.current().id == id3_path; });
                check(error.empty() && updated != changed.tracks.end() && updated->title == metadata.title, "changed file invalidates cached metadata");
                check(found->title == "cached metadata", "rescan preserves previous snapshot");
            }
            std::string comments;
            auto le = [](std::string& bytes,uint32_t n) { for (int i=0;i<4;++i) bytes += char(n >> (i*8)); };
            le(comments,0); le(comments,2);
            const auto value = std::string("TITLE=") + std::string(300,'x');
            le(comments,uint32_t(value.size())); comments += value;
            le(comments,13); comments += "ARTIST=Artist";
            const auto packet = std::string("OpusTags") + comments;
            auto page = [&](unsigned flags,unsigned sequence,const std::string& sizes,const std::string& payload) {
                std::string header("OggS",4); header += char(0); header += char(flags); header.append(8,'\0');
                le(header,1); le(header,sequence); le(header,0); header += char(sizes.size()); return header + sizes + payload;
            };
            const auto ogg_path = write_metadata("tags.opus",page(2,0,std::string(1,char(255)),packet.substr(0,255)) +
                page(1,1,std::string(1,char(packet.size()-255)),packet.substr(255)));
            const auto ogg = tag::read_metadata(ogg_path);
            check(ogg.title == std::string(300,'x') && ogg.artist == "Artist", "Ogg lacing reconstructs metadata packet across pages");
            const auto truncated = write_metadata("broken.mp3",id3.substr(0,14));
            check(tag::read_metadata(truncated).title.empty(), "truncated metadata is ignored safely");
            for (const auto& path : {id3_path,ogg_path,truncated}) platform::remove(path);
        }
        {
            // 离线脚本夹具；仅发版时执行，不访问真实音源或用户配置目录。
            const auto scripts = platform::join(dir, "script-fixtures");
            check(platform::make_dirs(scripts), "script fixture directory");
            const auto preferences = platform::join(scripts, "removed.json");
            const auto bad = platform::join(scripts, "01-bad.js");
            const auto good = platform::join(scripts, "02-good.js");
            const auto code = [](const char* suffix) {
                return std::string("const lx = globalThis.lx; lx.send('inited', {sources:{wy:{actions:['musicUrl'],qualitys:['128k']}}});") +
                    "lx.on('request', () => Promise.resolve('https://example.invalid/" + suffix + "'));";
            };
            for (const auto& entry : {std::make_pair(bad, code("bad")), std::make_pair(good, code("good"))}) {
                auto* file = platform::fopen(entry.first, "wb"); check(file != nullptr, "script fixture file");
                const bool written = std::fwrite(entry.second.data(), 1, entry.second.size(), file) == entry.second.size();
                const bool closed = std::fclose(file) == 0;
                check(written && closed, "script fixture contents");
            }
            {
                lx::Engine engine(preferences);
                check(engine.load_dir(scripts) == 2, "load isolated script fixtures");
                std::string url, error;
                int candidates = 0;
                const auto fixed = engine.choose_fixed_source(1);
                check(!engine.resolve("wy", "320k", "{}", url, error, nullptr,
                    [&](const auto&, const auto&, const auto&, auto& reason) {
                        ++candidates; reason = "fixture rejects fixed source"; return false;
                    }, nullptr, false) && candidates == 1, "fixed mode never tries another script after failure");
                check(engine.choose_fixed_source(1).file == fixed.file, "playback failure preserves pinned source");
                candidates = 0;
                check(engine.load_dir(scripts) == 2, "reset fixture outcomes before smart routing");
                check(engine.resolve("wy", "320k", "{}", url, error, nullptr,
                    [&](const auto& candidate, const auto&, const auto& quality, auto& reason) {
                        ++candidates;
                        check(quality == "128k", "script advertised quality fallback");
                        if (candidate.find("/bad") != std::string::npos) { reason = "fixture rejects media"; return false; }
                        return true;
                    }) && candidates == 2 && url == "https://example.invalid/good", "rejected media advances to next script");
                check(engine.remove_source(bad, error) && platform::exists(bad), "app removal preserves source file");
                check(engine.entries().size() == 1 && engine.load_dir(scripts) == 1, "reload does not restore removed script");
                check(engine.refresh_files(scripts, {"01-bad.js", "02-good.js"}) == 1 && engine.entries().size() == 1,
                      "ordinary refresh preserves removed script and refreshes active script");
                const auto revision = engine.revision();
                check(engine.refresh_files(scripts, {}) == 0 && engine.revision() == revision,
                      "empty online refresh preserves loaded runtimes");
                lx::Engine restarted(preferences);
                check(restarted.load_dir(scripts) == 1, "removed script survives application restart");
                check(engine.load_files(scripts, {"01-bad.js"}) == 1 && engine.entries().size() == 2, "explicit import or online fetch restores selected script");
                lx::Script script;
                check(script.load(good, code("good"), error) && script.init(error), "standalone script fixture");
                {
                    net::CancelScope cancelled([] { return true; });
                    check(!script.music_url("wy", "128k", "{}", url, error), "cancelled script request returns promptly");
                }
                check(script.music_url("wy", "128k", "{}", url, error), "cancelled request does not poison later calls");
                {
                    lx::Script deferred;
                    const std::string startup = "const lx = globalThis.lx;"
                        "const token = new Promise(resolve=>setTimeout(()=>resolve('https://example.invalid/startup'),10));"
                        "lx.send('inited',{sources:{wy:{actions:['musicUrl'],qualitys:['128k']}}})"
                        ".then(()=>lx.on('request',()=>token));";
                    check(deferred.load("startup-fixture.js", startup, error) && deferred.init(error),
                        "Promise-based send and on finish registering before initialization completes");
                    check(deferred.music_url("wy", "128k", "{}", url, error) && url == "https://example.invalid/startup",
                        "playback preserves initialization tasks needed by its handler");
                    check(deferred.debug_eval("lx.version") == "2.0.0", "LX API version is independent of application version");
                    check(deferred.debug_eval("lx.utils.buffer.bufToString(new Uint8Array([255,128,0]),'base64')") == "/4AA",
                        "binary base64 preserves high bytes without UTF-8 conversion");
                }
                {
                    lx::Script timed;
                    const std::string delayed = "const lx = globalThis.lx; lx.send('inited',{sources:{wy:{actions:['musicUrl'],qualitys:['128k']}}});"
                        "lx.on('request',()=>new Promise((resolve,reject)=>{"
                        "const fail=setTimeout(()=>reject(new Error('timer ran early')),80);"
                        "setTimeout(()=>{clearTimeout(fail);resolve('https://example.invalid/timer');},5); }));";
                    check(timed.load("timer-fixture.js", delayed, error) && timed.init(error) &&
                        timed.music_url("wy", "128k", "{}", url, error) && url == "https://example.invalid/timer",
                        "timers run in due order and cancellation prevents premature script failure");
                }
                std::atomic<bool> concurrent_ok{true};
                auto request = [&] {
                    std::string result, why;
                    for (int i = 0; i < 20; ++i)
                        if (!script.music_url("wy", "128k", "{}", result, why)) concurrent_ok.store(false);
                };
                std::thread first(request), second(request); first.join(); second.join();
                check(concurrent_ok.load(), "concurrent consumers serialize the QuickJS runtime");
                script.retire();
                check(!script.music_url("wy", "128k", "{}", url, error), "removed script rejects new requests");
            }
            for (const auto& path : {bad, good, preferences}) platform::remove(path);
            platform::remove(scripts);
        }
        const auto missing = platform::join(dir, "missing.wav");
        check(!audio::inspect_mp3(missing).valid, "missing audio rejected");
        {
            std::atomic<int> completed{0};
            try {
                background::ThreadGroup workers;
                workers.emplace_back([&] { ++completed; });
                throw std::runtime_error("leave partially constructed group");
            } catch (const std::runtime_error&) {}
            check(completed == 1, "worker group joins during exceptional exit");
            music::Source source; std::string id;
            check(library::parse_link("https://www.kuwo.cn/playlist_detail/12345", source, id) && source == music::Source::Kuwo && id == "playlist:12345", "kuwo playlist URL identity");
            check(library::parse_link("https://www.kugou.com/yy/special/single/12345.html", source, id) && source == music::Source::Kugou, "kugou playlist URL identity");
            check(library::parse_link("https://music.migu.cn/v3/music/playlist/12345", source, id) && source == music::Source::Migu, "migu playlist URL identity");
            check(!library::parse_link("https://www.kuwo.cn.evil.test/playlist_detail/12345", source, id), "playlist official host boundary");
            lx::Engine::Timing cached; cached.load_ms = 42; cached.samples = 1; cached.cached = true;
            const auto score = lx::Engine::selection_score("320k", nullptr, &cached, nullptr, 1000);
            check(score.availability == 1 && score.estimated && score.latency_ms == 42, "restored metrics estimate without claiming audio verification");
            const auto m3u = platform::join(dir, "local.m3u8");
            const auto audio_file = platform::join(dir, "missing track.mp3");
            library::Playlist local; local.tracks.push_back(library::local_track(platform::fs::u8path(audio_file)));
            std::string error;
            check(library::write_m3u(local, m3u, error), "write relative local M3U8");
            auto restored = library::read_m3u(m3u, error);
            check(error.empty() && restored.tracks.size() == 1 && library::local_path_key(platform::fs::u8path(restored.tracks.front().current().id)) == library::local_path_key(platform::fs::absolute(platform::fs::u8path(audio_file))), "relative M3U8 roundtrip");
            auto* unsafe = platform::fopen(m3u, "wb");
            check(unsafe != nullptr, "M3U URL fixture"); std::fputs("#EXTM3U\nhttps://example.test/music.mp3\n", unsafe); std::fclose(unsafe);
            check(library::read_m3u(m3u, error).tracks.empty() && !error.empty(), "M3U import rejects remote audio");
            platform::remove(m3u);
        }
        platform::remove(file); platform::remove(dir);
        std::cout << "All player regression checks passed\n";
        login::shutdown(); return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; login::shutdown(); return 1; }
}
