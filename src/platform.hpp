// 平台边界：UTF-8 文件路径、应用目录、时钟与子进程。
#pragma once
#include <chrono>
#include <array>
#include <atomic>
#include <functional>
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
#include <mutex>
#include <algorithm>
#include <memory>
#include <utility>
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
#include <shobjidl.h>
#include <bcrypt.h>
#else
#include <spawn.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
extern char** environ;
#endif

namespace platform {
inline constexpr const char* audio_extensions[]{".mp3", ".flac", ".wav", ".wave", ".m4a", ".aac", ".ogg", ".oga", ".opus", ".webm"};
inline constexpr const char* audio_mimes[]{"audio/mpeg", "audio/flac", "audio/x-flac", "audio/wav", "audio/x-wav", "audio/mp4", "audio/x-m4a", "audio/aac", "audio/x-aac", "audio/ogg", "audio/x-vorbis+ogg", "application/ogg", "audio/opus", "audio/webm"};
enum class MediaAction { Toggle, Play, Pause, Stop, Next, Previous, SeekRelative, SeekAbsolute, Volume, Show, Quit, Shuffle, Loop, OpenFiles, Search };
struct MediaEvent { MediaAction action; long long value = 0; std::string track; std::vector<std::string> files; };
// 系统打开与拖放只接受本地路径 / file URI，不把外部地址当音乐搜索词或执行命令。
std::string local_file_path(const std::string& value);
std::vector<std::string> local_uri_list(const std::string& value);
bool set_default_music_player(std::string& error, const std::function<bool()>& cancelled = {});
bool forward_local_files(const std::vector<std::string>& paths);
bool forward_search(const std::string& query);
inline std::string config_dir();
inline std::string instance_scope() {
    const auto directory = config_dir();
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char byte : directory) { hash ^= byte; hash *= 1099511628211ULL; }
    return std::to_string(hash);
}
/** @brief 本机活动网络接口地址；仅用于本地测速缓存签名，不发送或显示。 */
std::string network_signature();
/** @brief 后台批处理结束后归还分配器的空闲页；不移除活跃对象或强制裁剪工作集。 */
void release_unused_memory();
struct MediaState {
    std::string uid, title, artist, album, artwork;
    long long position_ms = 0, duration_ms = 0;
    int volume = 70, loop = 0;
    bool playing = false, paused = false, shuffle = false, can_seek = false;
};
/** @brief 悬浮歌词设置；坐标为物理像素，尺寸与字号为逻辑像素。 */
struct LyricOverlaySettings {
    bool enabled = false, double_line = true, locked = false, translation = true;
    int opacity = 100, background = 20, font_size = 26, width = 640;
    int placement = 0, x = -32000, y = -32000, panel_offset = 0; // 0 自由悬浮，1 拖到任务栏 / 顶栏停靠。
    bool topmost = true, karaoke = true;
    uint32_t sung_color = 0x31b6ff, unsung_color = 0xf5f5f5, background_color = 0x121212;
};
struct LyricOverlayProgress { float first = -1, second = -1; }; // -1 无时间轴，不显示进度填充。
using LyricPainter = std::function<void(uint8_t*, int, int, float, void*)>;
/** @brief 桌面媒体与托盘边界；只向 UI 线程排事件，不从系统回调直接操作播放器。 */
class Desktop {
public:
    Desktop();
    ~Desktop();
    Desktop(const Desktop&) = delete;
    Desktop& operator=(const Desktop&) = delete;
    void start(void* window, void* display, unsigned long xid, std::function<void()> wake);
    void stop();
    void update(const MediaState& state, bool tray_enabled);
    std::vector<MediaEvent> events();
    bool tray_available() const;
    bool media_available() const;
    bool hidden() const;
    void hide();
    void show();
    void set_icon(int width, int height, const std::vector<uint8_t>& rgba);
    /** @brief UI 线程调用；复用现有字体绘制，仅内容 / 设置变化时重绘；返回拖动造成的设置变化。 */
    bool update_lyrics(LyricOverlaySettings& settings, const std::string& first, const std::string& second,
                       const LyricOverlayProgress& progress, float scale, const LyricPainter& paint);
    std::string lyrics_error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
namespace fs = std::filesystem;
inline std::string path_utf8(const fs::path& path) {
    const auto bytes = path.u8string();
    return std::string(bytes.begin(), bytes.end());
}
inline std::wstring widen(const std::string& s) {
#ifdef _WIN32
    return fs::u8path(s).wstring();
#else
    return std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.from_bytes(s);
#endif
}
inline std::string narrow(const std::wstring& s) {
#ifdef _WIN32
    return path_utf8(fs::path(s));
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
    return path_utf8(fs::u8path(a) / fs::u8path(b));
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
/** @brief 发布同目录临时文件，目标已存在时绝不覆盖；调用方可换名重试。 */
inline bool move_file_no_replace(const std::string& from, const std::string& to) {
#ifdef _WIN32
    return MoveFileExW(widen(from).c_str(), widen(to).c_str(), MOVEFILE_WRITE_THROUGH) != 0;
#else
    // link 原子创建最终名称；避免 exists + rename 之间被其他下载抢占。
    if (::link(from.c_str(), to.c_str()) == 0) { ::unlink(from.c_str()); return true; }
    if (errno == EEXIST) return false;
    // FAT 等文件系统不支持硬链接；排他创建目标仍可保证不覆盖已有音乐。
    std::FILE* input = std::fopen(from.c_str(), "rb");
    if (!input) return false;
    const int fd = ::open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) { std::fclose(input); return false; }
    std::FILE* output = ::fdopen(fd, "wb");
    if (!output) { ::close(fd); ::unlink(to.c_str()); std::fclose(input); return false; }
    char buffer[65536]; bool ok = true; size_t count;
    while ((count = std::fread(buffer, 1, sizeof buffer, input)) != 0)
        if (std::fwrite(buffer, 1, count, output) != count) { ok = false; break; }
    ok = !std::ferror(input) && ok;
    std::fclose(input);
    ok = std::fclose(output) == 0 && ok;
    if (!ok) { ::unlink(to.c_str()); return false; }
    ::unlink(from.c_str()); return true;
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
/** @brief 持有当前用户资料目录的写入权；进程退出或崩溃后由系统释放。 */
class InstanceLock {
public:
    enum class Result { Primary, Existing, Error };
    InstanceLock() = default;
    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;
    ~InstanceLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
        if (handle_ >= 0) ::close(handle_);
#endif
    }
    Result acquire() {
        const auto dir = config_dir();
        if (dir.empty()) return Result::Error;
        const auto path = join(dir, "player.lock");
#ifdef _WIN32
        handle_ = CreateFileW(widen(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ != INVALID_HANDLE_VALUE) return Result::Primary;
        return GetLastError() == ERROR_SHARING_VIOLATION ? Result::Existing : Result::Error;
#else
        // 不删除锁文件：unlink 会让后来进程锁住不同 inode，失去互斥。
        handle_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (handle_ < 0) return Result::Error;
        if (::flock(handle_, LOCK_EX | LOCK_NB) == 0) return Result::Primary;
        return errno == EWOULDBLOCK || errno == EAGAIN ? Result::Existing : Result::Error;
#endif
    }
private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int handle_ = -1;
#endif
};
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
    if (!make_dirs(path_utf8(base))) return {};
#ifndef _WIN32
    ::chmod(base.c_str(), 0700);
#endif
    return path_utf8(base);
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
inline int run_process(const std::vector<std::string>& args, unsigned timeout_ms = 0, const std::function<bool()>& cancelled = {}) {
    if (args.empty()) return -1;
    if (cancelled && cancelled()) return 125;
    const bool bounded = timeout_ms || bool(cancelled);
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
                        CREATE_NO_WINDOW | (bounded ? CREATE_SUSPENDED : 0),
                        nullptr, nullptr, &si, &pi)) return -1;
    HANDLE job = bounded ? CreateJobObjectW(nullptr, nullptr) : nullptr;
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits) ||
            !AssignProcessToJobObject(job, pi.hProcess)) { CloseHandle(job); job = nullptr; }
    }
    if (bounded) ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    const auto began = std::chrono::steady_clock::now();
    for (;;) {
        if (WaitForSingleObject(pi.hProcess, bounded ? 20 : INFINITE) != WAIT_TIMEOUT) break;
        const bool stop = cancelled && cancelled();
        if (stop || (timeout_ms && std::chrono::steady_clock::now() - began >= std::chrono::milliseconds(timeout_ms))) {
            const int code = stop ? 125 : 124;
            if (job) TerminateJobObject(job, code); else TerminateProcess(pi.hProcess, code);
            WaitForSingleObject(pi.hProcess, 5000);
            if (job) CloseHandle(job);
            CloseHandle(pi.hProcess); return code;
        }
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
    if (bounded) { posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP); posix_spawnattr_setpgroup(&attr, 0); }
    const int spawned = posix_spawnp(&pid, argv[0], nullptr, &attr, argv.data(), environ);
    posix_spawnattr_destroy(&attr);
    if (spawned) return -1;
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const auto result = waitpid(pid, &status, bounded ? WNOHANG : 0);
        if (result == pid) break;
        if (result < 0 && errno != EINTR) return -1;
        const bool stop = cancelled && cancelled();
        if (stop || (timeout_ms && std::chrono::steady_clock::now() >= deadline)) {
            kill(-pid, SIGKILL); while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            return stop ? 125 : 124;
        }
        if (bounded) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}
