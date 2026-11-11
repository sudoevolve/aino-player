// probe_tag.cpp —— 验证 m4a 内嵌（真实下载一条 QQ 的 m4a 再写标签）
//
// 用法：build\probe_tag.exe <QQ 关键词> <输出目录>
// 做的事：QQ 搜索 → 取流 → 下载 m4a → 从已有 mp3 里抠出封面当素材 → 写 covr/©lyr
#include <cstdio>
#include <string>
#include <vector>

#include "../src/net.hpp"
#include "../src/sources.hpp"
#include "../src/tag.hpp"

static std::string read_config_value(const char* key) {
    wchar_t buf[MAX_PATH] = {0};
    if (GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH) == 0) return {};
    std::string path = net::narrow(buf) + "\\aino-player\\config.ini";
    std::FILE* f = _wfopen(net::widen(path).c_str(), L"rb");
    if (!f) return {};
    char line[8192];
    const std::string k = std::string(key) + "=";
    std::string out;
    while (std::fgets(line, sizeof line, f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.rfind(k, 0) == 0) out = s.substr(k.size());
    }
    std::fclose(f);
    return out;
}

// 只对一个已有文件做内嵌（用来单独验证 m4a 的 atom 手术）
static int mode_embed(const std::string& path) {
    std::string cover;
    {
        std::string all;
        if (tag::read_file("C:////tmp////aino-dl////CaMilLa_晴 - 长街烟火.mp3", all) && all.size() > 10) {
            const size_t apic = all.find("APIC");
            if (apic != std::string::npos) {
                const size_t sz = (size_t(uint8_t(all[apic + 4])) << 24) | (size_t(uint8_t(all[apic + 5])) << 16) |
                                  (size_t(uint8_t(all[apic + 6])) << 8) | uint8_t(all[apic + 7]);
                const size_t begin = all.find("\xff\xd8\xff", apic);
                if (begin != std::string::npos && begin < apic + 8 + sz)
                    cover = all.substr(begin, sz - (begin - apic - 8));
            }
        }
    }
    std::printf("cover=%zu\n", cover.size());
    const std::string lrc = "[00:00.00]测试歌词第一行\r\n[00:05.00]测试歌词第二行\r\n";
    std::string err;
    const bool ok = tag::embed(path, cover, false, lrc, err);
    std::printf("embed=%d err=%s\n", int(ok), err.c_str());
    std::string out;
    if (tag::read_file(path, out)) std::printf("size=%zu verify=%d\n", out.size(), int(tag::verify_mp4(out)));
    return ok ? 0 : 2;
}

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "embed") return mode_embed(argv[2]);
    const std::string kw = argc > 1 ? argv[1] : "长街烟火";
    const std::string dir = argc > 2 ? argv[2] : "C:\\tmp\\aino-dl";

    music::Settings st;
    st.qq_cookie = read_config_value("qq_cookie");
    std::printf("cookie len=%zu\n", st.qq_cookie.size());

    std::string err;
    auto got = music::search_qq(kw, 10, 10000, err, st);
    std::printf("search: %zu hit(s) err=%s\n", got.size(), err.c_str());
    if (got.empty()) return 1;
    // QQ 上大量曲目对非会员返回空 purl，挨个试到能取流为止
    music::Variant v;
    music::ResolveResult rr;
    bool found = false;
    for (const auto& c : got) {
        rr = music::resolve(c, st);
        if (rr.ok) { v = c; found = true; break; }
        std::printf("  skip %s - %s: %s\n", c.title.c_str(), c.artist.c_str(), rr.reason.c_str());
    }
    if (!found) return 1;
    std::printf("pick: %s - %s (%d ms) mid=%s\n", v.title.c_str(), v.artist.c_str(), v.duration_ms, v.id.c_str());
    std::printf("resolve detail=%s\n", rr.detail.c_str());

    const std::string path = dir + "\\probe.m4a";
    std::atomic<bool> cancel{false};
    const bool dl = net::download(rr.url, path, rr.headers, [](long long, long long) {}, cancel, err, 60000);
    std::printf("download=%d err=%s\n", int(dl), err.c_str());
    if (!dl) return 1;

    // 封面素材：从刚才下载好的 mp3 里抠 APIC（省掉一次网络请求，也顺便验证帧结构）
    std::string cover;
    {
        std::string all;
        if (tag::read_file(dir + "\\CaMilLa_晴 - 长街烟火.mp3", all) && all.size() > 10) {
            const size_t apic = all.find("APIC");
            if (apic != std::string::npos) {
                const size_t sz = (size_t(uint8_t(all[apic + 4])) << 24) | (size_t(uint8_t(all[apic + 5])) << 16) |
                                  (size_t(uint8_t(all[apic + 6])) << 8) | uint8_t(all[apic + 7]);
                const size_t begin = all.find("\xff\xd8\xff", apic);
                if (begin != std::string::npos && begin < apic + 8 + sz) cover = all.substr(begin, sz - (begin - apic - 8));
            }
        }
    }
    std::printf("cover bytes=%zu\n", cover.size());

    const std::string lrc = "[00:00.00]测试歌词第一行\r\n[00:05.00]测试歌词第二行\r\n";
    std::string terr;
    const bool ok = tag::embed(path, cover, /*is_png=*/false, lrc, terr);
    std::printf("embed=%d err=%s\n", int(ok), terr.c_str());

    std::string out;
    if (tag::read_file(path, out)) std::printf("final size=%zu verify=%d\n", out.size(), int(tag::verify_mp4(out)));
    return ok ? 0 : 2;
}
