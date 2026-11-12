// 官网在独立外部浏览器会话登录；只连接本机调试端口，手动导入保留为备用入口。
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shlwapi.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#endif
#include "login.h"
#include "platform.hpp"
#include "json.hpp"
#include "netease_api.hpp"
#include "qqapi.hpp"
#include <array>
#include <random>
#include <thread>
#include <cctype>
#include <limits>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>

namespace login {
namespace {
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
void close_socket(Socket s) { closesocket(s); }
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
void close_socket(Socket s) { ::close(s); }
#endif
struct Connection {
    Socket socket = invalid_socket;
    ~Connection() { if (socket != invalid_socket) close_socket(socket); }
};
State states[kSiteCount];
std::array<std::atomic<bool>, kSiteCount> cancelled{};
std::array<std::thread, kSiteCount> workers;
std::mutex worker_mutex;
// 仅输出阶段和错误说明，不输出 Cookie；用于隔离浏览器回归的故障诊断。
void trace(const std::string& text) {
    if (platform::env("AINO_LOGIN_TRACE") == "1") { std::fprintf(stderr, "login: %s\n", text.c_str()); std::fflush(stderr); }
}
std::string token() {
    std::random_device rd;
    std::string value;
    const char* hex = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) { const auto b = rd(); value += hex[b & 15]; value += hex[(b >> 4) & 15]; }
    return value;
}
void note(Site site, const std::string& text) {
    trace(text);
    auto& s = state(site);
    std::lock_guard<std::mutex> lock(s.m); s.note = text;
}
bool ready(Socket fd, int milliseconds) {
    fd_set reads; FD_ZERO(&reads); FD_SET(fd, &reads);
    timeval timeout{milliseconds / 1000, (milliseconds % 1000) * 1000};
    return select(int(fd + 1), &reads, nullptr, nullptr, &timeout) > 0;
}
bool send_all(Socket fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
#ifdef _WIN32
        const int n = ::send(fd, data.data() + sent, int(data.size() - sent), 0);
#else
        const auto n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
#endif
        if (n <= 0) return false;
        sent += size_t(n);
    }
    return true;
}
void respond(Socket fd, int status, const std::string& body, const std::string& type = "text/plain; charset=utf-8") {
    const auto headers = "HTTP/1.1 " + std::to_string(status) + " Response\r\nContent-Type: " + type +
        "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
        "X-Frame-Options: DENY\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    send_all(fd, headers + body);
}
struct HttpRequest { std::string method, path, host, origin, type, body; };
bool read_request(Socket fd, HttpRequest& out) {
    std::string data;
    size_t split = std::string::npos;
    const auto deadline = platform::ticks() + 4000;
    auto read_more = [&]() {
        while (!ready(fd, 100)) if (platform::ticks() >= deadline) return false;
        if (platform::ticks() >= deadline) return false;
        char buffer[4096];
        const auto n = recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) return false;
        data.append(buffer, size_t(n));
        return data.size() <= 96 * 1024;
    };
    while ((split = data.find("\r\n\r\n")) == std::string::npos) {
        if (!read_more()) return false;
        if (data.size() > 16 * 1024) return false;
    }
    const auto first = data.find("\r\n");
    const auto space = data.find(' '), second = data.find(' ', space + 1);
    if (space == std::string::npos || second == std::string::npos || second > first) return false;
    out.method = data.substr(0, space); out.path = data.substr(space + 1, second - space - 1);
    size_t length = 0;
    bool has_length = false;
    for (size_t pos = first + 2; pos < split;) {
        const auto end = data.find("\r\n", pos);
        const auto colon = data.find(':', pos);
        if (colon == std::string::npos || colon > end) return false;
        auto name = data.substr(pos, colon - pos);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        auto value = music::text::trim(data.substr(colon + 1, end - colon - 1));
        if (name == "host") { if (!out.host.empty()) return false; out.host = value; }
        else if (name == "origin") out.origin = value;
        else if (name == "content-type") out.type = value;
        else if (name == "transfer-encoding") return false;
        else if (name == "content-length") {
            if (has_length || value.empty() || value.find_first_not_of("0123456789") != std::string::npos) return false;
            try { length = std::stoul(value); } catch (...) { return false; }
            if (length > 64 * 1024) return false;
            has_length = true;
        }
        pos = end + 2;
    }
    if (out.method == "POST" && !has_length) return false;
    while (data.size() < split + 4 + length) if (!read_more()) return false;
    out.body = data.substr(split + 4, length);
    return true;
}
std::string json_string(const std::string& value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c >= 32) out += char(c);
    }
    return out + '"';
}
const char* official_url(Site site) { return site == Site::QQ ? "https://y.qq.com/" : "https://music.163.com/"; }