/**
 * @brief 音频管道子进程；输入和输出各由一个工作线程独占。
 *
 * terminate 可并发调用，只终止子进程，不关闭正在读写的父端句柄。
 * 所有 I/O 线程结束后才能销毁；wait 负责回收进程，防止 PID 复用误杀。
 */
class PipeProcess {
public:
    PipeProcess() = default;
    PipeProcess(const PipeProcess&) = delete;
    PipeProcess& operator=(const PipeProcess&) = delete;
    ~PipeProcess() {
        terminate(); wait(); finish_input();
#ifdef _WIN32
        if (output_) CloseHandle(output_);
        if (process_) CloseHandle(process_);
        if (job_) CloseHandle(job_);
#else
        if (output_ >= 0) ::close(output_);
#endif
    }
    bool start(const std::vector<std::string>& args) {
        if (args.empty()) return false;
#ifdef _WIN32
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE input_read = nullptr, output_write = nullptr;
        if (!CreatePipe(&input_read, &input_, &security, 0)) return false;
        if (!CreatePipe(&output_, &output_write, &security, 0)) { CloseHandle(input_read); return false; }
        SetHandleInformation(input_, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(output_, HANDLE_FLAG_INHERIT, 0);
        HANDLE null = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
        if (null == INVALID_HANDLE_VALUE) { CloseHandle(input_read); CloseHandle(output_write); return false; }
        std::wstring command;
        for (const auto& arg : args) {
            if (!command.empty()) command += L' ';
            command += L'"'; size_t slashes = 0;
            for (wchar_t c : widen(arg)) {
                if (c == L'\\') { ++slashes; continue; }
                command.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\'); slashes = 0; command += c;
            }
            command.append(slashes * 2, L'\\'); command += L'"';
        }
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof startup;
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input_read; startup.StartupInfo.hStdOutput = output_write; startup.StartupInfo.hStdError = null;
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<unsigned char> attributes(bytes);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        bool initialized = InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes) != 0;
        HANDLE inherited[] = {input_read, output_write, null};
        const bool limited = initialized && UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited, sizeof inherited, nullptr, nullptr);
        PROCESS_INFORMATION info{};
        const bool opened = limited && CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &info);
        if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
        CloseHandle(input_read); CloseHandle(output_write); CloseHandle(null);
        if (!opened) return false;
        process_ = info.hProcess;
        job_ = CreateJobObjectW(nullptr, nullptr);
        if (job_) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof limits) || !AssignProcessToJobObject(job_, process_)) {
                CloseHandle(job_); job_ = nullptr;
            }
        }
        ResumeThread(info.hThread); CloseHandle(info.hThread);
        return true;
