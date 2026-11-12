// net.hpp —— 基于 WinHTTP 的最小 HTTP 客户端（只用系统库，无第三方依赖）
//
// 提供四件事：
//   get / post        —— 小响应（JSON 接口），整段读进内存
//   probe_audio       —— 只取前 2 字节判断"这条直链到底能不能出声"
//   download          —— 带进度、可取消的流式落盘
//   url_encode        —— 查询参数编码（中文关键词必须）
#pragma once

// min/max 宏会和 <algorithm> 打架，必须在 windows.h 之前关掉
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <cstddef>
#include <cstring>
#include <array>
#include <cstdint>
#include <string>
#include <functional>
#include <atomic>
#include <algorithm>
namespace net {
inline const std::atomic<bool>* shutdown_signal = nullptr;
inline thread_local std::function<bool()> cancel_current;
inline bool cancelled() { return (shutdown_signal && shutdown_signal->load()) || (cancel_current && cancel_current()); }
struct CancelScope {
    std::function<bool()> previous;
    explicit CancelScope(std::function<bool()> signal) : previous(std::move(cancel_current)) { cancel_current = std::move(signal); }
    CancelScope(const CancelScope&) = delete;
    CancelScope& operator=(const CancelScope&) = delete;
    ~CancelScope() { cancel_current = std::move(previous); }
};
// YouTube 请求签名和本机 WebSocket 握手共用 SHA-1；不用于密码存储。
inline std::array<unsigned char, 20> sha1(std::string input) {
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
    std::array<unsigned char, 20> digest{};
    for (int i = 0; i < 20; ++i) digest[size_t(i)] = uint8_t(hash[i / 4] >> (24 - i % 4 * 8));
    return digest;
}
inline std::string sha1_hex(const std::string& input) {
    const auto digest = sha1(input);
    const char* hex = "0123456789abcdef";
    std::string out;
    for (auto byte : digest) { out += hex[byte >> 4]; out += hex[byte & 15]; }
    return out;
}
// 判断容器头而非只认 MP3；DASH 音轨通常是 M4A，YouTube 也可能返回 WebM。
inline bool audio_prefix(const char* bytes, size_t size) {
    if (size >= 3 && !std::memcmp(bytes, "ID3", 3)) return true;
    if (size >= 2 && static_cast<unsigned char>(bytes[0]) == 0xff &&
        (static_cast<unsigned char>(bytes[1]) & 0xe0) == 0xe0) return true;
    if (size >= 4 && (!std::memcmp(bytes, "fLaC", 4) || !std::memcmp(bytes, "OggS", 4) ||
        !std::memcmp(bytes, "\x1a\x45\xdf\xa3", 4))) return true;
    if (size >= 8 && !std::memcmp(bytes + 4, "ftyp", 4)) return true;
    return size >= 12 && !std::memcmp(bytes, "RIFF", 4) && !std::memcmp(bytes + 8, "WAVE", 4);
}
}

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace net {

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &out[0], n);
    return out;
}

inline std::string narrow(const std::wstring& s) {
    if (s.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), &out[0], n, nullptr, nullptr);
    return out;
}