// 只连接自己启动的独立浏览器会话，不读取用户日常浏览器的配置或 Cookie 数据库。
std::string default_browser() {
#ifdef _WIN32
    wchar_t executable[32768]{};
    DWORD size = DWORD(std::size(executable));
    if (SUCCEEDED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE, L"https", L"open", executable, &size)))
        return platform::narrow(executable);
    return {};
#else
    // 命令固定，不拼接用户输入；桌面文件只用于解析默认浏览器可执行文件。
    std::string desktop;
    if (auto* pipe = popen("xdg-settings get default-web-browser 2>/dev/null", "r")) {
        char text[512]{};
        if (std::fgets(text, sizeof(text), pipe)) desktop = music::text::trim(text);
        pclose(pipe);
    }
    if (desktop.empty() || desktop.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos) return {};
    std::vector<std::string> directories;
    auto home = platform::env("XDG_DATA_HOME");
    if (home.empty()) home = platform::join(platform::env("HOME"), ".local/share");
    directories.push_back(home);
    auto system = platform::env("XDG_DATA_DIRS");
    if (system.empty()) system = "/usr/local/share:/usr/share";
    std::istringstream paths(system);
    for (std::string path; std::getline(paths, path, ':');) directories.push_back(path);
    for (const auto& directory : directories) {
        std::ifstream input(platform::fs::u8path(platform::join(platform::join(directory, "applications"), desktop)));
        for (std::string line; std::getline(input, line);) if (line.rfind("Exec=", 0) == 0) {
            std::istringstream command(line.substr(5));
            std::string executable;
            if (command.peek() == '"') { command.get(); std::getline(command, executable, '"'); }
            else command >> executable;
            return executable;
        }
    }
    return {};
#endif
}
bool chromium_browser(const std::string& path) {
    auto name = platform::fs::u8path(path).filename().u8string();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char* supported : {"chrome.exe", "msedge.exe", "brave.exe", "vivaldi.exe", "chrome", "google-chrome", "google-chrome-stable", "chromium", "chromium-browser", "microsoft-edge", "microsoft-edge-stable", "brave-browser", "vivaldi", "vivaldi-stable"})
        if (name == supported) return true;
    return false;
}
struct BrowserProcess {
#ifdef _WIN32
    HANDLE handle = nullptr;
    HANDLE job = nullptr;
#else
    pid_t pid = 0;
    pid_t group = 0;
#endif
    bool launch(const std::vector<std::string>& args) {
#ifdef _WIN32
        std::wstring command;
        for (const auto& argument : args) {
            if (!command.empty()) command += L' ';
            command += L'"';
            size_t slashes = 0;
            for (wchar_t c : platform::widen(argument)) {
                if (c == L'\\') { ++slashes; continue; }
                if (c == L'"') command.append(slashes * 2 + 1, L'\\');
                else command.append(slashes, L'\\');
                slashes = 0; command += c;
            }
            command.append(slashes * 2, L'\\'); command += L'"';
        }
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const auto executable = platform::widen(args.front());
        job = CreateJobObjectW(nullptr, nullptr);
        if (!job) return false;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return false;
        // 挂起时加入独立 Job，确保登录窗口及其子进程均归本次尝试所有。
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process)) return false;
        handle = process.hProcess;
        if (!AssignProcessToJobObject(job, handle)) {
            TerminateProcess(handle, 1); WaitForSingleObject(handle, 2000);
            CloseHandle(process.hThread); return false;
        }
        const bool resumed = ResumeThread(process.hThread) != DWORD(-1);
        CloseHandle(process.hThread); return resumed;
#else
        std::vector<char*> argv;
        for (const auto& argument : args) argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        posix_spawnattr_t attributes;
        if (posix_spawnattr_init(&attributes)) return false;
        const bool configured = posix_spawnattr_setpgroup(&attributes, 0) == 0 &&
                                posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP) == 0;
        const int result = configured ? posix_spawnp(&pid, argv[0], nullptr, &attributes, argv.data(), environ) : -1;
        posix_spawnattr_destroy(&attributes);
        if (result) { pid = 0; return false; }
        group = pid; return true;