#else
        int input[2]{-1,-1}, output[2]{-1,-1};
#if defined(__linux__)
        if (::pipe2(input, O_CLOEXEC)) return false;
        if (::pipe2(output, O_CLOEXEC)) { ::close(input[0]); ::close(input[1]); return false; }
#else
        if (::pipe(input)) return false;
        if (::pipe(output)) { ::close(input[0]); ::close(input[1]); return false; }
#endif
        // 所有管道端设置 close-on-exec；只有 dup2 后的标准输入/输出传给解码器。
        for (int fd : {input[0], input[1], output[0], output[1]}) fcntl(fd, F_SETFD, FD_CLOEXEC);
        posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        for (int fd : {input[0], input[1], output[0], output[1]}) if (fd > STDERR_FILENO) posix_spawn_file_actions_addclose(&actions, fd);
        posix_spawnattr_t attr; posix_spawnattr_init(&attr);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP); posix_spawnattr_setpgroup(&attr, 0);
        std::vector<char*> argv; for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str())); argv.push_back(nullptr);
        const int rc = posix_spawnp(&pid_, argv[0], &actions, &attr, argv.data(), environ);
        posix_spawnattr_destroy(&attr); posix_spawn_file_actions_destroy(&actions);
        ::close(input[0]); ::close(output[1]);
        input_ = input[1]; output_ = output[0];
        if (rc) { pid_ = 0; return false; }
        return true;