inline std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            out += char(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

struct Response {
    int status = 0;
    std::string body;
    std::string content_type;
    std::string final_url;
    std::string error;   // 非空 = 传输层失败
    std::vector<std::string> set_cookies;
    std::string location;
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

struct Request {
    std::string url;
    std::string body;                 // 非空则用 POST
    std::vector<std::string> headers; // "Key: Value"
    int timeout_ms = 8000;
    bool follow_redirects = true;
};

namespace detail {
// WinHTTP 只由所属线程发起操作和关闭；回调只交付完成事件。
// 等到 HANDLE_CLOSING 才释放请求上下文和读写缓冲，取消不会遗留悬空回调。
struct RequestControl {
    HINTERNET request = nullptr;
    RequestControl* previous = nullptr;
    std::mutex mutex;
    std::condition_variable cv;
    DWORD completed = 0, value = 0, failure = 0;
    bool closing = false;
    RequestControl();
    ~RequestControl();
    static void CALLBACK callback(HINTERNET, DWORD_PTR context, DWORD status, void* data, DWORD size) {
        auto* self = reinterpret_cast<RequestControl*>(context);
        if (!self) return;
        std::lock_guard<std::mutex> lock(self->mutex);
        if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) self->closing = true;
        else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR && size >= sizeof(WINHTTP_ASYNC_RESULT))
            self->failure = static_cast<WINHTTP_ASYNC_RESULT*>(data)->dwError;
        else if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE || status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE ||
                 status == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE || status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
            self->completed = status;
            if (status == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE && size >= sizeof(DWORD)) self->value = *static_cast<DWORD*>(data);
            else if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) self->value = size;
        }
        self->cv.notify_all();
    }
    bool bind(HINTERNET handle) {
        request = handle;
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(this);
        if (!WinHttpSetOption(handle, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof context) ||
            WinHttpSetStatusCallback(handle, callback, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK) {
            WinHttpCloseHandle(request); request = nullptr; return false;
        }
        return true;
    }
    void close() {
        if (!request) return;
        WinHttpCloseHandle(request); request = nullptr;
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return closing; });
    }
    template<class F> bool run(F operation, DWORD expected, DWORD* amount = nullptr) {
        if (net::cancelled()) { SetLastError(ERROR_OPERATION_ABORTED); return false; }
        { std::lock_guard<std::mutex> lock(mutex); completed = value = failure = 0; }
        if (!operation() && GetLastError() != ERROR_IO_PENDING) return false;
        std::unique_lock<std::mutex> lock(mutex);
        while (completed != expected && !failure) {
            cv.wait_for(lock, std::chrono::milliseconds(20));
            if (net::cancelled()) {
                lock.unlock(); close(); SetLastError(ERROR_OPERATION_ABORTED); return false;
            }
        }
        if (failure) { SetLastError(failure); return false; }
        if (amount) *amount = value;
        return true;
    }
};
inline thread_local RequestControl* active_request = nullptr;
inline RequestControl::RequestControl() : previous(active_request) { active_request = this; }
inline RequestControl::~RequestControl() { close(); active_request = previous; }
inline void close_request(HINTERNET handle) {
    if (active_request) active_request->close(); else if (handle) WinHttpCloseHandle(handle);
}
inline bool receive(HINTERNET handle) {
    return active_request->run([&] { return WinHttpReceiveResponse(handle, nullptr); }, WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE);
}
inline bool available(HINTERNET handle, DWORD* amount) {
    return active_request->run([&] { return WinHttpQueryDataAvailable(handle, nullptr); }, WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, amount);
}
inline bool read(HINTERNET handle, void* data, DWORD size, DWORD* amount) {
    return active_request->run([&] { return WinHttpReadData(handle, data, size, nullptr); }, WINHTTP_CALLBACK_STATUS_READ_COMPLETE, amount);
}


struct Session {
    HINTERNET h = nullptr;
    ~Session() { if (h) WinHttpCloseHandle(h); }
};