#endif
    }
    bool running() {
#ifdef _WIN32
        return handle && WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
#else
        if (!pid) return false;
        const auto result = waitpid(pid, nullptr, WNOHANG);
        if (result == pid || (result < 0 && errno == ECHILD)) { pid = 0; return false; }
        return true;
#endif
    }
    void stop() {
        trace("stopping owned browser processes");
        // Browser.close 异步关闭；先等本次启动的进程退出，再兜底终止。
        for (int i = 0; i < 20 && running(); ++i) platform::sleep_ms(50);
#ifdef _WIN32
        if (running()) { TerminateProcess(handle, 0); WaitForSingleObject(handle, 2000); }
#else
        // 独立进程组也覆盖浏览器启动脚本和仍存活的子进程，绝不向播放器的进程组发信号。
        if (group > 0) {
            kill(-group, SIGTERM);
            for (int i = 0; i < 20 && kill(-group, 0) == 0; ++i) platform::sleep_ms(50);
            if (kill(-group, 0) == 0) kill(-group, SIGKILL);
            group = 0;
        }
        if (running()) { kill(pid, SIGKILL); waitpid(pid, nullptr, 0); pid = 0; }
#endif
#ifdef _WIN32
        if (job) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
            for (int i = 0; i < 20; ++i) {
                if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr) || !accounting.ActiveProcesses) break;
                platform::sleep_ms(50);
            }
            TerminateJobObject(job, 0);
            for (int i = 0; i < 40; ++i) {
                if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr) || !accounting.ActiveProcesses) break;
                platform::sleep_ms(50);
            }
            CloseHandle(job); job = nullptr;
        }
        if (handle) { CloseHandle(handle); handle = nullptr; }
