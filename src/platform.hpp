// 平台边界：UTF-8 文件路径、应用目录、时钟与子进程。
#pragma once
#include <chrono>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include <codecvt>
#include <locale>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <bcrypt.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>
extern char** environ;
#endif

namespace platform {
namespace fs = std::filesystem;
inline std::wstring widen(const std::string& s) {
#ifdef _WIN32
    return fs::u8path(s).wstring();
#else
    return std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.from_bytes(s);
#endif
}
inline std::string narrow(const std::wstring& s) {
#ifdef _WIN32
    return fs::path(s).u8string();
#else
    return std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.to_bytes(s);
#endif
}
inline std::FILE* fopen(const std::string& path, const char* mode) {
#ifdef _WIN32
    return _wfopen(widen(path).c_str(), widen(mode).c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}
inline std::string env(const char* key) {
#ifdef _WIN32
    const auto name = widen(key);
    DWORD n = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
    if (!n) return {};
    std::wstring value(n, L'\0');
    n = GetEnvironmentVariableW(name.c_str(), value.data(), n);
    if (!n || n >= value.size()) return {};
    value.resize(n);
    return narrow(value);
#else
    const char* p = std::getenv(key);
    return p ? p : "";
#endif
}
inline std::string join(const std::string& a, const std::string& b) {
    return (fs::u8path(a) / fs::u8path(b)).u8string();
}
inline bool make_dirs(const std::string& path) {
    std::error_code ec;
    fs::create_directories(fs::u8path(path), ec);
    return !ec;
}
inline bool exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(fs::u8path(path), ec);
}
inline void remove(const std::string& path) { std::error_code ec; fs::remove(fs::u8path(path), ec); }
inline bool replace_file(const std::string& from, const std::string& to) {
#ifdef _WIN32
    return MoveFileExW(widen(from).c_str(), widen(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return ::rename(from.c_str(), to.c_str()) == 0;
#endif
}
inline long long file_size(const std::string& path) {
    std::error_code ec;
    const auto n = fs::file_size(fs::u8path(path), ec);
    return ec ? -1 : static_cast<long long>(n);
}
inline unsigned long long ticks() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
inline std::string config_dir() {
#ifdef _WIN32
    auto root = env("APPDATA");
#else
    auto root = env("XDG_CONFIG_HOME");
    if (root.empty()) root = join(env("HOME"), ".config");
#endif
    if (root.empty()) return {};
    auto dir = join(root, "aino-player");
    if (!make_dirs(dir)) return {};
#ifndef _WIN32
    ::chmod(dir.c_str(), 0700);
#endif
    return dir;
}
inline uint32_t process_id() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return uint32_t(getpid());
#endif
}
inline bool process_running(uint32_t pid) {
    if (!pid) return false;
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return GetLastError() != ERROR_INVALID_PARAMETER;
    DWORD code = STILL_ACTIVE;
    const bool alive = !GetExitCodeProcess(process, &code) || code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
#else
    return ::kill(pid_t(pid), 0) == 0 || errno != ESRCH;
#endif
}
inline std::string cache_root() {
    std::error_code ec;
    const auto root = fs::temp_directory_path(ec);
    if (ec) return {};
    // 不同进程不能删除对方正在播放的缓存。
#ifdef _WIN32
    const auto base = root / "aino-player";
#else
    const auto base = root / ("aino-player-" + std::to_string(getuid()));
#endif
    if (!make_dirs(base.u8string())) return {};
#ifndef _WIN32
    ::chmod(base.c_str(), 0700);
#endif
    return base.u8string();
}
inline std::string temp_dir() {
    const auto base = cache_root();
    if (base.empty()) return {};
    const auto dir = join(base, std::to_string(process_id()));
    if (!make_dirs(dir)) return {};
#ifndef _WIN32
    ::chmod(dir.c_str(), 0700);
#endif
    return dir + "/";
}
inline std::string download_dir() {
#ifdef _WIN32
    const auto home = env("USERPROFILE");
#else
    const auto home = env("HOME");
#endif
    return join(join(home, "Music"), "aino player");
}
// argv 不经过 shell，中文、引号等文件名不会成为命令。
inline int run_process(const std::vector<std::string>& args, unsigned timeout_ms = 0) {
    if (args.empty()) return -1;
#ifdef _WIN32
    std::wstring command;
    for (const auto& arg : args) {
        if (!command.empty()) command += L' ';
        command += L'"';
        size_t slashes = 0;
        for (wchar_t c : widen(arg)) {
            if (c == L'\\') { ++slashes; continue; }
            command.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            slashes = 0;
            command += c;
        }
        command.append(slashes * 2, L'\\');
        command += L'"';
    }
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | (timeout_ms ? CREATE_SUSPENDED : 0),
                        nullptr, nullptr, &si, &pi)) return -1;
    HANDLE job = timeout_ms ? CreateJobObjectW(nullptr, nullptr) : nullptr;
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits) ||
            !AssignProcessToJobObject(job, pi.hProcess)) { CloseHandle(job); job = nullptr; }
    }
    if (timeout_ms) ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    if (WaitForSingleObject(pi.hProcess, timeout_ms ? timeout_ms : INFINITE) == WAIT_TIMEOUT) {
        if (job) TerminateJobObject(job, 124); else TerminateProcess(pi.hProcess, 124);
        WaitForSingleObject(pi.hProcess, 5000);
        if (job) CloseHandle(job);
        CloseHandle(pi.hProcess); return 124;
    }
    if (job) CloseHandle(job);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