#endif
    }
    bool write(const char* data, size_t size) {
#ifndef _WIN32
        // 只屏蔽当前写入线程的 SIGPIPE，不改变应用其余线程的信号设置。
        sigset_t blocked, old, pending; sigemptyset(&blocked); sigaddset(&blocked, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &blocked, &old); sigpending(&pending);
        const bool already_pending = sigismember(&pending, SIGPIPE) == 1;
        bool broken = false;
#endif
        bool ok = true;
        while (size) {
#ifdef _WIN32
            DWORD written = 0;
            if (!WriteFile(input_, data, DWORD(std::min<size_t>(size, 65536)), &written, nullptr) || !written) { ok = false; break; }
#else
            const auto written = ::write(input_, data, size);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { broken = errno == EPIPE; ok = false; break; }
#endif
            data += written; size -= size_t(written);
        }
#ifndef _WIN32
        if (broken && !already_pending) { timespec zero{}; while (sigtimedwait(&blocked, nullptr, &zero) < 0 && errno == EINTR) {} }
        pthread_sigmask(SIG_SETMASK, &old, nullptr);
#endif
        return ok;
    }
    size_t read(char* data, size_t size) {
#ifdef _WIN32
        DWORD got = 0;
        return ReadFile(output_, data, DWORD(size), &got, nullptr) ? size_t(got) : 0;
#else
        ssize_t got;
        do { got = ::read(output_, data, size); } while (got < 0 && errno == EINTR);
        return got > 0 ? size_t(got) : 0;
#endif
    }
    void finish_input() {
#ifdef _WIN32
        if (input_) { CloseHandle(input_); input_ = nullptr; }
#else
        if (input_ >= 0) { ::close(input_); input_ = -1; }
#endif
    }
    void terminate() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (reaped_) return;
#ifdef _WIN32
        if (process_) { if (job_) TerminateJobObject(job_, 125); else TerminateProcess(process_, 125); }
#else
        if (pid_ > 0) kill(-pid_, SIGKILL);