inline bool open_session(Session& s) {
    s.h = WinHttpOpen(L"aino-player/1.0 (Windows NT; WinHTTP)",
                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                      WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (!s.h)
        s.h = WinHttpOpen(L"aino-player/1.0 (Windows NT; WinHTTP)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    return s.h != nullptr;
}

inline std::string last_error_text(const char* where) {
    const DWORD e = GetLastError();
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s failed (win32=%lu)", where, (unsigned long)e);
    return buf;
}

// 建连接 + 请求 + 发送，成功时返回 request 句柄（连接句柄由调用方负责关闭）
inline HINTERNET send(HINTERNET session, const Request& req, const wchar_t* verb, HINTERNET* out_conn,
                      const void* body, DWORD body_len, std::string& err) {
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof uc;
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    const std::wstring wurl = widen(req.url);
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) {
        err = last_error_text("WinHttpCrackUrl");
        return nullptr;
    }
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    HINTERNET conn = WinHttpConnect(session, host.c_str(), uc.nPort, 0);
    if (!conn) {
        err = last_error_text("WinHttpConnect");
        return nullptr;
    }
    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hreq = WinHttpOpenRequest(conn, verb, path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hreq) {
        err = last_error_text("WinHttpOpenRequest");
        WinHttpCloseHandle(conn);
        return nullptr;
    }

    if (active_request && !active_request->bind(hreq)) {
        err = "已取消"; WinHttpCloseHandle(conn); return nullptr;
    }

    DWORD redirect = req.follow_redirects ? WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS
                                          : WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(hreq, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect);

    // 超时：解析/连接/发送/接收四段分别设，接收超时是"卡死"的主要防线
    const int t = req.timeout_ms;
    WinHttpSetTimeouts(hreq, t, t, t, t);

    std::wstring header_block;
    for (const auto& h : req.headers) {
        header_block += widen(h);
        header_block += L"\r\n";
    }

    const bool sent = active_request->run([&] { return WinHttpSendRequest(
        hreq, header_block.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header_block.c_str(),
        header_block.empty() ? 0 : (DWORD)-1L, const_cast<void*>(body), body_len, body_len,
        reinterpret_cast<DWORD_PTR>(active_request)); }, WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE);
    if (!sent) {
        err = last_error_text("WinHttpSendRequest");
        detail::close_request(hreq);
        WinHttpCloseHandle(conn);
        return nullptr;
    }
    *out_conn = conn;
    return hreq;
}

inline std::string query_header(HINTERNET hreq, DWORD info) {
    DWORD len = 0;
    WinHttpQueryHeaders(hreq, info, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &len,
                        WINHTTP_NO_HEADER_INDEX);
    if (!len) return std::string();
    std::wstring buf(len / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(hreq, info, WINHTTP_HEADER_NAME_BY_INDEX, &buf[0], &len,
                             WINHTTP_NO_HEADER_INDEX))
        return std::string();
    buf.resize(len / sizeof(wchar_t));
    return narrow(buf);
}

inline std::string final_url_of(HINTERNET hreq) {
    DWORD len = 0;
    WinHttpQueryOption(hreq, WINHTTP_OPTION_URL, nullptr, &len);
    if (!len) return std::string();
    std::wstring buf(len / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryOption(hreq, WINHTTP_OPTION_URL, &buf[0], &len)) return std::string();
    buf.resize(len / sizeof(wchar_t));
    return narrow(buf);
}

}  // namespace detail

// 通用请求：整段读进内存（接口 JSON 用）
inline Response execute(const Request& req) {
    detail::RequestControl cancellation;
    Response out;
    detail::Session s;
    if (!detail::open_session(s)) {
        out.error = detail::last_error_text("WinHttpOpen");
        return out;
    }
    HINTERNET conn = nullptr;
    const wchar_t* verb = req.body.empty() ? L"GET" : L"POST";
    HINTERNET hreq = detail::send(s.h, req, verb, &conn, req.body.empty() ? nullptr : req.body.data(),
                                  DWORD(req.body.size()), out.error);
    if (!hreq) return out;

    if (!detail::receive(hreq)) {
        out.error = detail::last_error_text("WinHttpReceiveResponse");
        detail::close_request(hreq);
        WinHttpCloseHandle(conn);
        return out;
    }
    DWORD status = 0, slen = sizeof status;
    WinHttpQueryHeaders(hreq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
    out.status = int(status);
    out.content_type = detail::query_header(hreq, WINHTTP_QUERY_CONTENT_TYPE);
    out.final_url = detail::final_url_of(hreq);
    out.location = detail::query_header(hreq, WINHTTP_QUERY_LOCATION);
    DWORD cookie_index = 0;
    for (int i = 0; i < 64; ++i) {
        std::wstring buffer(16384, L'\0');
        DWORD length = DWORD(buffer.size() * sizeof(wchar_t));
        if (!WinHttpQueryHeaders(hreq, WINHTTP_QUERY_SET_COOKIE, WINHTTP_HEADER_NAME_BY_INDEX,
                                 buffer.data(), &length, &cookie_index)) break;
        buffer.resize(length / sizeof(wchar_t));
        out.set_cookies.push_back(narrow(buffer));
    }

    for (;;) {
        DWORD avail = 0;
        if (!detail::available(hreq, &avail)) { out.error = detail::last_error_text("读取响应"); break; }
        if (!avail) break;
        avail = std::min<DWORD>(avail, 65536);
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!detail::read(hreq, buf.data(), avail, &read)) { out.error = detail::last_error_text("读取响应"); break; }
        if (!read) break;
        out.body.append(buf.data(), read);
        if (out.body.size() > 8u * 1024 * 1024) { out.error = "接口响应过大"; break; }  // 接口响应不可能这么大
    }
    detail::close_request(hreq);
    WinHttpCloseHandle(conn);
    if (net::cancelled()) out.error = "已取消";
    if (!out.error.empty()) { out.status = 0; out.body.clear(); }
    return out;
}

