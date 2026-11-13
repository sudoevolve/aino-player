// Linux HTTP 后端：保持与 WinHTTP 相同的业务接口，TLS 校验由 libcurl 完成。
#pragma once
#include <curl/curl.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#include "platform.hpp"

namespace net {
inline std::wstring widen(const std::string& s) { return platform::widen(s); }
inline std::string narrow(const std::wstring& s) { return platform::narrow(s); }
inline std::string url_encode(const std::string& s) {
    const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((std::isalnum(c) && c < 128) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}
struct Response {
    int status = 0;
    std::string body, content_type, final_url, error, content_range;
    std::vector<std::string> set_cookies;
    std::string location;
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};
struct Request {
    std::string url, body;
    std::vector<std::string> headers;
    int timeout_ms = 8000;
    bool follow_redirects = true;
    size_t max_body = 8u * 1024 * 1024;
};
namespace detail {
struct Transfer {
    CURL* handle = nullptr;
    curl_slist* headers = nullptr;
    std::string content_range;
    std::vector<std::string> set_cookies;
    std::string location;
    Transfer() {
        static const auto init = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (init == CURLE_OK) handle = curl_easy_init();
    }
    ~Transfer() { if (handle) curl_easy_cleanup(handle); if (headers) curl_slist_free_all(headers); }
    bool setup(const Request& req) {
        if (!handle) return false;
        curl_easy_setopt(handle, CURLOPT_URL, req.url.c_str());
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, long(req.timeout_ms));
        curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, long(req.timeout_ms));
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, req.follow_redirects ? 1L : 0L);
        curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(handle, CURLOPT_PROTOCOLS, long(CURLPROTO_HTTP | CURLPROTO_HTTPS));
        curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS, long(CURLPROTO_HTTP | CURLPROTO_HTTPS));
        curl_easy_setopt(handle, CURLOPT_USERAGENT, "aino-player/1.0");
        curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, +[](void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int { return net::cancelled() ? 1 : 0; });
        curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
        for (const auto& h : req.headers) headers = curl_slist_append(headers, h.c_str());
        if (headers) curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
        if (!req.body.empty()) {
            curl_easy_setopt(handle, CURLOPT_POSTFIELDS, req.body.data());
            curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, curl_off_t(req.body.size()));
        }
        // 部分 libcurl 头文件把 setopt 定义为宏，不能把含逗号声明的 lambda 直接作为宏参数。
        const auto header_callback = +[](char* p, size_t a, size_t b, void* data) -> size_t {
            const size_t n = a * b;
            std::string line(p, n), lower = line;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (lower.rfind("content-range:", 0) == 0) static_cast<Transfer*>(data)->content_range = line.substr(14);
            auto value = [&](size_t offset) {
                auto text = line.substr(offset);
                while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) text.pop_back();
                const auto first = text.find_first_not_of(" \t");
                return first == std::string::npos ? std::string() : text.substr(first);
            };
            if (lower.rfind("set-cookie:", 0) == 0) static_cast<Transfer*>(data)->set_cookies.push_back(value(11));
            if (lower.rfind("location:", 0) == 0) static_cast<Transfer*>(data)->location = value(9);
            return n;
        };
        curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(handle, CURLOPT_HEADERDATA, this);
        return true;
    }
    void response(Response& out) {
        long status = 0;
        char* type = nullptr; char* url = nullptr;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(handle, CURLINFO_CONTENT_TYPE, &type);
        curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &url);
        out.status = int(status); out.content_range = content_range;
        out.set_cookies = set_cookies; out.location = location;
        if (type) out.content_type = type;
        if (url) out.final_url = url;
    }
};
}
inline Response execute(const Request& req) {
    Response out;
    detail::Transfer transfer;
    if (!transfer.setup(req)) { out.error = "网络初始化失败"; return out; }
    struct Body { std::string& text; size_t limit; } body{out.body, req.max_body};
    curl_easy_setopt(transfer.handle, CURLOPT_WRITEFUNCTION, +[](char* p, size_t a, size_t b, void* data) -> size_t {
        auto& body = *static_cast<Body*>(data);
        const auto n = a * b;
        if (body.text.size() + n > body.limit) return 0;
        body.text.append(p, n); return n;
    });
    curl_easy_setopt(transfer.handle, CURLOPT_WRITEDATA, &body);
    const auto rc = curl_easy_perform(transfer.handle);
    transfer.response(out);
    if (rc != CURLE_OK) out.error = curl_easy_strerror(rc);
    return out;
}
inline Response get(const std::string& url, const std::vector<std::string>& headers = {}, int timeout_ms = 8000) {
    return execute({url, {}, headers, timeout_ms, true});
}
inline Response post(const std::string& url, const std::string& body,
                     const std::vector<std::string>& headers = {}, int timeout_ms = 8000) {
    return execute({url, body, headers, timeout_ms, true});
}
struct AudioProbe {
    bool playable = false;
    std::string final_url, content_type;
    long long total_bytes = 0;
    int status = 0;
    std::string reason;
};
inline AudioProbe probe_audio(const std::string& url, const std::vector<std::string>& headers, int timeout_ms = 6000) {
    AudioProbe out;
    Request req{url, {}, headers, timeout_ms, true};
    req.headers.push_back("Range: bytes=0-11");
    detail::Transfer transfer;
    if (!transfer.setup(req)) { out.reason = "网络初始化失败"; return out; }
    std::string first;
    curl_easy_setopt(transfer.handle, CURLOPT_WRITEFUNCTION, +[](char* p, size_t a, size_t b, void* data) -> size_t {
        auto& first = *static_cast<std::string*>(data);
        const auto n = a * b;
        first.append(p, std::min(n, size_t(12) - first.size()));
        return first.size() >= 12 ? 0 : n;
    });
    curl_easy_setopt(transfer.handle, CURLOPT_WRITEDATA, &first);
    const auto rc = curl_easy_perform(transfer.handle);
    Response response;
    transfer.response(response);
    out.status = response.status; out.content_type = response.content_type; out.final_url = response.final_url;
    const auto slash = transfer.content_range.find('/');
    if (slash != std::string::npos) { try { out.total_bytes = std::stoll(transfer.content_range.substr(slash + 1)); } catch (...) {} }
    const bool audio_type = out.content_type.find("audio") != std::string::npos || out.content_type.find("octet-stream") != std::string::npos ||
        out.content_type.find("mpeg") != std::string::npos || out.content_type.find("mp4") != std::string::npos || out.content_type.find("webm") != std::string::npos;
    out.playable = (rc == CURLE_OK || rc == CURLE_WRITE_ERROR) && audio_prefix(first.data(), first.size()) &&
        (out.status == 200 || out.status == 206) && audio_type;
    if (!out.playable) out.reason = "取流失败 HTTP " + std::to_string(out.status);
    return out;
}
inline bool download(const std::string& url, const std::string& path, const std::vector<std::string>& headers,
                     const std::function<void(long long, long long)>& progress, const std::atomic<bool>& cancel,
                     std::string& err, int timeout_ms = 20000,
                     const std::function<bool(const char*, size_t)>& sink = {}) {
    detail::Transfer transfer;
    if (!transfer.setup({url, {}, headers, timeout_ms, true})) { err = "网络初始化失败"; return false; }
    auto* file = sink ? nullptr : platform::fopen(path, "wb");
    if (!sink && !file) { err = "无法创建临时文件"; return false; }
    struct Output { std::FILE* file; const std::function<bool(const char*,size_t)>& sink; size_t bytes = 0; } output{file, sink};
    struct Progress { const std::atomic<bool>& cancel; const std::function<void(long long,long long)>& fn; } state{cancel, progress};
    curl_easy_setopt(transfer.handle, CURLOPT_FAILONERROR, 1L);
    // 播放时管道背压会暂停网络读取，不能把整首播放时间当成总下载超时。
    if (sink) {
        curl_easy_setopt(transfer.handle, CURLOPT_TIMEOUT_MS, 0L);
        curl_easy_setopt(transfer.handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(transfer.handle, CURLOPT_LOW_SPEED_TIME, 20L);
    }
    curl_easy_setopt(transfer.handle, CURLOPT_WRITEFUNCTION, +[](char* p, size_t a, size_t b, void* data) -> size_t {
        auto& output = *static_cast<Output*>(data);
        const auto n = a * b;
        if (net::cancelled()) return 0;
        const auto written = output.sink ? (output.sink(p, n) ? n : size_t(0)) : std::fwrite(p, 1, n, output.file);
        output.bytes += written; return written;
    });
    curl_easy_setopt(transfer.handle, CURLOPT_WRITEDATA, &output);
    curl_easy_setopt(transfer.handle, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(transfer.handle, CURLOPT_XFERINFOFUNCTION, +[](void* p, curl_off_t total, curl_off_t got, curl_off_t, curl_off_t) -> int {
        auto& state = *static_cast<Progress*>(p);
        if (state.cancel.load() || net::cancelled()) return 1;
        if (state.fn) state.fn(got, total);
        return 0;
    });
    curl_easy_setopt(transfer.handle, CURLOPT_XFERINFODATA, &state);
    const auto rc = curl_easy_perform(transfer.handle);
    const bool closed = !file || std::fclose(file) == 0;
    if (rc != CURLE_OK || !closed || !output.bytes || cancel.load() || net::cancelled()) {
        err = cancel.load() || net::cancelled() ? "已取消" : rc != CURLE_OK ? curl_easy_strerror(rc) : "文件写入失败或响应为空";
        if (!sink) platform::remove(path);
        return false;
    }
    return true;
}
} // namespace net