#endif
    }
    int wait() {
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (reaped_) return code_;
#ifdef _WIN32
                if (!process_) return -1;
                if (WaitForSingleObject(process_, 0) != WAIT_TIMEOUT) {
                    DWORD code = 1; GetExitCodeProcess(process_, &code); code_ = int(code); reaped_ = true; return code_;
                }
#else
                if (pid_ <= 0) return -1;
                int status = 0; const auto got = waitpid(pid_, &status, WNOHANG);
                if (got == pid_ || (got < 0 && errno != EINTR)) {
                    code_ = got == pid_ && WIFEXITED(status) ? WEXITSTATUS(status) : -1; reaped_ = true; return code_;
                }
#endif
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
private:
    std::mutex mutex_;
    bool reaped_ = false;
    int code_ = -1;
#ifdef _WIN32
    HANDLE input_ = nullptr, output_ = nullptr, process_ = nullptr, job_ = nullptr;
#else
    int input_ = -1, output_ = -1;
    pid_t pid_ = 0;
#endif
};
// 由专用 STA 工作线程调用；取消计时器也在该 apartment 关闭对话框。
#ifdef _WIN32
struct FolderPickerCancel { IFileOpenDialog* dialog; const std::atomic<bool>* cancelled; };
inline void CALLBACK cancel_folder_picker(HWND window, UINT, UINT_PTR, DWORD) {
    const auto* state = reinterpret_cast<const FolderPickerCancel*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (state && state->cancelled->load()) state->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
}
#endif
#ifndef _WIN32
std::string choose_desktop_file(bool folder, const std::atomic<bool>& cancelled, std::string& error,
    int kind = 0, std::vector<std::string>* multi = nullptr);
#endif
// kind：0 歌词 / 文件夹，1 本地音频，2 音源脚本。
// multi 非空时开启多选，所有选中的路径都填进去（selected 仍返回第一个，兼容旧调用）。
inline std::string choose_music_folder(void* owner, const std::atomic<bool>& cancelled, std::string& error,
                                       bool folder = true, int kind = 0,
                                       std::vector<std::string>* multi = nullptr) {
    error.clear();
#ifdef _WIN32
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) { error = "无法初始化文件夹选择器"; return {}; }
    IFileOpenDialog* dialog = nullptr;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(result)) { CoUninitialize(); error = "无法创建文件夹选择器"; return {}; }
    DWORD options = 0;
    result = dialog->GetOptions(&options);
    if (SUCCEEDED(result)) result = dialog->SetOptions(options | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST)
        | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR
        | (multi && !folder ? FOS_ALLOWMULTISELECT : 0));
    if (SUCCEEDED(result)) result = dialog->SetTitle(folder ? L"选择音乐文件夹"
                                                    : (kind == 2 ? L"选择音源脚本（.js）" : kind == 1 ? L"打开本地音乐" : L"选择本地歌词"));
    if (!folder && SUCCEEDED(result)) {
        if (kind == 2) {
            const COMDLG_FILTERSPEC filters[]{{L"音源脚本 (*.js)", L"*.js"}, {L"所有文件", L"*.*"}};
            result = dialog->SetFileTypes(2, filters);
        } else if (kind == 1) {
            const COMDLG_FILTERSPEC filters[]{{L"音乐文件", L"*.mp3;*.flac;*.wav;*.wave;*.m4a;*.aac;*.ogg;*.oga;*.opus;*.webm"}, {L"所有文件", L"*.*"}};
            result = dialog->SetFileTypes(2, filters);
        } else {
            const COMDLG_FILTERSPEC filters[]{{L"LRC 歌词", L"*.lrc"}, {L"所有文件", L"*.*"}};
            result = dialog->SetFileTypes(2, filters);
        }
    }
    const auto timer_window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    FolderPickerCancel state{dialog, &cancelled};
    if (timer_window) SetWindowLongPtrW(timer_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
    const auto timer = timer_window ? SetTimer(timer_window, 1, 100, cancel_folder_picker) : 0;
    if (!timer) result = E_FAIL;
    std::string selected;
    if (SUCCEEDED(result) && !cancelled.load()) result = dialog->Show(static_cast<HWND>(owner));
    if (SUCCEEDED(result) && !cancelled.load()) {
        if (multi && !folder) {
            IShellItemArray* items = nullptr;
            result = dialog->GetResults(&items);
            if (SUCCEEDED(result)) {
                DWORD count = 0;
                if (SUCCEEDED(items->GetCount(&count))) {
                    for (DWORD i = 0; i < count; ++i) {
                        IShellItem* item = nullptr;
                        if (FAILED(items->GetItemAt(i, &item))) continue;
                        PWSTR path = nullptr;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                            const std::string p = narrow(path);
                            CoTaskMemFree(path);
                            multi->push_back(p);
                            if (selected.empty()) selected = p;
                        }
                        item->Release();
                    }
                }
                items->Release();
            }
        } else {
            IShellItem* item = nullptr;
            result = dialog->GetResult(&item);
            if (SUCCEEDED(result)) {
                PWSTR path = nullptr;
                result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
                if (SUCCEEDED(result)) { selected = narrow(path); CoTaskMemFree(path); }
                item->Release();
            }
        }
    }
    if (timer) KillTimer(timer_window, timer);
    if (timer_window) DestroyWindow(timer_window);
    dialog->Release();
    CoUninitialize();
    if (FAILED(result) && result != HRESULT_FROM_WIN32(ERROR_CANCELLED) && !cancelled.load()) error = "无法打开或读取文件选择结果，请直接粘贴路径";
    return selected;
