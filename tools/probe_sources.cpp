// probe_sources.cpp —— 源层探针：不涉及 UI，直接验证检索/合并/取流这条链
// 用法：probe_sources.exe [关键词]
#include <cstdio>
#include <string>

#include "../src/sources.hpp"

// 命令行参数必须走宽字符：main(int,char**) 拿到的是 GBK 字节，
// 直接当 UTF-8 拼进 URL 会让服务端解析成乱码（实测搜"周杰伦"返回一堆无关歌）
static std::string first_arg_utf8() {
    const std::wstring cmd = GetCommandLineW();
    size_t i = 0;
    // 跳过 argv[0]（可能带引号）
    if (i < cmd.size() && cmd[i] == L'"') {
        i = cmd.find(L'"', i + 1);
        i = (i == std::wstring::npos) ? cmd.size() : i + 1;
    } else {
        i = cmd.find(L' ');
        if (i == std::wstring::npos) return std::string();
    }
    while (i < cmd.size() && cmd[i] == L' ') ++i;
    if (i >= cmd.size()) return std::string();
    std::wstring arg;
    if (cmd[i] == L'"') {
        ++i;
        while (i < cmd.size() && cmd[i] != L'"') arg += cmd[i++];
    } else {
        while (i < cmd.size() && cmd[i] != L' ') arg += cmd[i++];
    }
    return net::narrow(arg);
}

static void print_utf8_console() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);  // 被 kill 时也要能看到进度
}

int main() {
    print_utf8_console();
    std::string keyword = first_arg_utf8();
    if (keyword.empty()) keyword = "周杰伦";
    music::Settings st;

    std::printf("== search \"%s\" ==\n", keyword.c_str());
    const music::SearchResult r = music::run_search(keyword, st, 8, 9000);
    for (const auto& rep : r.reports) {
        std::printf("%-8s ok=%d count=%-3d %4dms %s\n", music::source_name(rep.source), int(rep.ok),
                    rep.count, rep.ms, rep.err.c_str());
    }
    std::printf("merged=%zu elapsed=%dms\n\n", r.tracks.size(), r.elapsed_ms);

    size_t shown = 0;
    for (const auto& t : r.tracks) {
        std::printf("[%5.1f] %s - %s  (%d:%02d) variants=%zu\n", t.score, t.title.c_str(), t.artist.c_str(),
                    t.duration_ms / 60000, (t.duration_ms / 1000) % 60, t.variants.size());
        for (const auto& v : t.variants)
            std::printf("        %-8s id=%-16s %s%s\n", music::source_name(v.source), v.id.c_str(),
                        v.needs_login ? "NEEDS_LOGIN " : "", v.album.c_str());
        if (++shown >= 6) break;
    }

    if (!r.tracks.empty()) {
        std::printf("\n== resolve 前 3 首的每个源 ==\n");
        for (size_t i = 0; i < r.tracks.size() && i < 3; ++i) {
            for (const auto& v : r.tracks[i].variants) {
                const music::ResolveResult rr = music::resolve(v, st);
                std::printf("%-24s %-8s ok=%d %-16s %s\n", r.tracks[i].title.c_str(),
                            music::source_name(v.source), int(rr.ok), rr.reason.c_str(),
                            rr.ok ? rr.url.substr(0, 70).c_str() : rr.detail.c_str());
            }
        }
    }
    return 0;
}