#endif
        trace("owned browser processes stopped");
    }
    ~BrowserProcess() { stop(); }
};
std::string base64(const unsigned char* data, size_t size) {
    const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    for (size_t at = 0; at < size; at += 3) {
        const uint32_t bits = uint32_t(data[at]) << 16 | (at + 1 < size ? uint32_t(data[at + 1]) << 8 : 0) | (at + 2 < size ? data[at + 2] : 0);
        output += alphabet[(bits >> 18) & 63]; output += alphabet[(bits >> 12) & 63];
        output += at + 1 < size ? alphabet[(bits >> 6) & 63] : '=';
        output += at + 2 < size ? alphabet[bits & 63] : '=';
    }
    return output;
}
// SHA-1 仅用于校验本机 WebSocket 握手，不承担凭据加密职责。
std::string websocket_accept(const std::string& key) {
    std::string input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const uint64_t bits = uint64_t(input.size()) * 8;
    input += char(0x80);
    while (input.size() % 64 != 56) input += '\0';
    for (int i = 7; i >= 0; --i) input += char(bits >> (i * 8));
    uint32_t hash[] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    const auto rotate = [](uint32_t value, int count) { return value << count | value >> (32 - count); };
    for (size_t block = 0; block < input.size(); block += 64) {
        uint32_t words[80]{};
        for (int i = 0; i < 16; ++i) for (int j = 0; j < 4; ++j) words[i] = words[i] << 8 | uint8_t(input[block + i * 4 + j]);
        for (int i = 16; i < 80; ++i) words[i] = rotate(words[i-3] ^ words[i-8] ^ words[i-14] ^ words[i-16], 1);
        uint32_t a = hash[0], b = hash[1], c = hash[2], d = hash[3], e = hash[4];
        for (int i = 0; i < 80; ++i) {
            const uint32_t f = i < 20 ? (b & c) | (~b & d) : i < 40 ? b ^ c ^ d : i < 60 ? (b & c) | (b & d) | (c & d) : b ^ c ^ d;
            const uint32_t k = i < 20 ? 0x5a827999 : i < 40 ? 0x6ed9eba1 : i < 60 ? 0x8f1bbcdc : 0xca62c1d6;
            const uint32_t next = rotate(a, 5) + f + e + k + words[i];
            e = d; d = c; c = rotate(b, 30); b = a; a = next;
        }
        hash[0] += a; hash[1] += b; hash[2] += c; hash[3] += d; hash[4] += e;
    }
    unsigned char digest[20];
    for (int i = 0; i < 20; ++i) digest[i] = uint8_t(hash[i / 4] >> (24 - i % 4 * 8));
    return base64(digest, sizeof(digest));
}
class BrowserConnection {
    Connection connection_;
    int sequence_ = 0;
    bool healthy_ = true;
    bool read_bytes(char* target, size_t size, unsigned long long deadline, const std::atomic<bool>& cancel) {
        size_t received = 0;
        while (received < size && !cancel.load() && platform::ticks() < deadline) {
            if (!ready(connection_.socket, 100)) continue;
            const auto count = recv(connection_.socket, target + received, int(size - received), 0);
            if (count <= 0) return false;
            received += size_t(count);
        }
        return received == size;
    }
    bool send_frame(unsigned char opcode, const std::string& message) {
        std::string frame(1, char(0x80 | opcode));
        if (message.size() < 126) frame += char(0x80 | message.size());
        else if (message.size() <= 65535) { frame += char(0xfe); frame += char(message.size() >> 8); frame += char(message.size()); }
        else { frame += char(0xff); for (int i = 7; i >= 0; --i) frame += char(uint64_t(message.size()) >> (i * 8)); }
        std::random_device random;
        unsigned char mask[4];
        for (auto& byte : mask) { byte = uint8_t(random()); frame += char(byte); }
        for (size_t i = 0; i < message.size(); ++i) frame += char(uint8_t(message[i]) ^ mask[i % 4]);
        return send_all(connection_.socket, frame);
    }
    bool read_message(std::string& message, const std::atomic<bool>& cancel, unsigned long long deadline) {
        message.clear();
        for (;;) {
            unsigned char header[2];
            if (!read_bytes(reinterpret_cast<char*>(header), 2, deadline, cancel)) return false;
            uint64_t length = header[1] & 127;
            if (length >= 126) {
                unsigned char extra[8]{}; const int count = length == 126 ? 2 : 8;
                if (!read_bytes(reinterpret_cast<char*>(extra), count, deadline, cancel)) return false;
                length = 0; for (int i = 0; i < count; ++i) length = length << 8 | extra[i];
            }
            if (length > 2 * 1024 * 1024 || message.size() + length > 2 * 1024 * 1024) return false;
            unsigned char mask[4]{};
            if ((header[1] & 128) && !read_bytes(reinterpret_cast<char*>(mask), 4, deadline, cancel)) return false;
            std::string payload(size_t(length), '\0');
            if (!read_bytes(payload.data(), payload.size(), deadline, cancel)) return false;
            if (header[1] & 128) for (size_t i = 0; i < payload.size(); ++i) payload[i] = char(uint8_t(payload[i]) ^ mask[i % 4]);
            const auto opcode = header[0] & 15;
            if (opcode == 8) return false;
            if (opcode == 9) { if (!send_frame(10, payload)) return false; continue; }
            if (opcode == 10) continue;
            if (opcode != 0 && opcode != 1) return false;
            message += payload;
            if (header[0] & 128) return true;
        }
    }
public:
    bool connect(int port, const std::string& path, const std::atomic<bool>& cancel) {
        if (port < 1 || port > 65535 || path.rfind("/devtools/browser/", 0) != 0 || path.find_first_of("\r\n ") != std::string::npos) return false;
        connection_.socket = socket(AF_INET, SOCK_STREAM, 0);
        if (connection_.socket == invalid_socket) return false;
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(uint16_t(port)); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(connection_.socket, reinterpret_cast<sockaddr*>(&address), sizeof(address))) return false;
#ifdef _WIN32
        DWORD timeout = 3000;
        setsockopt(connection_.socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        timeval timeout{3, 0}; setsockopt(connection_.socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
        std::random_device random; unsigned char bytes[16]; for (auto& byte : bytes) byte = uint8_t(random());
        const auto key = base64(bytes, sizeof(bytes));
        if (!send_all(connection_.socket, "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n\r\n")) return false;
        std::string response;
        const auto deadline = platform::ticks() + 5000;
        while (response.size() < 16384 && response.find("\r\n\r\n") == std::string::npos) {
            char next;
            if (!read_bytes(&next, 1, deadline, cancel)) return false;
            response += next;
        }
        if (response.rfind("HTTP/1.1 101 ", 0) != 0) return false;
        std::istringstream headers(response);
        for (std::string line; std::getline(headers, line);) {
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            auto name = line.substr(0, colon);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (name == "sec-websocket-accept") return music::text::trim(line.substr(colon + 1)) == websocket_accept(key);
        }
        return false;
    }
    bool call(const std::string& method, const std::string& parameters, js::Value& result, const std::atomic<bool>& cancel, const std::string& session = {}) {
        const int id = ++sequence_;
        if (!send_frame(1, "{\"id\":" + std::to_string(id) + ",\"method\":" + json_string(method) + ",\"params\":" + parameters + (session.empty() ? "" : ",\"sessionId\":" + json_string(session)) + "}")) { healthy_ = false; return false; }
        const auto deadline = platform::ticks() + 5000;
        for (int i = 0; i < 256; ++i) {
            std::string message; js::Value root;
            if (!read_message(message, cancel, deadline) || !js::parse(message, root)) { healthy_ = false; return false; }
            if (root["id"].integer(-1) != id) continue;
            if (!root["error"].is_null()) return false;
            result = root["result"]; return true;
        }
        return false;
    }
    bool healthy() const { return healthy_; }
    void close_browser() {
        trace("requesting Browser.close");
        send_frame(1, "{\"id\":999999,\"method\":\"Browser.close\",\"params\":{}}");
        trace("Browser.close sent");
    }
};
bool browser_credentials(Site site, const js::Value& result, std::string& cookie, std::string& uid) {
    if (!result["cookies"].is_array()) return false;
    cookie.clear();
    for (const auto& item : result["cookies"].array) {
        auto domain = item["domain"].str();
        if (!domain.empty() && domain.front() == '.') domain.erase(0, 1);
        // Network.getCookies 已按官网 URL 限定，额外校验域名，防止伪造同名 Cookie。
        const bool allowed = site == Site::QQ ? domain == "qq.com" || domain == "y.qq.com" : domain == "music.163.com" || domain == "163.com";
        const auto name = item["name"].str(), value = item["value"].str();
        if (!allowed || name.empty() || name.find_first_of(";=\r\n\0", 0, 5) != std::string::npos || value.find_first_of(";\r\n\0", 0, 4) != std::string::npos) continue;
        if (!cookie.empty()) cookie += "; "; cookie += name + "=" + value;
    }
    std::string accepted, error;
    if (!parse_credentials(site, "{\"cookie\":" + json_string(cookie) + "}", accepted, uid, error)) { cookie.clear(); return false; }
    cookie = std::move(accepted); return true;
}
void run_browser(Site site, BrowserOptions options) {
    const auto executable = options.executable.empty() ? default_browser() : options.executable;
    if (!chromium_browser(executable)) { note(site, "默认浏览器暂不支持自动回传；请将 Chrome、Edge 或 Chromium 设为默认浏览器后重试"); return; }
#ifdef _WIN32
    static const bool initialized = [] { WSADATA data; return WSAStartup(MAKEWORD(2,2), &data) == 0; }();
    if (!initialized) { note(site, "无法初始化官网登录连接"); return; }
#endif
    const auto parent = platform::temp_dir();
    if (parent.empty()) { note(site, "无法创建浏览器临时登录目录"); return; }
    const auto profile = platform::join(parent, "browser-auth-" + token());
    if (!platform::make_dirs(profile)) { note(site, "无法创建浏览器临时登录目录"); return; }
    // 目录名由本进程生成；析构只清除此目录，不处理浏览器的正常用户配置。
    struct ProfileCleanup {
        std::string path;
        ~ProfileCleanup() {
            trace("removing owned browser profile");
            auto directory = platform::fs::u8path(path);
#ifdef _WIN32
            // Chromium 的组件文件名可能使完整路径超过 MAX_PATH。
            directory.make_preferred();
            const auto native = directory.native();
            if (native.rfind(L"\\\\?\\", 0) != 0)
                directory = native.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + native.substr(2) : L"\\\\?\\" + native;
#endif
            // Windows 上浏览器主进程退出后，子进程仍可能短暂持有 profile 文件锁。
            for (int i = 0; i < 40; ++i) {
                std::error_code error;
                platform::fs::remove_all(directory, error);
                if (!error) { trace("owned browser profile removed"); return; }
                platform::sleep_ms(100);
            }
        }
    } cleanup{profile};
    BrowserProcess browser;
    std::vector<std::string> arguments = {executable, "--user-data-dir=" + profile, "--remote-debugging-address=127.0.0.1", "--remote-debugging-port=0", "--no-first-run", "--no-default-browser-check", "--disable-background-mode", "--new-window"};
    if (options.headless) { arguments.push_back("--headless=new"); arguments.push_back("--disable-gpu"); arguments.push_back("--disable-background-networking"); }
    arguments.push_back(options.headless ? "about:blank" : official_url(site));
    if (!browser.launch(arguments)) { note(site, "无法启动默认浏览器的独立登录窗口"); return; }
    auto& cancel = cancelled[int(site)];
    auto& s = state(site);
    note(site, "已打开官网，请在网页中点击登录，使用官网支持的扫码或密码方式完成登录");
    const auto startup_deadline = platform::ticks() + 15000;
    int port = 0; std::string websocket_path;
    while (!cancel.load() && platform::ticks() < startup_deadline) {
        std::ifstream input(platform::fs::u8path(platform::join(profile, "DevToolsActivePort")));
        std::string number;
        if (std::getline(input, number) && std::getline(input, websocket_path)) {
            try { port = std::stoi(number); } catch (...) { port = 0; }
            if (port > 0 && port <= 65535) break;
        }
        platform::sleep_ms(100);
    }
    BrowserConnection connection;
    if (!port || !connection.connect(port, websocket_path, cancel)) { note(site, cancel.load() ? "登录已取消" : "浏览器登录回传连接失败，请重试"); return; }
    struct CloseBrowser { BrowserConnection& connection; ~CloseBrowser() { connection.close_browser(); } } close{connection};
    const auto endpoint = "ws://127.0.0.1:" + std::to_string(port) + websocket_path;
    if (options.on_ready) options.on_ready(endpoint);
    trace("official browser connection ready");
    const auto deadline = platform::ticks() + 300000;
    std::string session;
    int retries = 0;
    while (!cancel.load() && platform::ticks() < deadline) {
        js::Value result;
        if (session.empty()) {
            if (!connection.call("Target.getTargets", "{}", result, cancel)) break;
            std::string target;
            for (const auto& item : result["targetInfos"].array) if (item["type"].str() == "page") { target = item["targetId"].str(); break; }
            if (target.empty()) { platform::sleep_ms(100); continue; }
            if (!connection.call("Target.attachToTarget", "{\"targetId\":" + json_string(target) + ",\"flatten\":true}", result, cancel)) {
                if (!connection.healthy() || ++retries > 5) break;
                platform::sleep_ms(100); continue;
            }
            session = result["sessionId"].str();
            if (session.empty()) break;
        }
        if (!connection.call("Network.getCookies", "{\"urls\":[" + json_string(official_url(site)) + "]}", result, cancel, session)) {
            if (!connection.healthy() || ++retries > 5) break;
            session.clear(); platform::sleep_ms(100); continue;
        }
        retries = 0;
        std::string cookie, uid;
        if (browser_credentials(site, result, cookie, uid)) {
            trace("official credentials received");
            std::lock_guard<std::mutex> lock(s.m);
            if (cancel.load()) return;
            s.cookie = std::move(cookie); s.uid = std::move(uid); s.note = "官网登录成功，已返回播放器";
            s.return_requested.store(true); s.done.store(true); return;
        }
        for (int i = 0; i < 10 && !cancel.load(); ++i) platform::sleep_ms(100);
    }
    note(site, cancel.load() ? "登录已取消" : platform::ticks() >= deadline ? "官网登录等待超时，请重试" : "官网登录窗口已关闭或连接中断，请重试");
}
std::string page(Site site) {
    const std::string name = site == Site::QQ ? "QQ 音乐" : "网易云音乐";
    return R"HTML(<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'; base-uri 'none'; form-action 'none'">
<title>aino player 官网登录</title><style>body{font:16px system-ui,sans-serif;background:#f5f5f7;color:#222;max-width:620px;margin:40px auto;padding:24px}section{background:white;padding:28px;border-radius:16px}button,a{display:inline-block;margin:8px 12px 8px 0;padding:10px;border-radius:8px}button{cursor:pointer}textarea{box-sizing:border-box;width:100%;padding:12px;font:14px system-ui;height:120px}p{line-height:1.7}#status{white-space:pre-wrap}</style><section>
<h1>)HTML" + name + R"HTML(官网登录</h1><p>正常登录会直接打开默认浏览器的专用官网窗口，由官网处理扫码、密码和验证；支持 Chrome、Edge 和 Chromium。</p>
<p>此页仅用于不支持自动回传时的备用导入。</p><a id="official" target="_blank" rel="noreferrer" href=")HTML" + official_url(site) + R"HTML(">前往官网登录</a>
<p>登录后，从开发者工具 Network 中复制官网请求的 Cookie 值。</p><textarea id="cookie" autocomplete="off" spellcheck="false" placeholder="完整 Cookie 值（只发送到本机播放器）"></textarea><button id="import">导入并验证</button>
<button id="cancel">取消连接</button><button id="return" hidden>返回播放器</button><p id="status" role="status">等待导入登录信息</p></section>
<script>
const path=location.pathname,status=document.getElementById('status');let finished=false;
async function post(endpoint,body={}){const r=await fetch(path+'/'+endpoint,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});if(!r.ok)throw Error(await r.text());return r}
function success(){finished=true;status.textContent='登录成功，已通知播放器返回前台';document.getElementById('return').hidden=false;document.getElementById('import').disabled=true;document.getElementById('cancel').hidden=true}
async function poll(){if(finished)return;try{const data=await(await fetch(path+'/status')).json();status.textContent=data.note;if(data.done){success();return}}catch(e){status.textContent='连接已结束，请返回播放器查看结果';return}setTimeout(poll,1200)}
document.getElementById('return').onclick=async()=>{try{await post('return')}catch(e){status.textContent='请从任务栏打开播放器'}};
document.getElementById('cancel').onclick=async()=>{finished=true;try{await post('cancel');status.textContent='已取消连接'}catch(e){status.textContent='连接已结束'}};
document.getElementById('import').onclick=async()=>{const input=document.getElementById('cookie');try{await post('cookie',{cookie:input.value});input.value='';success()}catch(e){status.textContent=e.message}};poll();
</script></html>)HTML";
}
void run_manual(Site site, bool launch_browser) {
    auto& s = state(site);
    const int idx = int(site);
#ifdef _WIN32
    static const bool initialized = [] { WSADATA data; return WSAStartup(MAKEWORD(2,2), &data) == 0; }();
    if (!initialized) { note(site, "无法初始化本机登录连接"); return; }
#endif
    Connection server;
    server.socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server.socket == invalid_socket) { note(site, "无法创建本机登录连接"); return; }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server.socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(server.socket, 4)) {
        note(site, "本机登录连接被系统拒绝"); return;
    }
