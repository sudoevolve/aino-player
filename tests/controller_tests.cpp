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

// 不访问音乐网站，覆盖平台编码差异、译文对齐与同曲匹配的拒绝条件。
static void lyric_checks() {
    const std::string original = "[00:01.00]First line (stay)\n[00:02.00]Another line\n[00:03.00]Last line";
    const std::string translation = "[00:01.03]第一行\n[00:02.00]另一行\n[00:03.00]最后一行";
    auto doc = lyr::qq_lyric_document("MusicJsonCallback({\"code\":0,\"lyric\":" + library::quote(original) +
        ",\"trans\":" + library::quote(lx::detail::base64_encode(translation)) + "})");
    check(doc.lines.size() == 3 && doc.lines[0].translation == "第一行", "QQ JSONP decodes original and independent Base64 translation");
    doc = lyr::qq_lyric_document("{\"code\":0,\"lyric\":" + library::quote(lx::detail::base64_encode("\xef\xbb\xbf" + original)) +
        ",\"trans\":" + library::quote(translation) + "}");
    check(doc.lines.size() == 3 && doc.lines[2].text == "Last line" && doc.lines[2].translation == "最后一行",
        "QQ Base64 original with BOM supports plaintext translations");
    check(lyr::qq_lyric_document("{\"code\":1101,\"lyric\":\"wrong\"}").empty(), "QQ business error is not displayed as lyrics");
    auto native = lyr::from_lrc(original, "酷狗");
    check(lyr::needs_translation(native) && !lyr::needs_translation(doc), "foreign lyrics request missing translation only");
    check(!lyr::needs_translation(lyr::from_lrc("[00:01]中文歌词\n[00:02]没有翻译\n[00:03]也能显示", "酷我")),
        "Chinese originals do not trigger translation searches");
    auto shifted = doc;
    for (auto& line : shifted.lines) line.ms += 600;
    check(lyr::supplement_translation(native, shifted) && native.lines[0].ms == 1000 && native.lines[0].translation == "第一行",
        "supplement aligns exact text while retaining native timestamps");
    auto wrong = lyr::from_lrc(original, "咪咕");
    auto different = shifted;
    different.lines[0].text = "First line (leave)";
    check(!lyr::supplement_translation(wrong, different) && !lyr::has_translation(wrong),
        "bracketed lyric content cannot be discarded to force matches");
    different = shifted;
    for (auto& line : different.lines) line.ms += 10000;
    check(!lyr::supplement_translation(wrong, different), "unrelated timelines cannot attach translated lines");
    auto timed = lyr::from_lrc("[00:01]one\n[00:02]two", "test");
    lyr::add_translation(timed, "\xef\xbb\xbf[00:01.04]一\n[00:02.20]二");
    check(timed.lines[0].translation == "一" && timed.lines[1].translation.empty(), "same-platform alignment tolerates rounding, not line shifts");
    check(lyr::to_lrc(doc).find("[00:01.00]第一行") != std::string::npos, "LRC exports preserve translation timestamps");
    {
        net::CancelScope cancel([] { return true; });
        music::Track track; track.title = "cancelled";
        check(lyr::fetch(track, {}).error == "歌词加载已取消", "cancelled fetch cannot search external platforms");
    }
    const auto providers = platform::fs::path(__FILE__).parent_path().parent_path() / "providers";
    auto provider_lyric = [&](const char* key, const std::string& mock, const std::string& args) {
        std::ifstream file(providers / (std::string(key) + ".js"), std::ios::binary);
        const std::string code((std::istreambuf_iterator<char>(file)), {});
        check(!code.empty(), "bundled provider fixture exists");
        prov::Provider provider; std::string error; js::Value result;
        if (!provider.load(key, mock + "\n" + code, error)) throw std::runtime_error(std::string(key) + ": " + error);
        if (!provider.request("lyric", args, result, error)) throw std::runtime_error(std::string(key) + ": " + error);
        return result;
    };
    const auto kuwo = provider_lyric("kuwo", R"JS(ctx.http = function(url) {
        if (!url.includes('musicId=440613')) throw Error('wrong Kuwo song');
        return {statusCode:200,body:{data:{lrclist:[{time:'1.234',lineLyric:'中文原文'}]}}};
    };)JS", R"({"id":"440613"})");
    check(lyr::from_lrc(kuwo["lyric"].str(), "酷我").lines[0].ms == 1234, "Kuwo timestamps preserve millisecond precision");
    const auto kugou = provider_lyric("kugou", R"JS(ctx.http = function(url) {
        if (url.includes('/search?')) return {statusCode:200,body:{candidates:[{id:'bad',duration:120000},{id:'match',accesskey:'key',duration:240000}]}};
        if (!url.includes('id=match') || !url.includes('fmt=lrc')) throw Error('wrong Kugou candidate');
        return {statusCode:200,body:{fmt:'lrc',content:ctx.b64encode('[00:01]酷狗原文')}};
    };)JS", R"({"native_id":"da60f11f69affc7107adede9dd17352a","duration_ms":240000,"title":"test"})");
    check(kugou["lyric"].str() == "[00:01]酷狗原文", "Kugou selects matching duration and decodes LRC");
    const auto migu = provider_lyric("migu", R"JS(ctx.http = function(url,opts) {
        if (url.includes('/resourceinfo.do')) {
            if (opts.method !== 'POST' || opts.body !== 'resourceId=1139116209') throw Error('wrong Migu ID');
            return {statusCode:200,body:{resource:[{songId:'other',copyrightId:'other',lrcUrl:'https://bad.test/'},{songId:'1139116209',copyrightId:'600ABC',lrcUrl:'https://test/lrc',trcUrl:'https://test/trc',mrcUrl:'https://test/mrc'}]}};
        }
        if (url.endsWith('/mrc')) return {statusCode:200,bodyText:'06045c357454a5644d9424fc99d16df07f689a9af7680331cda53eecf2f6e29661b0d9448f011d53dce7d817b32dd38a0ca4e3b519ac61a240ccc84a05df48ba'};
        return {statusCode:200,bodyText:url.endsWith('/trc')?'[00:01]中文译文':'[00:01]Original'};
    };)JS", R"({"id":"1139116209","native_id":"600ABC"})");
    check(migu["lyric"].str() == "[00:01.000]Original" && migu["translation"].str() == "[00:01]中文译文",
        "Migu selects exact ID, restores MRC timeline and retrieves translation");
}

