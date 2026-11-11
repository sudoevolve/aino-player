// probe_audio.cpp —— 验证 MCI 能不能播 m4a（QQ 音乐给的是 mp4 容器）
#include <windows.h>
#include <mmsystem.h>

#include <cstdio>
#include <string>

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

static bool try_open(const std::string& path, const char* type, const char* alias) {
    char cmd[1024];
    if (type && *type)
        std::snprintf(cmd, sizeof cmd, "open \"%s\" type %s alias %s", path.c_str(), type, alias);
    else
        std::snprintf(cmd, sizeof cmd, "open \"%s\" alias %s", path.c_str(), alias);

    MCIERROR e = mciSendStringW(widen(cmd).c_str(), nullptr, 0, nullptr);
    if (e) {
        wchar_t buf[256] = {0};
        mciGetErrorStringW(e, buf, 256);
        std::printf("  open %-12s FAIL err=%lu %s\n", type ? type : "(auto)", e, narrow(buf).c_str());
        return false;
    }
    wchar_t ret[128] = {0};
    char q[128];
    std::snprintf(q, sizeof q, "status %s length", alias);
    mciSendStringW(widen(q).c_str(), ret, 128, nullptr);
    std::printf("  open %-12s OK  length=%s ms\n", type ? type : "(auto)", narrow(ret).c_str());

    std::snprintf(q, sizeof q, "play %s", alias);
    MCIERROR pe = mciSendStringW(widen(q).c_str(), nullptr, 0, nullptr);
    if (pe) {
        wchar_t b2[256] = {0};
        mciGetErrorStringW(pe, b2, 256);
        std::printf("    play FAIL %s\n", narrow(b2).c_str());
    } else {
        Sleep(1200);
        std::snprintf(q, sizeof q, "status %s position", alias);
        wchar_t pos[64] = {0};
        mciSendStringW(widen(q).c_str(), pos, 64, nullptr);
        std::printf("    play OK   position=%s ms\n", narrow(pos).c_str());
        std::snprintf(q, sizeof q, "status %s mode", alias);
        wchar_t mode[64] = {0};
        mciSendStringW(widen(q).c_str(), mode, 64, nullptr);
        std::printf("    mode=%s\n", narrow(mode).c_str());
    }
    std::snprintf(q, sizeof q, "close %s", alias);
    mciSendStringW(widen(q).c_str(), nullptr, 0, nullptr);
    return true;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::string path = "C:/Users/admin/AppData/Local/Temp/aino-player/track-3.mp3";
    int n = 0;
    if (LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n)) {
        if (n > 1) path = narrow(w[1]);
        LocalFree(w);
    }
    std::printf("file: %s\n", path.c_str());
    try_open(path, "", "a1");
    try_open(path, "mpegvideo", "a2");
    try_open(path, "MPEGVideo", "a3");

    // 换个扩展名再试：MCI 有时按扩展名选设备
    const std::string alt = path + ".m4a";
    CopyFileW(widen(path).c_str(), widen(alt).c_str(), FALSE);
    try_open(alt, "", "a4");
    try_open(alt, "mpegvideo", "a5");
    DeleteFileW(widen(alt).c_str());
    return 0;
}