#else
    (void)owner;
    return choose_desktop_file(folder, cancelled, error, kind, multi);
#endif
}
inline std::vector<std::string> choose_audio_files(void* owner, const std::atomic<bool>& cancelled, std::string& error) {
    std::vector<std::string> files;
    choose_music_folder(owner, cancelled, error, false, 1, &files);
    return files;
}
// 音源脚本选择器（.js）
inline std::string choose_js_file(void* owner, const std::atomic<bool>& cancelled, std::string& error) {
    return choose_music_folder(owner, cancelled, error, /*folder=*/false, /*kind=*/2);
}
// 音源脚本选择器（.js，一次可选多个）
inline std::vector<std::string> choose_js_files(void* owner, const std::atomic<bool>& cancelled, std::string& error) {
    std::vector<std::string> out;
#ifdef _WIN32
    choose_music_folder(owner, cancelled, error, /*folder=*/false, /*kind=*/2, &out);
#else
    const auto single = choose_js_file(owner, cancelled, error);
    if (!single.empty()) out.push_back(single);
#endif
    return out;
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
#elif defined(__linux__)
// coreutils 是 Debian/Ubuntu 基础组件；只读取固定系统程序的 SHA-256 输出，不新增密码库依赖。
inline std::string sha256_file(const std::string& path) {
    PipeProcess process;
    if (!process.start({"/usr/bin/sha256sum", "--zero", "--", path})) return {};
    process.finish_input();
    std::string output;
    std::array<char, 512> buffer{};
    while (const auto n = process.read(buffer.data(), buffer.size())) {
        if (output.size() + n > 8192) return {};
        output.append(buffer.data(), n);
    }
    if (process.wait() != 0 || output.size() < 66 || output[64] != ' ') return {};
    const auto digest = output.substr(0, 64);
    return digest.find_first_not_of("0123456789abcdef") == std::string::npos ? digest : std::string();
}
inline bool deb_installer_available() {
    return ::access("/usr/bin/pkexec", X_OK) == 0 && ::access("/usr/bin/apt-get", X_OK) == 0;
}
// 系统授权窗口负责提权；按 argv 覆盖安装，禁止移除其他包。开始安装后不强杀 dpkg。
inline int install_deb(const std::string& path, const std::string& log_path) {
    if (!deb_installer_available()) return -1;
    std::error_code ec;
    const auto package = fs::canonical(fs::u8path(path), ec);
    if (ec || !fs::is_regular_file(package, ec) || ec) return -1;
    std::vector<std::string> args{"/usr/bin/pkexec", "--disable-internal-agent", "/usr/bin/apt-get",
        "-o", "Dpkg::Use-Pty=0", "install", "--yes", "--no-remove", "--", package.u8string()};
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) return -1;
    int error = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (!error) error = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    pid_t pid = 0;
    if (!error) error = posix_spawn(&pid, args[0].c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (error) return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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