static void playlist_completion_checks() {
    std::string detail = R"({"code":200,"playlist":{"trackCount":205,"tracks":[{"id":205,"name":"last","ar":[{"name":"Artist"}]}],"trackIds":[)";
    for (int id = 1; id <= 205; ++id) { if (id > 1) detail += ','; detail += "{\"id\":" + std::to_string(id) + "}"; }
    detail += "]}}";
    int calls = 0; bool fail = false;
    auto request = [&](const std::string& url, const std::vector<std::string>&, int) {
        net::Response r; r.status = 200; ++calls;
        if (url.find("/playlist/detail?") != std::string::npos) { r.body = detail; return r; }
        if (fail) { r.status = 500; return r; }
        const auto start = url.find("ids="); check(start != std::string::npos, "completion calls song details");
        std::string decoded;
        for (size_t i = start + 4; i < url.size(); ++i) {
            if (url[i] == '%' && i + 2 < url.size()) { decoded += char(std::stoul(url.substr(i + 1, 2), nullptr, 16)); i += 2; }
            else decoded += url[i];
        }
        js::Value ids; check(js::parse(decoded, ids), "completion sends JSON ID array");
        check(ids.array.size() <= 100, "completion batches are bounded");
        r.body = "{\"code\":200,\"songs\":[";
        for (size_t i = ids.array.size(); i-- > 0;) {
            if (i + 1 != ids.array.size()) r.body += ',';
            r.body += "{\"id\":" + std::to_string(ids[i].integer()) + R"(,"name":"song","artists":[{"name":"Artist"}],"album":{"name":"Album"},"duration":180000})";
        }
        r.body += "]}"; return r;
    };
    std::string error;
    auto songs = ne::fetch_playlist({}, "123", error, 1000, nullptr, request);
    check(error.empty() && songs.size() == 205 && calls == 4, "preview is completed using missing IDs across bounded batches");
    check(songs.front().current().id == "1" && songs.back().current().id == "205" && songs.front().album == "Album" && songs.front().duration_ms == 180000,
        "unordered legacy details retain playlist order and metadata");
    fail = true;
    check(ne::fetch_playlist({}, "123", error, 1000, nullptr, request).empty() && !error.empty(), "failed completion cannot overwrite saved playlists with previews");
}

