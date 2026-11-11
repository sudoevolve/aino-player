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

#include <windows.h>
#include <winhttp.h>

#include <atomic>
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

struct Session {
    HINTERNET h = nullptr;
    ~Session() { if (h) WinHttpCloseHandle(h); }
};

inline bool open_session(Session& s) {
    s.h = WinHttpOpen(L"aino-player/1.0 (Windows NT; WinHTTP)",
                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s.h)
        s.h = WinHttpOpen(L"aino-player/1.0 (Windows NT; WinHTTP)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
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

    DWORD redirect = req.follow_redirects ? WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS
                                          : WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(hreq, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect);

    // 超时：解析/连接/发送/接收四段分别设，接收超时是"卡死"的主要防线
    const int t = req.timeout_ms;
    WinHttpSetTimeouts(session, t, t, t, t);

    std::wstring header_block;
    for (const auto& h : req.headers) {
        header_block += widen(h);
        header_block += L"\r\n";
    }

    const BOOL sent = WinHttpSendRequest(
        hreq, header_block.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header_block.c_str(),
        header_block.empty() ? 0 : (DWORD)-1L, const_cast<void*>(body), body_len, body_len, 0);
    if (!sent) {
        err = last_error_text("WinHttpSendRequest");
        WinHttpCloseHandle(hreq);
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

    if (!WinHttpReceiveResponse(hreq, nullptr)) {
        out.error = detail::last_error_text("WinHttpReceiveResponse");
        WinHttpCloseHandle(hreq);
        WinHttpCloseHandle(conn);
        return out;
    }
    DWORD status = 0, slen = sizeof status;
    WinHttpQueryHeaders(hreq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
    out.status = int(status);
    out.content_type = detail::query_header(hreq, WINHTTP_QUERY_CONTENT_TYPE);
    out.final_url = detail::final_url_of(hreq);

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hreq, &avail) || avail == 0) break;
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(hreq, buf.data(), avail, &read) || read == 0) break;
        out.body.append(buf.data(), read);
        if (out.body.size() > 8u * 1024 * 1024) break;  // 接口响应不可能这么大
    }
    WinHttpCloseHandle(hreq);
    WinHttpCloseHandle(conn);
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

// 只取前 2 字节：能出声的直链一定返回 206/200 + audio/*（或其首字节像 mp3）
inline AudioProbe probe_audio(const std::string& url, const std::vector<std::string>& headers,
                              int timeout_ms = 6000) {
    AudioProbe out;
    detail::Session s;
    if (!detail::open_session(s)) {
        out.reason = "网络初始化失败";
        return out;
    }
    Request req;
    req.url = url;
    req.headers = headers;
    req.headers.push_back("Range: bytes=0-1");
    req.timeout_ms = timeout_ms;

    HINTERNET conn = nullptr;
    std::string err;
    HINTERNET hreq = detail::send(s.h, req, L"GET", &conn, nullptr, 0, err);
    if (!hreq) {
        out.reason = err;
        out.status = -1;
        return out;
    }
    if (!WinHttpReceiveResponse(hreq, nullptr)) {
        out.reason = detail::last_error_text("WinHttpReceiveResponse");
        out.status = -1;
        WinHttpCloseHandle(hreq);
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

    char head[2] = {0, 0};
    DWORD read = 0;
    WinHttpReadData(hreq, head, 2, &read);
    WinHttpCloseHandle(hreq);
    WinHttpCloseHandle(conn);

    const bool looks_mp3 = read >= 2 && ((unsigned char)head[0] == 0xFF || head[0] == 'I');
    const bool audio_type = out.content_type.find("audio") != std::string::npos ||
                            out.content_type.find("octet-stream") != std::string::npos ||
                            out.content_type.find("mpeg") != std::string::npos;
    out.playable = (out.status == 200 || out.status == 206) && audio_type && looks_mp3;
    if (!out.playable) {
        if (out.status == 404) out.reason = "已下架 / 无版权";
        else if (out.status == 403) out.reason = "直链被拒（可能需要登录）";
        else if (!looks_mp3 && audio_type) out.reason = "返回内容不是音频";
        else if (out.status == 200 && !audio_type) out.reason = "需要登录或会员";
        else out.reason = "取流失败 HTTP " + std::to_string(out.status);
    }
    return out;
}

// 流式下载；返回 false 时 err 有内容。progress(已下载, 总长) 由调用线程回调。
inline bool download(const std::string& url, const std::string& path,
                     const std::vector<std::string>& headers,
                     const std::function<void(long long, long long)>& progress,
                     const std::atomic<bool>& cancel, std::string& err, int timeout_ms = 20000) {
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
    if (!WinHttpReceiveResponse(hreq, nullptr)) {
        err = detail::last_error_text("WinHttpReceiveResponse");
        WinHttpCloseHandle(hreq);
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
        WinHttpCloseHandle(hreq);
        WinHttpCloseHandle(conn);
        return false;
    }

    // 必须用 _wfopen：fopen 按 ANSI(GBK) 解释路径，而 path 是 UTF-8，
    // 中文文件名（"周杰伦 - 屋顶.mp3"）会直接开不出来，报"无法创建临时文件"。
    std::FILE* f = _wfopen(widen(path).c_str(), L"wb");
    if (!f) {
        err = "无法创建临时文件";
        WinHttpCloseHandle(hreq);
        WinHttpCloseHandle(conn);
        return false;
    }
    long long got = 0;
    bool ok = true;
    std::vector<char> buf(64 * 1024);
    for (;;) {
        if (cancel.load(std::memory_order_relaxed)) {
            ok = false;
            err = "已取消";
            break;
        }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hreq, &avail) || avail == 0) break;
        const DWORD want = avail > buf.size() ? DWORD(buf.size()) : avail;
        DWORD read = 0;
        if (!WinHttpReadData(hreq, buf.data(), want, &read) || read == 0) break;
        std::fwrite(buf.data(), 1, read, f);
        got += read;
        if (progress) progress(got, total);
    }
    std::fclose(f);
    WinHttpCloseHandle(hreq);
    WinHttpCloseHandle(conn);
    if (ok && got == 0) {
        err = "响应为空";
        return false;
    }
    return ok;
}

}  // namespace net
