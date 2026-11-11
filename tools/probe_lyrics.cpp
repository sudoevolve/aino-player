// probe_lyrics.cpp —— 歌词链路探针：不启动界面，直接验证抓取与解析
// 用法：probe_lyrics.exe "花海" "周杰伦"
#include <cstdio>
#include <string>

// 先拉进 windows.h（经 net.hpp），否则单独 include shellapi.h 会缺 HDROP 等类型
#include "../src/lyrics.hpp"

#include <shellapi.h>

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::string title = "花海", artist = "周杰伦";
    int duration_ms = 260000;
    int argc = 0;
    if (LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        if (argc > 1) title = net::narrow(wargv[1]);
        if (argc > 2) artist = net::narrow(wargv[2]);
        if (argc > 3) duration_ms = _wtoi(wargv[3]);
        LocalFree(wargv);
    }

    music::Track t;
    t.title = title;
    t.artist = artist;
    t.duration_ms = duration_ms;

    music::Settings st;
    const unsigned long long t0 = GetTickCount64();
    const lyr::Doc doc = lyr::fetch(t, st);
    const unsigned long long ms = GetTickCount64() - t0;

    std::printf("=== %s / %s  (%llu ms)\n", title.c_str(), artist.c_str(), ms);
    if (!doc.error.empty()) std::printf("ERROR: %s\n", doc.error.c_str());
    std::printf("origin=%s synced=%d lines=%d\n", doc.origin.c_str(), int(doc.synced), int(doc.lines.size()));
    for (int i = 0; i < int(doc.lines.size()) && i < 8; ++i) {
        std::printf("  [%6.2fs] %s\n", doc.lines[size_t(i)].ms / 1000.0, doc.lines[size_t(i)].text.c_str());
    }
    if (doc.lines.size() > 8) {
        std::printf("  ... 共 %d 行，当前行@30s = %d\n", int(doc.lines.size()),
                    lyr::current_line(doc.lines, 30000));
    }
    return doc.lines.empty() ? 1 : 0;
}