int main(int argc, char** argv) {
    try {
        lyric_checks();
        playlist_completion_checks();
        using namespace player;
        static_assert(std::is_const_v<std::remove_reference_t<decltype(ViewModel::library)>>);
        static_assert(std::is_const_v<std::remove_reference_t<decltype(ViewModel::playback)>>);
        check(initialize(false), "initialize isolated business state");
        auto model = view_model();
        auto wait_library = [&] {
            const auto deadline = platform::ticks() + 4000;
            for (;;) {
                tick();
                bool busy;
                { std::lock_guard<std::mutex> lock(g_library_write_mutex); busy = g_library_writer || !g_library_edits.empty(); }
                if (!busy) { tick(); return; }
                check(platform::ticks() < deadline, "library writer finishes accepted edits");
                platform::sleep_ms(5);
            }
        };
        check(model.ui.tab == 1 && model.playback.tracks.empty(), "default Discover and empty list");
#if defined(_WIN32) || defined(__linux__)
        const auto hash_path = platform::join(g_temp_dir, "update hash fixture.bin");
        auto* hash_file = platform::fopen(hash_path, "wb"); check(hash_file != nullptr, "hash fixture file");
        std::fwrite("abc", 1, 3, hash_file); std::fclose(hash_file);
        check(platform::sha256_file(hash_path) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "update integrity uses the known SHA-256 of abc");
        hash_file = platform::fopen(hash_path, "wb"); check(hash_file != nullptr, "empty hash fixture"); std::fclose(hash_file);
        check(platform::sha256_file(hash_path) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "update integrity accepts an empty file digest");
        platform::remove(hash_path);
        check(platform::sha256_file(hash_path).empty(), "missing update file never yields a valid digest");
#endif
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
            check(g_covers.stats().pixel_bytes <= cover::Store::pixel_budget, "cover cache stays within pixel budget");
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
            const std::string version = tag[0] == 'v' ? tag + 1 : tag;
            const std::string name = std::string(suffix) == "_amd64.deb" ? "aino-player_" + version + suffix :
                std::string("aino-player-") + tag + suffix;
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
        auto with_notes = response("v2.0.0", "_amd64.deb");
        const std::string notes = "## 更新内容\n\n- **修复** 中文与流式播放\n[发布页](https://github.com/sudoevolve/aino-player)";
        with_notes.body.insert(1, "\"body\":" + library::quote(notes) + ",");
        check(updates::parse_release(with_notes, "1.0.0", "_amd64.deb").release_notes == notes &&
            updates::parse_release(with_notes, "2.0.0", "_amd64.deb").release_notes == notes,
            "release body preserves Unicode and Markdown for new and installed versions");
        check(updates::parse_release(response("v2.0.0", "_amd64.deb"), "1.0.0", "_amd64.deb").release_notes.empty(),
            "missing release notes leave updates available");
        auto long_notes = response("v2.0.0", "_amd64.deb");
        long_notes.body.insert(1, "\"body\":" + library::quote(std::string(65535, 'a') + "中文") + ",");
        const auto bounded_notes = updates::parse_release(long_notes, "1.0.0", "_amd64.deb").release_notes;
        check(bounded_notes.substr(0, 65535) == std::string(65535, 'a') && bounded_notes[65535] == '\n' &&
            bounded_notes.size() < 66000, "long release notes are bounded without splitting UTF-8 characters");
        auto deb = response("v2.0.0", "_amd64.deb");
        deb.body.insert(deb.body.rfind("}]"), ",\"size\":123456,\"digest\":\"sha256:" + std::string(64, 'a') + "\"");
        const auto deb_update = updates::parse_release(deb, "1.0.0", "_amd64.deb");
        check(deb_update.installer && deb_update.asset_url.find("aino-player_2.0.0_amd64.deb") != std::string::npos &&
            deb_update.asset_size == 123456 && deb_update.digest.size() == 71, "Debian update uses exact package name and integrity metadata");
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

        {
            const auto old_tracks = g_as.tracks, old_results = g_as.results;
            const int old_tracks_version = g_as.tracks_version, old_results_version = g_as.results_version;
            music::Track item; item.title = "unchanged";
            g_as.tracks = {item}; g_as.results = {item};
            ++g_as.tracks_version; ++g_as.results_version;
            Snapshot cached; refresh_snapshot(cached);
            const auto* queue_data = cached.tracks.data();
            const auto* result_data = cached.results.data();
            g_as.tracks[0].title = g_as.results[0].title = "changed";
            refresh_snapshot(cached);
            check(cached.tracks.data() == queue_data && cached.results.data() == result_data &&
                  cached.tracks[0].title == "unchanged" && cached.results[0].title == "unchanged",
                  "unchanged playback revisions retain vector storage and contents");
            ++g_as.tracks_version; ++g_as.results_version; refresh_snapshot(cached);
            check(cached.tracks[0].title == "changed" && cached.results[0].title == "changed",
                  "changed playback revisions publish updated queue and search results");
            g_as.tracks = old_tracks; g_as.results = old_results;
            g_as.tracks_version = old_tracks_version; g_as.results_version = old_results_version;
        }
        {
            { std::lock_guard<std::mutex> lock(g_qq.m); g_qq.playlists.resize(1); g_qq.playlists[0].name = "first"; ++g_qq.playlists_revision; }
            qq_snapshot();
            { std::lock_guard<std::mutex> lock(g_qq.m); g_qq.playlists[0].name = "changed"; }
            qq_snapshot(); check(g_qq_snap.playlists[0].name == "first", "QQ snapshot does not copy unchanged revision");
            { std::lock_guard<std::mutex> lock(g_qq.m); ++g_qq.playlists_revision; }
            qq_snapshot(); check(g_qq_snap.playlists[0].name == "changed", "QQ revision publishes changed playlists");
            { std::lock_guard<std::mutex> lock(g_qq.m); g_qq.playlists.clear(); ++g_qq.playlists_revision; }
            { std::lock_guard<std::mutex> lock(g_ne.m); g_ne.playlists.resize(1); g_ne.playlists[0].name = "first"; ++g_ne.playlists_revision; }
            ne_snapshot();
            { std::lock_guard<std::mutex> lock(g_ne.m); g_ne.playlists[0].name = "changed"; }
            ne_snapshot(); check(g_ne_snap.playlists[0].name == "first", "NetEase snapshot does not copy unchanged revision");
            { std::lock_guard<std::mutex> lock(g_ne.m); ++g_ne.playlists_revision; }
            ne_snapshot(); check(g_ne_snap.playlists[0].name == "changed", "NetEase revision publishes changed playlists");
            { std::lock_guard<std::mutex> lock(g_ne.m); g_ne.playlists.clear(); ++g_ne.playlists_revision; }
            g_snapshot.ps = PlayState::Playing; finish_frame(1);
            check(g_fps_hint.load() == 10, "ordinary playback uses lower refresh cadence");
            model.ui.lyrics_open = true; finish_frame(1);
            check(g_fps_hint.load() == 10, "visible lyrics use low playback cadence; scrolling requests its own animation frames");
            model.ui.window_hidden = true; model.ui.lyric_overlay.enabled = true; finish_frame(1);
            check(g_fps_hint.load() == 10, "floating lyrics retain playback synchronization with the main window hidden");
            model.ui.lyric_overlay.enabled = false; finish_frame(1);
            check(g_fps_hint.load() == 1, "hidden player without floating lyrics returns to low refresh cadence");
            model.ui.window_hidden = false;
            model.ui.lyrics_open = false; g_snapshot.ps = PlayState::Idle; finish_frame(1);
        }
        {
            const auto saved_snapshot = g_snapshot;
            const auto saved_view = g_lyric_view;
            const auto saved_settings = model.ui.lyric_overlay;
            const auto saved_position = model.ui.pos_ms;
            auto doc = std::make_shared<lyr::Doc>(lyr::from_lrc("[00:01]First\n[00:03]Next", "fixture"));
            doc->lines[0].translation = "译文";
            g_snapshot.now_uid = "floating-fixture"; g_snapshot.now_title = "Title"; g_snapshot.now_artist = "Artist";
            g_lyric_view = {doc, "floating-fixture", false}; model.ui.pos_ms = 1500;
            model.ui.lyric_overlay.enabled = true; model.ui.lyric_overlay.double_line = true; model.ui.lyric_overlay.translation = true;
            check(floating_lyric_text() == std::make_pair(std::string("First"), std::string("译文")), "floating second line prefers translation");
            auto fill = floating_lyric_progress();
            check(std::abs(fill.first - 0.25f) < 0.001f && fill.second == fill.first, "floating lyrics and translation share line progress");
            model.ui.lyric_overlay.translation = false;
            check(floating_lyric_text().second == "Next", "floating second line can preview next lyric");
            check(floating_lyric_progress().second < 0, "next lyric preview is not marked as already sung");
            model.ui.lyric_overlay.karaoke = false;
            check(floating_lyric_progress().first < 0, "disabled karaoke does not request progress redraws");
            model.ui.lyric_overlay.karaoke = true;
            model.ui.lyric_overlay.double_line = false;
            check(floating_lyric_text().second.empty(), "single-line floating mode omits second lyric");
            g_lyric_view.uid = "previous-song";
            check(floating_lyric_text().first == "Title", "floating window never shows lyrics belonging to the previous track");
            check(floating_lyric_progress().first < 0, "old-song timing cannot color the current title");
            model.ui.lyric_overlay.enabled = false;
            check(floating_lyric_text().first.empty(), "disabled floating lyrics do not prepare text");
            g_snapshot = saved_snapshot; g_lyric_view = saved_view; model.ui.lyric_overlay = saved_settings; model.ui.pos_ms = saved_position;
        }
        music::Track nav_a, nav_b, nav_c;
        auto navigation_track = [](const char* name) {
            music::Track value; value.title = name;
            music::Variant variant; variant.source = music::Source::Local; variant.id = name;
            value.variants.push_back(variant); return value;
        };
        nav_a = navigation_track("a"); nav_b = navigation_track("b"); nav_c = navigation_track("c");
        auto local_a = navigation_track("/music/one/a.mp3");
        local_a.artist = "Artist"; local_a.album = "Same title";
        local_a.variants[0].artist = local_a.artist; local_a.variants[0].album = local_a.album;
        auto local_b = local_a; local_b.variants[0].id = "/music/two/b.mp3";
        library::Library catalog_library;
        catalog_library.playlists.front().tracks = {local_a, local_b, local_a};
        music::CatalogTarget local_artist; local_artist.source = music::Source::Local;
        local_artist.kind = music::CatalogKind::Artist; local_artist.name = "Artist";
        const auto local_songs = local_catalog(local_artist, catalog_library, {}, {}, false);
        const auto local_albums = local_catalog(local_artist, catalog_library, {}, {}, true);
        check(local_songs.tracks.size() == 2 && local_albums.albums.size() == 2, "local artist browsing deduplicates songs and keeps same-name albums in distinct folders");
        const auto local_album = local_catalog(local_albums.albums.front(), catalog_library, {}, {}, false);
        check(local_album.tracks.size() == 1, "local album browsing is scoped to its folder");
        g_ui.tab = 0; g_playlist_view = {}; g_detail_history.clear();
        detail_begin(); g_playlist_view.playlist.name = "First page";
        g_ui.playlist_filter = "saved filter";
        detail_begin(); g_playlist_view.playlist.name = "Second page";
        playlist_close();
        check(g_playlist_view.playlist.name == "First page" && g_ui.playlist_filter == "saved filter", "detail back restores its page and filter");
        playlist_close(); check(!g_playlist_view.open && g_ui.tab == 0, "detail back returns to original search page");
        queue_push(nav_a); queue_push(nav_b); queue_push(nav_c);
        auto mark_current = [&](const music::Track& value, int index) {
            std::lock_guard<std::mutex> lock(g_mu); g_as.now_uid = track_uid(value); g_as.playing = index;
        };
        mark_current(nav_a, 0); set_play_mode(PlayMode::RepeatOne);
        check(next_queue_index(1, true) == 0 && next_queue_index(1, false) == 1, "repeat one only affects natural end");
        queue_play_next(nav_c); tick();
        check(model.playback.tracks.size() == 3 && model.playback.playing == 0 &&
              track_uid(model.playback.tracks[1]) == track_uid(nav_c), "play next moves without duplicating or interrupting current");
        check(next_queue_index(1, true, false) == 1 && next_queue_index(1, true, false) == 1 &&
              next_queue_index(1, true) == 1, "preload peek preserves explicit next and overrides repeat one");
        set_play_mode(PlayMode::Sequential); mark_current(nav_b, 2);
        check(next_queue_index(1, true) == -1, "sequential mode stops at queue end");
        set_play_mode(PlayMode::RepeatAll);
        check(next_queue_index(1, true) == 0, "repeat all wraps at queue end");
        set_play_mode(PlayMode::Shuffle); mark_current(nav_a, 0);
        const int preview_next = next_queue_index(1, true, false);
        const int shuffled = next_queue_index(1, true);
        check(preview_next == shuffled, "preload peek reserves but does not consume shuffle selection");
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
        // 失败跳过使用同一队列规则，但不重试本轮失败曲，也不受单曲循环困住。
        { std::lock_guard<std::mutex> lock(g_mu);
          // 前面的“下一首播放”会重排队列；此组断言明确使用 a、c、b 的顺序。
          g_as.tracks = {nav_a, nav_c, nav_b}; ++g_as.tracks_version; g_play_next.clear(); }
        std::set<std::string> failed{track_uid(nav_a)};
        mark_current(nav_a, 0); set_play_mode(PlayMode::RepeatOne);
        check(next_queue_index(1, false, true, &failed) == 1, "failed song bypasses repeat one");
        failed.insert(track_uid(nav_c));
        check(next_queue_index(1, false, true, &failed) == 2, "consecutive failed songs are excluded");
        failed.insert(track_uid(nav_b));
        check(next_queue_index(1, false, true, &failed) == -1, "all unavailable songs terminate repeat loop");
        set_play_mode(PlayMode::Shuffle);
        check(next_queue_index(1, false, true, &failed) == -1, "all unavailable songs terminate shuffle loop");
        failed.erase(track_uid(nav_c));
        check(next_queue_index(1, false, true, &failed) == 1, "shuffle chooses only an unfailed candidate");
        mark_current(nav_b, 2); set_play_mode(PlayMode::Sequential);
        check(next_queue_index(1, false, true, &failed) == -1, "failed sequential tail does not wrap");
        set_play_mode(PlayMode::RepeatAll);
        check(next_queue_index(1, false, true, &failed) == 1, "repeat all wraps past failed candidates");
        int pending_gen;
        { std::lock_guard<std::mutex> lock(g_mu); pending_gen = g_as.gen; g_as.ps = PlayState::Buffering; g_play_ready_paused = false; }
        toggle_pause();
        { std::lock_guard<std::mutex> lock(g_mu);
          check(g_as.gen == pending_gen && g_as.ps == PlayState::Buffering && g_play_ready_paused,
                "pause while buffering preserves the pending request"); }
        toggle_pause();
        { std::lock_guard<std::mutex> lock(g_mu);
          check(g_as.gen == pending_gen && !g_play_ready_paused, "resume pending playback does not resolve again"); }
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
        const auto old_library = view_model().library_storage;
        favorite_toggle(track); wait_library();
        check(!old_library->contains(track), "published library snapshot remains immutable");
        check(view_model().library.contains(track), "favorite action updates library");
        model.ui.local_name = "测试快照";
        library_save_current(); wait_library();
        check(view_model().library.playlists.size() == 2, "save named playlist action");
        library_remove(1); wait_library();
        check(view_model().library.playlists.size() == 1 && view_model().library.contains(track), "remove playlist preserves favorites");

        const auto music_folder = platform::join(model.paths.config, "music 中文");
        platform::make_dirs(platform::join(music_folder, "disc"));
        auto write_fixture = [](const std::string& path, const char* contents) {
            auto* f = platform::fopen(path, "wb"); check(f != nullptr, "create local fixture");
            std::fputs(contents, f); std::fclose(f);
        };
        // 下载按实际格式排他发布；并发同名下载不得覆盖已有音乐。
        for (const auto* extension : {"flac", "m4a"}) {
            const auto existing = platform::join(music_folder, std::string("song.") + extension);
            const auto part_a = platform::join(music_folder, "a.part"), part_b = platform::join(music_folder, "b.part");
            write_fixture(existing, "original"); write_fixture(part_a, "first"); write_fixture(part_b, "second");
            std::string path_a, path_b, error_a, error_b;
            std::thread a([&] { path_a = publish_download(part_a, music_folder, "song", extension, error_a); });
            std::thread b([&] { path_b = publish_download(part_b, music_folder, "song", extension, error_b); });
            a.join(); b.join();
            check(!path_a.empty() && !path_b.empty() && path_a != path_b && platform::file_size(existing) == 8 &&
                  platform::file_size(path_a) == 5 && platform::file_size(path_b) == 6, "concurrent actual-extension downloads preserve all three files");
            check(!platform::exists(part_a) && !platform::exists(part_b), "successful download publication removes partial files");
            platform::remove(existing); platform::remove(path_a); platform::remove(path_b);
        }
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_as.tracks.clear();
            for (int i = 0; i < 1001; ++i) {
                auto item = track; item.title = "title\twith\nnewlines";
                for (auto& variant : item.variants) variant.id += std::to_string(i);
                item.chosen = 1; g_as.tracks.push_back(std::move(item));
            }
            g_as.now_uid = track_uid(g_as.tracks[910]); g_as.position_ms = 12345;
        }
        check(queue_save(), "large multi-source queue save"); queue_load(); tick();
        check(model.playback.tracks.size() == 1001 && model.playback.tracks[910].variants.size() == 2 &&
              model.playback.tracks[910].chosen == 1 && model.playback.playing == 910 && model.ui.pos_ms == 12345 &&
              model.playback.tracks[910].title == "title\twith\nnewlines" && model.playback.ps == PlayState::Idle,
              "queue restores all songs, variants, chosen source and position without autoplay");
        ++g_queue_gen; g_queue_dirty.store(false);
        write_fixture(queue_path(), "{broken"); queue_load();
        check(!queue_save() && platform::file_size(queue_path()) == 7, "corrupt queue is retained rather than overwritten");
        write_fixture(queue_path(), "# aino queue v1\n5\tlocal.mp3\t\tLegacy\tArtist\tAlbum\t20000\t\n"); queue_load(); tick();
        check(model.playback.tracks.size() == 1 && model.playback.tracks[0].current().id == "local.mp3", "legacy queue migration");
        queue_clear(); queue_push(track); choose_source(0, 1, false); tick();
        const auto original = platform::join(music_folder, "歌手 - 歌名.MP3");
        write_fixture(original, "fixture");
        write_fixture(platform::join(music_folder, "disc/other.flac"), "fixture");
        write_fixture(platform::join(music_folder, "ignore.txt"), "fixture");
        std::string scan_error;
        auto scanned = library::scan_folder(music_folder, false, scan_error);
        check(scan_error.empty() && scanned.tracks.size() == 1 && scanned.tracks[0].title == "歌名", "folder scanner filters and parses names");
        check(scanned.tracks[0].current().source == music::Source::Local, "local source separated from network sources");
        const auto before_preload_uid = model.playback.now_uid;
        const auto before_preload_state = model.playback.ps;
        request_preload(scanned.tracks[0], model.ui.settings);
        bool prepared = false;
        const auto preload_deadline = platform::ticks() + 3000;
        while (!prepared && platform::ticks() < preload_deadline) {
            { std::lock_guard<std::mutex> lock(g_preload_mutex); prepared = bool(g_preload_ready); }
            if (!prepared) platform::sleep_ms(5);
        }
        auto preloaded = take_preloaded(scanned.tracks[0], model.ui.settings);
        check(preloaded && preloaded->original == scanned.tracks[0].current().id && model.playback.now_uid == before_preload_uid &&
              model.playback.ps == before_preload_state, "bounded preload prepares media without publishing playback state");
        preloaded.reset(); check(platform::exists(original), "preload never deletes a local original");
        auto cancelled_request = std::make_shared<PreloadRequest>(); cancelled_request->track = scanned.tracks[0];
        cancelled_request->cancelled.store(true);
        check(!prepare_next(cancelled_request), "cancelled preload cannot prepare or publish stale media");
        auto changed_quality = model.ui.settings; ++changed_quality.quality;
        check(preload_key(scanned.tracks[0], changed_quality) != preload_key(scanned.tracks[0], model.ui.settings), "quality changes invalidate preload");
        write_fixture(platform::join(music_folder, "歌手 - 歌名.lrc"), "\xef\xbb\xbf[00:01.00]本地歌词");
        const auto local_lyrics = lyr::fetch(scanned.tracks[0], model.ui.settings);
        check(local_lyrics.synced && local_lyrics.lines.size() == 1 && local_lyrics.lines[0].ms == 1000, "local BOM LRC is read offline");
        auto cancelled = library::scan_folder(music_folder, true, scan_error, [] { return true; });
        check(!scan_error.empty() && cancelled.tracks.empty(), "scan cancellation");
        library_import_folder(music_folder, true);
        const auto scan_deadline = platform::ticks() + 3000;
        while (model.ui.importing_playlist && platform::ticks() < scan_deadline) { tick(); platform::sleep_ms(10); }
        wait_library();
        check(!model.ui.importing_playlist && view_model().library.playlists.size() == 2, "asynchronous folder import");
        check(view_model().library.playlists[1].tracks.size() == 2 && !view_model().library.playlists[1].local_folder.empty(), "recursive folder playlist");
        library_open(1); tick();
        model.ui.playlist_filter = "歌名"; playlist_filter_rows();
        check(!model.playlist.rows.empty(), "playlist title filtering preserves original row indices");
        model.ui.playlist_rename = "重命名歌单"; playlist_rename_local(); wait_library();
        check(view_model().library.playlists[1].name == "重命名歌单", "rename persists through asynchronous writer");
        model.ui.playlist_filter.clear(); playlist_filter_rows();
        model.ui.library_transfer_path = platform::join(model.paths.config, "backup.json");
        library_export(false);
        const auto export_deadline = platform::ticks() + 4000;
        while (!platform::exists(model.ui.library_transfer_path) && platform::ticks() < export_deadline) { tick(); platform::sleep_ms(5); }
        library::Library backup; std::string backup_error;
        check(backup.load(model.ui.library_transfer_path, backup_error) && backup.playlists.size() == 2, "backup has favorites and local playlist");
        const auto backup_size = platform::file_size(model.ui.library_transfer_path);
        library_export(false); tick();
        check(platform::file_size(model.ui.library_transfer_path) == backup_size, "existing backup cannot be overwritten");
        favorite_toggle(track); favorite_toggle(track); wait_library();
        check(view_model().library.contains(track), "coalesced edits apply in order without dropping toggles");
        model.ui.query = "独立的在线搜索词";
        { std::lock_guard<std::mutex> lock(g_mu); g_as.results = {track}; ++g_as.results_version; }
        tick();
        library_open(1); tick();
        check(model.ui.tab == 3 && model.ui.query == "独立的在线搜索词" && model.playlist.open &&
            model.playlist.playlist.tracks.size() == 2, "folder opens a dedicated detail view without changing search text");
        check(model.playback.tracks.size() == 1 && model.playback.results.size() == 1, "browsing playlist preserves queue and search results");
        music::Settings offline_search;
        for (auto& enabled : offline_search.enable) enabled = false;
        {
            std::lock_guard<std::mutex> lock(g_mu); g_search_worker = true; g_as.searching = true;
        }
        const auto saved_mask = model.ui.src_mask; model.ui.src_mask = 0;
        const int disabled_search = g_search_gen.load();
        model.ui.query = "disabled"; start_search();
        check(g_search_gen.load() == disabled_search, "search with no enabled source is rejected");
        auto dispatch_offline = [&](const char* keyword) {
            SearchRequest request; request.keyword = keyword; request.settings = offline_search;
            dispatch_search(std::move(request));
        };
        dispatch_offline("older"); const int obsolete_search = g_search_gen.load();
        dispatch_offline("latest");
        {
            std::lock_guard<std::mutex> lock(g_mu);
            check(g_pending_search.keyword == "latest" && g_pending_search.generation > obsolete_search,
                  "busy search retains only the latest new request");
            g_search_worker = false; g_pending_search = {};
        }
        { SearchRequest request; request.keyword = "older"; request.settings = offline_search; request.generation = obsolete_search; do_search(g_win, std::move(request)); }
        check(g_as.searching, "obsolete search cannot clear latest request status");
        model.ui.query = "独立的在线搜索词"; dispatch_offline("独立的在线搜索词");
        const auto search_deadline = platform::ticks() + 3000;
        bool searching = true;
        while (searching && platform::ticks() < search_deadline) {
            { std::lock_guard<std::mutex> lock(g_mu); searching = g_search_worker; }
            if (searching) platform::sleep_ms(5);
        }
        check(!searching, "latest offline search completes on the bounded worker");
        model.ui.src_mask = saved_mask;
        const int search_generation = ++g_search_gen;
        { SearchRequest request; request.keyword = "fixture"; request.settings = offline_search; request.generation = search_generation; do_search(g_win, std::move(request)); } tick();
        check(model.playback.tracks.size() == 1 && model.playback.results.empty() && model.playlist.playlist.tracks.size() == 2,
            "online search does not overwrite the playing queue or playlist detail");
        playlist_close();
        check(!model.playlist.open && model.ui.tab == 3 && model.ui.query == "独立的在线搜索词", "return from playlist keeps search state");
        queue_clear();
        for (const auto& local : view_model().library.playlists[1].tracks) queue_push(local);
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
        { std::lock_guard<std::mutex> lock(g_mu);
          local_gen = ++g_as.gen; g_as.tracks = {missing}; ++g_as.tracks_version;
          g_as.now_uid = track_uid(missing); g_as.now_track = missing; g_as.playing = 0;
          g_play_next.clear(); g_failed_tracks.clear(); }
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
        model.ui.lyric_overlay = {true, false, true, false, 78, 12, 32, 780, 1, -240, 860, -16};
        model.ui.lyric_overlay.topmost = false; model.ui.lyric_overlay.karaoke = false;
        model.ui.lyric_overlay.sung_color = 0xff7894; model.ui.lyric_overlay.unsung_color = 0x102030; model.ui.lyric_overlay.background_color = 0x314159;
        check(config::save(model.ui), "persist fake credentials");
        auto& login_state = login::state(login::Site::NetEase);
        { std::lock_guard<std::mutex> lock(login_state.m); login_state.cookie = "MUSIC_U=fixture"; }
        login_state.return_requested.store(true);
        tick();
        check(activations.load() == 1, "login return activates host without UI dependency");
        clear_data(ClearAction::Cookies);
        UiState restored;
        config::load(restored);
        check(restored.lyric_overlay.enabled && !restored.lyric_overlay.double_line && restored.lyric_overlay.locked &&
            !restored.lyric_overlay.translation && restored.lyric_overlay.opacity == 78 && restored.lyric_overlay.background == 12 &&
            restored.lyric_overlay.font_size == 32 && restored.lyric_overlay.width == 780 && restored.lyric_overlay.placement == 1 &&
            restored.lyric_overlay.x == -240 && restored.lyric_overlay.y == 860 && restored.lyric_overlay.panel_offset == -16,
            "floating lyric appearance and dock position survive configuration reload");
        check(!restored.lyric_overlay.topmost && !restored.lyric_overlay.karaoke && restored.lyric_overlay.sung_color == 0xff7894 &&
            restored.lyric_overlay.unsung_color == 0x102030 && restored.lyric_overlay.background_color == 0x314159,
            "floating lyric colors, stacking and karaoke settings survive configuration reload");
        check(restored.settings.netease_cookie.empty() && restored.settings.qq_cookie.empty() && restored.qq_uin.empty() && restored.ne_uid.empty(), "logout persists cleared credentials");
        check(restored.volume == 43 && restored.dark && restored.download_dir == model.ui.download_dir, "logout preserves unrelated settings");
        check(restored.play_mode == PlayMode::Shuffle && !restored.sleep_deadline && !restored.sleep_after_track, "play mode persists without restoring a timer");
        check(view_model().library.contains(track), "logout preserves local library");
        clear_data(ClearAction::Queue);
        tick();
        check(model.playback.tracks.empty() && model.playback.results.size() == 1, "clear queue preserves independent search results");
        check(view_model().library.contains(track), "clear queue preserves favorites");
        const auto cached = platform::join(model.paths.current_cache, "track-99.mp3");
        file = platform::fopen(cached, "wb");
        check(file != nullptr, "cache fixture"); std::fputs("remove", file); std::fclose(file);
        clear_data(ClearAction::Cache);
        check(!platform::exists(cached) && platform::exists(downloaded) && platform::exists(original), "cache clear preserves downloaded songs");
        clear_data(ClearAction::Library); wait_library();
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