#else
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    posix_spawnattr_t attr; posix_spawnattr_init(&attr);
    if (timeout_ms) { posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP); posix_spawnattr_setpgroup(&attr, 0); }
    const int spawned = posix_spawnp(&pid, argv[0], nullptr, &attr, argv.data(), environ);
    posix_spawnattr_destroy(&attr);
    if (spawned) return -1;
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const auto result = waitpid(pid, &status, timeout_ms ? WNOHANG : 0);
        if (result == pid) break;
        if (result < 0 && errno != EINTR) return -1;
        if (timeout_ms && std::chrono::steady_clock::now() >= deadline) {
            kill(-pid, SIGKILL); while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            return 124;
        }
        if (timeout_ms) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}
// Windows 原生选择器；其他桌面保留可粘贴的跨平台路径输入，不新增桌面工具依赖。
inline std::string choose_music_folder() {
#ifdef _WIN32
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    BROWSEINFOW info{}; info.hwndOwner = GetActiveWindow();
    info.lpszTitle = L"选择音乐文件夹";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    auto* item = SHBrowseForFolderW(&info);
    wchar_t path[MAX_PATH]{};
    std::string result;
    if (item) { if (SHGetPathFromIDListW(item, path)) result = narrow(path); CoTaskMemFree(item); }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return result;
#else
    return {};
#endif
}
inline bool open_browser(const std::string& url) {
#ifdef _WIN32
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", widen(url).c_str(),
        nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#else
    return run_process({"xdg-open", url}) == 0;
#endif
}
#ifdef _WIN32
// 流式校验 GitHub 提供的安装包摘要，不把整个安装包读进内存。
inline std::string sha256_file(const std::string& path) {
    auto* file = fopen(path, "rb");
    if (!file) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
              BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::array<unsigned char, 65536> buffer{};
    while (ok) {
        const auto read = std::fread(buffer.data(), 1, buffer.size(), file);
        if (read && BCryptHashData(hash, buffer.data(), ULONG(read), 0) < 0) { ok = false; break; }
        if (read < buffer.size()) { ok = !std::ferror(file); break; }
    }
    std::array<unsigned char, 32> digest{};
    if (ok) ok = BCryptFinishHash(hash, digest.data(), ULONG(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    std::fclose(file);
    if (!ok) return {};
    std::string result;
    for (const auto byte : digest) { result += "0123456789abcdef"[byte >> 4]; result += "0123456789abcdef"[byte & 15]; }
    return result;
}
inline bool launch_installer(const std::string& path) {
    const auto file = widen(path);
    SHELLEXECUTEINFOW info{}; info.cbSize = sizeof info; info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"open"; info.lpFile = file.c_str();
    info.lpParameters = L"/SILENT /NORESTART /CLOSEAPPLICATIONS /AINORESTART=1";
    info.nShow = SW_SHOWNORMAL;
    const bool launched = ShellExecuteExW(&info) != FALSE;
    if (info.hProcess) CloseHandle(info.hProcess);
    return launched;
}
#endif
inline bool open_folder(const std::string& path) {
    std::error_code ec;
    if (!fs::is_directory(fs::u8path(path), ec) || ec) return false;
#ifdef _WIN32
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", widen(path).c_str(),
        nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#else
    return run_process({"xdg-open", path}) == 0;
#endif
}
} // namespace platform