inline Response get(const std::string& url, const std::vector<std::string>& headers = {},
                    int timeout_ms = 8000) {
    Request r;
    r.url = url;
    r.headers = headers;
    r.timeout_ms = timeout_ms;
    return execute(r);
}

inline Response post(const std::string& url, const std::string& body,
                     const std::vector<std::string>& headers = {}, int timeout_ms = 8000) {
    Request r;
    r.url = url;
    r.body = body;
    r.headers = headers;
    r.timeout_ms = timeout_ms;
    return execute(r);
}

struct AudioProbe {
    bool playable = false;
    std::string final_url;
    std::string content_type;
    long long total_bytes = 0;
    int status = 0;
    std::string reason;  // 不可播时的结构化原因
};

// 小范围读取容器头，既拒绝 HTML/JSON，也支持 MP3/FLAC/WAV/M4A/Ogg/WebM。
inline AudioProbe probe_audio(const std::string& url, const std::vector<std::string>& headers,
                              int timeout_ms = 6000) {
    detail::RequestControl cancellation;
    AudioProbe out;
    detail::Session s;
    if (!detail::open_session(s)) {
        out.reason = "网络初始化失败";
        return out;
    }
    Request req;
    req.url = url;
    req.headers = headers;
    req.headers.push_back("Range: bytes=0-11");
    req.timeout_ms = timeout_ms;

    HINTERNET conn = nullptr;
    std::string err;
    HINTERNET hreq = detail::send(s.h, req, L"GET", &conn, nullptr, 0, err);
    if (!hreq) {
        out.reason = err;
        out.status = -1;
        return out;
    }
    if (!detail::receive(hreq)) {
        out.reason = detail::last_error_text("WinHttpReceiveResponse");
        out.status = -1;
        detail::close_request(hreq);
        WinHttpCloseHandle(conn);
        return out;
    }
    DWORD status = 0, slen = sizeof status;
    WinHttpQueryHeaders(hreq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
    out.status = int(status);
    out.content_type = detail::query_header(hreq, WINHTTP_QUERY_CONTENT_TYPE);
    out.final_url = detail::final_url_of(hreq);

    // Content-Range: bytes 0-1/3368088 → 总长
    const std::string cr = detail::query_header(hreq, WINHTTP_QUERY_CONTENT_RANGE);
    const size_t slash = cr.find('/');
    if (slash != std::string::npos) {
        try { out.total_bytes = std::stoll(cr.substr(slash + 1)); } catch (...) {}
    }

    char head[12]{};
    DWORD read = 0;
    while (read < sizeof head) {
        DWORD received = 0;
        if (!detail::read(hreq, head + read, DWORD(sizeof head) - read, &received) || !received) break;
        read += received;
    }
    detail::close_request(hreq);
    WinHttpCloseHandle(conn);

    const bool looks_audio = audio_prefix(head, read);
    const bool audio_type = out.content_type.find("audio") != std::string::npos ||
                            out.content_type.find("octet-stream") != std::string::npos ||
                            out.content_type.find("mpeg") != std::string::npos ||
                            out.content_type.find("mp4") != std::string::npos ||
                            out.content_type.find("webm") != std::string::npos;
    out.playable = (out.status == 200 || out.status == 206) && audio_type && looks_audio;
    if (!out.playable) {
        if (out.status == 404) out.reason = "已下架 / 无版权";
        else if (out.status == 403) out.reason = "直链被拒（可能需要登录）";
        else if (!looks_audio && audio_type) out.reason = "返回内容不是音频";
        else if (out.status == 200 && !audio_type) out.reason = "需要登录或会员";
        else out.reason = "取流失败 HTTP " + std::to_string(out.status);
    }
    return out;
}

// 流式下载；返回 false 时 err 有内容。progress(已下载, 总长) 由调用线程回调。
inline bool download(const std::string& url, const std::string& path,
                     const std::vector<std::string>& headers,
                     const std::function<void(long long, long long)>& progress,
                     const std::atomic<bool>& cancel, std::string& err, int timeout_ms = 20000,
                     const std::function<bool(const char*, size_t)>& sink = {}) {
    const auto parent = cancel_current;
    CancelScope signal([&cancel, parent] { return cancel.load() || (parent && parent()); });
    detail::RequestControl cancellation;
    detail::Session s;
    if (!detail::open_session(s)) {
        err = detail::last_error_text("WinHttpOpen");
        return false;
    }
    Request req;
    req.url = url;
    req.headers = headers;
    req.timeout_ms = timeout_ms;

    HINTERNET conn = nullptr;
    HINTERNET hreq = detail::send(s.h, req, L"GET", &conn, nullptr, 0, err);
    if (!hreq) return false;
    if (!detail::receive(hreq)) {
        err = detail::last_error_text("WinHttpReceiveResponse");
        detail::close_request(hreq);
        WinHttpCloseHandle(conn);
        return false;
    }
    DWORD status = 0, slen = sizeof status;
    WinHttpQueryHeaders(hreq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
    long long total = -1;
    const std::string cl = detail::query_header(hreq, WINHTTP_QUERY_CONTENT_LENGTH);
    if (!cl.empty()) {
        try { total = std::stoll(cl); } catch (...) {}
    }
    if (status != 200 && status != 206) {
        err = "下载失败 HTTP " + std::to_string(status);
        detail::close_request(hreq);
        WinHttpCloseHandle(conn);
        return false;
    }

    // 必须用 _wfopen：fopen 按 ANSI(GBK) 解释路径，而 path 是 UTF-8，
    // 中文文件名（"周杰伦 - 屋顶.mp3"）会直接开不出来，报"无法创建临时文件"。
    std::FILE* f = sink ? nullptr : _wfopen(widen(path).c_str(), L"wb");
    if (!sink && !f) {
        err = "无法创建临时文件";
        detail::close_request(hreq);
        WinHttpCloseHandle(conn);
        return false;
    }
    long long got = 0;
    bool ok = true;
    std::vector<char> buf(64 * 1024);
    for (;;) {
        if (net::cancelled()) {
            ok = false;
            err = "已取消";
            break;
        }
        DWORD avail = 0;
        if (!detail::available(hreq, &avail)) { ok = false; err = net::cancelled() ? "已取消" : detail::last_error_text("读取音频"); break; }
        if (avail == 0) break;
        const DWORD want = avail > buf.size() ? DWORD(buf.size()) : avail;
        DWORD read = 0;
        if (!detail::read(hreq, buf.data(), want, &read)) { ok = false; err = net::cancelled() ? "已取消" : detail::last_error_text("读取音频"); break; }
        if (read == 0) break;
        if (sink ? !sink(buf.data(), read) : std::fwrite(buf.data(), 1, read, f) != read) { ok = false; err = sink ? "音频流已停止" : "文件写入失败"; break; }
        got += read;
        if (progress) progress(got, total);
    }
    if (f && std::fclose(f) != 0) { ok = false; err = "文件写入失败"; }
    detail::close_request(hreq);
    WinHttpCloseHandle(conn);
    if (net::cancelled() || (total >= 0 && got != total)) { ok = false; err = net::cancelled() ? "已取消" : "下载未完成"; }
    if (ok && got == 0) {
        err = "响应为空";
        return false;
    }
    return ok;
}

}  // namespace net

#else
#include "net_curl.hpp"
#endif