#ifdef _WIN32
    int size = sizeof(address);
#else
    socklen_t size = sizeof(address);
#endif
    if (getsockname(server.socket, reinterpret_cast<sockaddr*>(&address), &size)) return;
    const std::string host = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    const std::string origin = "http://" + host;
    const std::string path = "/session/" + token();
    const auto url = origin + path;
    { std::lock_guard<std::mutex> lock(s.m); s.portal_url = url; s.note = "等待备用导入登录信息（5 分钟内有效）"; }
    if (launch_browser && !platform::open_browser(url)) { note(site, "无法打开系统默认浏览器，请检查默认浏览器设置"); return; }
    const auto deadline = platform::ticks() + 300000;
    bool completed = false;
    unsigned long long complete_deadline = 0;
    while (!cancelled[idx].load() && platform::ticks() < deadline) {
        if (completed && platform::ticks() > complete_deadline) return;
        if (!ready(server.socket, 200)) continue;
        Connection client;
        client.socket = accept(server.socket, nullptr, nullptr);
        if (client.socket == invalid_socket) continue;
        HttpRequest req;
        if (!read_request(client.socket, req)) { respond(client.socket, 400, "无效请求"); continue; }
        const auto route = req.path.substr(0, req.path.find('?'));
        const bool session_route = route == path || route == path + "/cookie" || route == path + "/cancel" ||
            route == path + "/status" || route == path + "/return";
        if (req.host != host || !session_route) { respond(client.socket, 403, "登录地址无效"); continue; }
        if (req.method == "GET") {
            if (route == path) respond(client.socket, 200, page(site), "text/html; charset=utf-8");
            else if (route == path + "/status") {
                std::string text;
                { std::lock_guard<std::mutex> lock(s.m); text = s.note; }
                respond(client.socket, 200, "{\"done\":" + std::string(completed ? "true" : "false") + ",\"note\":" + json_string(text) + "}", "application/json");
            } else respond(client.socket, 404, "无效路径");
            continue;
        }
        if (req.method != "POST" || req.origin != origin || req.type != "application/json") {
            respond(client.socket, 403, "请求来源或类型无效"); continue;
        }
        if (route == path + "/return") {
            if (!completed) { respond(client.socket, 409, "请先完成登录"); continue; }
            {
                std::lock_guard<std::mutex> lock(s.m);
                if (!cancelled[idx].load()) s.return_requested.store(true);
            }
            respond(client.socket, 200, "已通知播放器返回前台"); continue;
        }
        if (route == path + "/cancel") { respond(client.socket, 200, "已取消"); cancelled[idx].store(true); break; }
        if (route != path + "/cookie") { respond(client.socket, 404, "无效路径"); continue; }
        if (completed) { respond(client.socket, 409, "本次登录已成功，请返回播放器"); continue; }
        std::string cookie, uid, error;
        if (!parse_credentials(site, req.body, cookie, uid, error)) { respond(client.socket, 400, error); continue; }
        note(site, "正在向官网验证登录态…");
        if (site == Site::QQ) {
            const auto profile = qq::fetch_profile(cookie, uid, 6000);
            if (!profile.ok()) error = "QQ 音乐登录态验证失败，请重新登录或导入完整 Cookie";
        } else {
            const auto profile = ne::fetch_profile(cookie, {}, 6000);
            if (!profile.ok() || profile.uid.empty()) error = "网易云登录态验证失败，请重新登录或导入完整 Cookie";
            else uid = profile.uid;
        }
        if (!error.empty()) { note(site, error); respond(client.socket, 401, error); continue; }
        if (cancelled[idx].load()) break;
        {
            std::lock_guard<std::mutex> lock(s.m);
            if (cancelled[idx].load()) return;
            s.cookie = cookie; s.uid = uid; s.note = "连接成功，可以返回播放器";
            s.return_requested.store(true); s.done.store(true);
        }
        completed = true; complete_deadline = platform::ticks() + 60000;
        respond(client.socket, 200, "连接成功，可以关闭本页并返回播放器");
    }
    note(site, cancelled[idx].load() ? "登录已取消" : "登录连接已超时，请重试");
}
} // namespace
State& state(Site s) { return states[int(s)]; }
bool parse_credentials(Site site, const std::string& input, std::string& cookie, std::string& uid, std::string& error) {
    cookie.clear(); uid.clear(); error.clear();
    js::Value root;
    if (input.size() > 64 * 1024 || !js::parse(input, root) || !root.is_object()) { error = "无效的登录数据"; return false; }
    cookie = music::text::trim(root["cookie"].str());
    if (cookie.rfind("Cookie:", 0) == 0 || cookie.rfind("cookie:", 0) == 0) cookie = music::text::trim(cookie.substr(7));
    if (cookie.empty() || cookie.size() > 32768 || std::any_of(cookie.begin(), cookie.end(), [](unsigned char c) { return c < 32 || c == 127; })) {
        error = "请粘贴完整 Cookie 值，不要包含换行"; cookie.clear(); return false;
    }
    if (site == Site::NetEase) {
        if (ne::cookie_value(cookie, "MUSIC_U").empty()) { error = "缺少 MUSIC_U，请先在官网登录"; return false; }
    } else {
        uid = qq::cookie_value(cookie, "uin");
        if (uid.empty()) uid = qq::cookie_value(cookie, "p_uin");
        if (uid.empty()) uid = qq::cookie_value(cookie, "wxuin");
        uid = qq::clean_uin(uid);
        const bool key = !qq::cookie_value(cookie, "qm_keyst").empty() || !qq::cookie_value(cookie, "qqmusic_key").empty() ||
            !qq::cookie_value(cookie, "musickey").empty() || !qq::cookie_value(cookie, "authst").empty();
        if (uid.empty() || uid.find_first_not_of("0123456789") != std::string::npos || !key) {
            error = "缺少 QQ 音乐账号或登录凭据，请先在官网登录"; return false;
        }
    }
    return true;
}
static void begin_work(Site site, std::function<void()> action) {
    std::lock_guard<std::mutex> lock(worker_mutex);
    auto& s = state(site);
    if (s.running.load() && !s.done.load()) return;
    if (s.running.load()) cancelled[int(site)].store(true);
    if (workers[int(site)].joinable()) workers[int(site)].join();
    s.running.store(true);
    cancelled[int(site)].store(false); s.done.store(false); s.return_requested.store(false);
    { std::lock_guard<std::mutex> guard(s.m); s.cookie.clear(); s.uid.clear(); s.portal_url.clear(); s.note = "正在打开系统默认浏览器官网…"; }
    workers[int(site)] = std::thread([site, action = std::move(action)] {
        try { action(); } catch (...) { note(site, "官网登录连接失败，请重试"); }
        auto& s = state(site); s.running.store(false); s.done.store(true);
    });
}
void start(Site site, bool launch_browser) {
    if (launch_browser) start_browser(site, {});
    else begin_work(site, [site] { run_manual(site, false); });
}
void start_browser(Site site, BrowserOptions options) {
    begin_work(site, [site, options = std::move(options)] { run_browser(site, options); });
}
void cancel(Site site) { cancelled[int(site)].store(true); }
void forget(Site site) {
    cancelled[int(site)].store(true);
    auto& s = state(site);
    std::lock_guard<std::mutex> lock(s.m);
    s.cookie.clear(); s.uid.clear(); s.portal_url.clear(); s.note = "已清除登录信息";
    s.return_requested.store(false);
}
void shutdown() {
    std::lock_guard<std::mutex> lock(worker_mutex);
    for (auto& c : cancelled) c.store(true);
    for (auto& w : workers) if (w.joinable()) w.join();
}
} // namespace login
