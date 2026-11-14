// lxsource.hpp —— 落雪（LX Music）音源脚本运行时
//
// 干什么：把 lx-music 的 .js 音源脚本跑起来，用它的 musicUrl 解析播放直链。
// 怎么跑：嵌入 QuickJS（vendor/quickjs），注入一个最小的 `globalThis.lx`
//         （on / send / request / utils / EVENT_NAMES），脚本里的 HTTP 请求
//         排入运行时任务队列，再交给 aino 自己的 HTTP 后端。
//
// 设计取舍：
//   * HTTP 在独立网络任务中并行执行，JS 回调、microtask 与到期定时器由运行时线程推进。
//   * 每个脚本一个独立 JSRuntime。脚本之间状态互不污染，坏掉的源不会拖垮别的。
//   * 只实现脚本真正用得到的 API（md5 / base64 / buffer）。aes、rsa 明确抛错，
//     而不是静默返回垃圾数据——出问题要能一眼看出来。
#pragma once

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <stdexcept>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <tuple>

#include "background.hpp"
#include "json.hpp"
#include "net.hpp"
#include "platform.hpp"

extern "C" {
#include "quickjs.h"
}

namespace lx {

inline bool script_file(const std::filesystem::path& path) {
    auto extension = path.extension().u8string();
    for (char& c : extension) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return extension == ".js";
}

class Script;  // 前向声明：C 回调和 detail 下面的垫片都要用到它

// ---------------------------------------------------------------- 数据结构

struct Decl {                          // 脚本声明的"我支持哪个平台"
    std::string key;                   // wy / tx / kw / kg / mg
    std::string name;                  // 脚本里给这个平台起的名字
    std::vector<std::string> actions;  // musicUrl / lyric / pic
    std::vector<std::string> qualitys;
};

struct Info {
    std::string name, version, author, description, homepage, file;
    std::string fingerprint;
};

// `/** @name xxx */` 头注释，规则和 lx 自己一致
inline std::map<std::string, std::string> parse_header(const std::string& script) {
    std::map<std::string, std::string> out;
    const size_t end = script.find("*/");
    if (end == std::string::npos) return out;
    const std::string head = script.substr(0, end);
    size_t pos = 0;
    while ((pos = head.find('@', pos)) != std::string::npos) {
        size_t eol = head.find('\n', pos);
        if (eol == std::string::npos) eol = head.size();
        const std::string line = head.substr(pos + 1, eol - pos - 1);
        const size_t sp = line.find_first_of(" \t");
        if (sp != std::string::npos) {
            std::string key = line.substr(0, sp), val = line.substr(sp + 1);
            while (!val.empty() && (val.back() == '\r' || val.back() == ' ')) val.pop_back();
            while (!val.empty() && val.front() == ' ') val.erase(val.begin());
            if (!key.empty() && !val.empty()) out[key] = val;
        }
        pos = eol;
    }
    return out;
}

// aino 的音质档位 → lx 的 type
inline const char* quality_key(int q) {
    switch (q) {
        case 0: return "128k";
        case 2: return "flac";
        default: return "320k";
    }
}

namespace detail {

inline thread_local Script* current = nullptr;  // 供 C 回调取回当前脚本

inline std::string from_js(JSContext* ctx, JSValueConst v);

inline std::string js_quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char tmp[8];
                    std::snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                    out += tmp;
                } else {
                    out += char(c);
                }
        }
    }
    out += '"';
    return out;
}

inline std::string from_js(JSContext* ctx, JSValueConst v) {
    const char* s = JS_ToCString(ctx, v);
    if (!s) return std::string();
    std::string out(s);
    JS_FreeCString(ctx, s);
    return out;
}

inline JSValue to_js(JSContext* ctx, const std::string& s) {
    return JS_NewStringLen(ctx, s.data(), s.size());
}

inline std::string take_exception(JSContext* ctx) {
    JSValue e = JS_GetException(ctx);
    std::string text = from_js(ctx, e);
    if (JS_IsObject(e)) {
        JSValue st = JS_GetPropertyStr(ctx, e, "stack");
        if (JS_IsString(st)) {
            const std::string stack = from_js(ctx, st);
            if (!stack.empty() && stack.find(text) == std::string::npos) text += "\n" + stack;
        }
        JS_FreeValue(ctx, st);
    }
    JS_FreeValue(ctx, e);
    if (text.empty()) text = "脚本抛出异常（无消息）";
    return text;
}

// ------------------------------------------------------------ md5 / base64

struct Md5 {
    uint32_t h[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    uint64_t total = 0;
    unsigned char buf[64]{};
    size_t used = 0;

    static uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

    void block(const unsigned char* p) {
        static const uint32_t K[64] = {
            0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u,
            0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u,
            0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du,
            0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
            0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u,
            0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
            0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u,
            0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
            0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu,
            0xeb86d391u};
        static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                  5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        uint32_t w[16];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(p[i * 4]) | (uint32_t(p[i * 4 + 1]) << 8) | (uint32_t(p[i * 4 + 2]) << 16) |
                (uint32_t(p[i * 4 + 3]) << 24);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            uint32_t f;
            int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            const uint32_t tmp = d;
            d = c;
            c = b;
            b += rotl(a + f + K[i] + w[g], S[i]);
            a = tmp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }

    void update(const unsigned char* p, size_t n) {
        total += n;
        while (n) {
            const size_t take = std::min(n, size_t(64 - used));
            std::memcpy(buf + used, p, take);
            used += take; p += take; n -= take;
            if (used == 64) { block(buf); used = 0; }
        }
    }

    std::string hex() {
        const uint64_t bits = total * 8;
        unsigned char pad = 0x80, zero = 0;
        update(&pad, 1);
        while (used != 56) update(&zero, 1);
        unsigned char len[8];
        for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (i * 8));
        update(len, 8);
        static const char* hx = "0123456789abcdef";
        std::string out;
        out.reserve(32);
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                const unsigned char byte = uint8_t(h[i] >> (j * 8));
                out += hx[byte >> 4];
                out += hx[byte & 0xF];
            }
        return out;
    }
};

inline std::string md5_hex(const std::string& s) {
    Md5 m;
    m.update(reinterpret_cast<const unsigned char*>(s.data()), s.size());
    return m.hex();
}

inline std::string base64_encode(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = (uint32_t(uint8_t(in[i])) << 16) | (uint32_t(uint8_t(in[i + 1])) << 8) | uint8_t(in[i + 2]);
        out += t[(v >> 18) & 63]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
    }
    if (i + 1 == in.size()) {
        const uint32_t v = uint32_t(uint8_t(in[i])) << 16;
        out += t[(v >> 18) & 63]; out += t[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        const uint32_t v = (uint32_t(uint8_t(in[i])) << 16) | (uint32_t(uint8_t(in[i + 1])) << 8);
        out += t[(v >> 18) & 63]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '=';
    }
    return out;
}

inline std::string base64_decode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    int carry = 0, bits = 0;
    for (char c : in) {
        const int v = val(c);
        if (v < 0) continue;
        carry = (carry << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += char((carry >> bits) & 0xFF);
        }
    }
    return out;
}

// 脚本里的 console.log 落到 aino 的日志（stderr）
inline void log_line(const std::string& line) {
    std::fprintf(stderr, "[lx] %s\n", line.c_str());
}

}  // namespace detail

// ---------------------------------------------------------------- 脚本实例

class Script {
public:
    Script() = default;
    Script(const Script&) = delete;
    Script& operator=(const Script&) = delete;
    ~Script() { close(); }

    const Info& info() const { return info_; }
    const std::vector<Decl>& sources() const { return sources_; }
    bool ready() const { return inited_ && !sources_.empty(); }
    const std::string& error() const { return error_; }
    const std::string& path() const { return info_.file; }
    void retire() { retired_.store(true); }
    bool retired() const { return retired_.load(); }
    bool playback_waiting() const { return foreground_.load() != 0; }

    const Decl* find(const std::string& key) const {
        for (const auto& d : sources_)
            if (d.key == key) return &d;
        return nullptr;
    }

    bool supports(const std::string& key, const std::string& action) const {
        const Decl* d = find(key);
        if (!d) return false;
        return std::find(d->actions.begin(), d->actions.end(), action) != d->actions.end();
    }

    // 脚本声明的最优可用音质（给它一个 aino 档位，挑实际支持的）
    std::string pick_quality(const std::string& key, int wanted) const {
        const Decl* d = find(key);
        if (!d || d->qualitys.empty()) return quality_key(wanted);
        // 只向下兼容档位，不把用户选的标准音质升级为更大的无损流。
        const std::vector<std::string> prefer = wanted >= 2 ? std::vector<std::string>{"flac", "flac24bit", "320k", "128k"}
            : wanted == 1 ? std::vector<std::string>{"320k", "128k"} : std::vector<std::string>{"128k"};
        for (const auto& p : prefer) {
            if (std::find(d->qualitys.begin(), d->qualitys.end(), p) != d->qualitys.end()) return p;
        }
        return {};
    }

    void on_send(const std::string& event, const std::string& json);
    unsigned start_http(net::Request request) {
        for (auto it = http_.begin(); it != http_.end();) {
            if (it->second.cancel->load() && it->second.result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
                it = http_.erase(it);
            else ++it;
        }
        if (http_.size() >= 16) return 0;
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        const auto id = next_http_++;
        auto result = std::async(std::launch::async, [request = std::move(request), cancel] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(request.timeout_ms);
            // 网络线程不持有 QuickJS 或调用栈中的 Operation，取消状态独立存活。
            net::CancelScope cancellation([cancel, deadline] { return cancel->load() || std::chrono::steady_clock::now() >= deadline; });
            return net::execute(request);
        });
        http_.emplace(id, HttpTask{std::move(cancel), std::move(result)});
        return id;
    }
    bool poll_http(unsigned id, net::Response& response) {
        const auto it = http_.find(id);
        if (it == http_.end()) { response.error = "请求已取消"; return true; }
        if (it->second.result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return false;
        try { response = it->second.result.get(); }
        catch (const std::exception& e) { response.error = e.what(); }
        http_.erase(it);
        return true;
    }
    void cancel_http(unsigned id) {
        const auto it = http_.find(id);
        if (it != http_.end()) it->second.cancel->store(true);
    }

    bool load(const std::string& path, const std::string& script, std::string& err);
    bool init(std::string& err);
    bool music_url(const std::string& platform, const std::string& quality,
                   const std::string& music_info_json, std::string& url, std::string& err, bool background = false);
    void close();

    // 只由后台维护调用，忙碌的脚本直接跳过，避免 GC 阻塞点播或 UI。
    void collect_idle() {
        if (foreground_.load() || retired()) return;
        std::unique_lock<std::timed_mutex> lock(execution_, std::try_to_lock);
        if (!lock || !rt_ || foreground_.load() || retired()) return;
        JS_UpdateStackTop(rt_);
        for (auto it = http_.begin(); it != http_.end();) {
            if (it->second.cancel->load() && it->second.result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
                it = http_.erase(it);
            else ++it;
        }
        JS_RunGC(rt_);
    }

    // 调试用：在脚本上下文里跑一段代码，把结果当字符串返回（失败返回错误文本）
    std::string debug_eval(const std::string& code) {
        Operation operation(*this, 1000);
        if (!operation) return "<script busy or cancelled>";
        if (!ctx_) return "<no context>";
        JSValue v = JS_Eval(ctx_, code.c_str(), code.size(), "<debug>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) {
            const std::string err = detail::take_exception(ctx_);
            JS_FreeValue(ctx_, v);
            return "<error> " + err;
        }
        std::string out = detail::from_js(ctx_, v);
        JS_FreeValue(ctx_, v);
        return out;
    }

private:
    // 同一个 QuickJS runtime 不允许被点播、预加载和下载线程同时进入。
    // 截止时间包含等待锁和同步 HTTP 的实际时间，每次调用重新计时。
    struct Operation {
        Script& owner;
        std::chrono::steady_clock::time_point deadline;
        std::unique_lock<std::timed_mutex> lock;
        net::CancelScope network;
        Script* previous = detail::current;
        Operation(Script& script, int milliseconds)
            : owner(script), deadline(std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds)),
              lock(script.execution_, std::defer_lock),
              network([this, parent = net::cancel_current] {
                  return owner.retired() || std::chrono::steady_clock::now() >= deadline || (parent && parent());
              }) {
            while (!net::cancelled() && !lock.try_lock_for(std::chrono::milliseconds(20))) {}
            if (lock.owns_lock() && !net::cancelled()) {
                if (owner.rt_) JS_UpdateStackTop(owner.rt_);
                detail::current = &owner;
            }
        }
        explicit operator bool() const { return lock.owns_lock() && !net::cancelled(); }
        ~Operation() { detail::current = previous; }
    };
    bool eval(const std::string& code, const char* name, std::string& err);
    int pump(std::string& err);
    void clear_tasks() {
        // 仅结束当前取流作用域，保留脚本初始化创建的令牌和更新任务。
        const char* code = "globalThis.__lxClearTasks && __lxClearTasks()";
        // 清理有独立预算，取消原请求也必须能清掉它留下的定时器。
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
        net::CancelScope cleanup([deadline] { return std::chrono::steady_clock::now() >= deadline; });
        JSValue r = JS_Eval(ctx_, code, std::strlen(code), "<clear-tasks>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) { JSValue e = JS_GetException(ctx_); JS_FreeValue(ctx_, e); }
        JS_FreeValue(ctx_, r);
    }
    bool call_handler(const std::string& event, JSValueConst arg, JSValue& out, std::string& err);
    bool settle(JSValue& value, std::string& err);

    std::timed_mutex execution_;
    std::atomic<bool> retired_{false};
    std::atomic<unsigned> foreground_{0};
    JSRuntime* rt_ = nullptr;
    JSContext* ctx_ = nullptr;
    JSValue handlers_ = JS_UNDEFINED;
    Info info_;
    std::vector<Decl> sources_;
    std::string error_;
    bool inited_ = false;
    bool declarations_frozen_ = false;
    struct HttpTask {
        std::shared_ptr<std::atomic<bool>> cancel;
        std::future<net::Response> result;
    };
    unsigned next_http_ = 1;
    std::map<unsigned, HttpTask> http_;
};

// ---------------------------------------------------------------- C 回调

namespace detail {

// js::Value → QuickJS 值。响应体在 lx 真机上已经解析过，脚本会直接 body.code，
// 所以这里必须给对象而不是字符串。
inline JSValue to_js_value(JSContext* ctx, const js::Value& v) {
    switch (v.type) {
        case js::Type::Null: return JS_NULL;
        case js::Type::Bool: return JS_NewBool(ctx, v.boolean);
        case js::Type::Number: return JS_NewFloat64(ctx, v.number);
        case js::Type::String: return to_js(ctx, v.string);
        case js::Type::Array: {
            JSValue arr = JS_NewArray(ctx);
            for (size_t i = 0; i < v.array.size(); ++i)
                JS_SetPropertyUint32(ctx, arr, uint32_t(i), to_js_value(ctx, v.array[i]));
            return arr;
        }
        case js::Type::Object: {
            JSValue obj = JS_NewObject(ctx);
            for (const auto& kv : v.object)
                JS_SetPropertyStr(ctx, obj, kv.first.c_str(), to_js_value(ctx, kv.second));
            return obj;
        }
    }
    return JS_NULL;
}

inline bool http_request(JSContext* ctx, int argc, JSValueConst* argv, net::Request& request) {
    if (argc < 2) return false;
    js::Value options;
    if (!js::parse(from_js(ctx, argv[1]), options) || !options.is_object()) return false;
    std::string method = options["method"].str("GET");
    for (char& c : method) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    request.url = from_js(ctx, argv[0]);
    request.method = method;
    request.body = options["body"].str();
    if (options["bodyBase64"].type == js::Type::String) request.body = base64_decode(options["bodyBase64"].string);
    request.timeout_ms = int(std::clamp(options["timeout"].num(8000), 100.0, 12000.0));
    for (const auto& header : options["headers"].object)
        if (header.second.type == js::Type::String)
            request.headers.push_back(header.first + ": " + header.second.string);
    return true;
}

inline JSValue http_response(JSContext* ctx, const net::Response& r) {
    JSValue out = JS_NewObject(ctx);
    if (JS_IsException(out)) return out;
    if (!r.error.empty()) {
        JS_DefinePropertyValueStr(ctx, out, "error", to_js(ctx, r.error), JS_PROP_C_W_E);
        return out;
    }
    // lx 的响应对象用 statusCode；顺手也给 status，省得脚本挑食
    JS_DefinePropertyValueStr(ctx, out, "statusCode", JS_NewInt32(ctx, r.status), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, out, "status", JS_NewInt32(ctx, r.status), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, out, "statusMessage", to_js(ctx, "OK"), JS_PROP_C_W_E);
    // 标准 JSON 直接进入脚本运行时，避免先建 C++ 树再逐节点复制到 JS。
    JSValue body = JS_ParseJSON(ctx, r.body.c_str(), r.body.size(), "<http-body>");
    if (JS_IsException(body)) {
        JSValue exception = JS_GetException(ctx);
        JSValue name = JS_IsObject(exception) ? JS_GetPropertyStr(ctx, exception, "name") : JS_UNDEFINED;
        const bool syntax = JS_IsString(name) && from_js(ctx, name) == "SyntaxError";
        if (JS_IsException(name)) { JSValue nested = JS_GetException(ctx); JS_FreeValue(ctx, nested); }
        JS_FreeValue(ctx, name);
        if (!syntax) { JS_FreeValue(ctx, out); return JS_Throw(ctx, exception); }
        JS_FreeValue(ctx, exception);
        js::Value parsed;
        body = js::parse(r.body, parsed, true) ? to_js_value(ctx, parsed) : to_js(ctx, r.body);
    }
    if (JS_IsException(body)) { JS_FreeValue(ctx, out); return body; }
    JS_DefinePropertyValueStr(ctx, out, "body", body, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, out, "bytes", JS_NewInt32(ctx, int(r.body.size())), JS_PROP_C_W_E);
    // 原始文本也留一份：有些接口返回单引号 JSON，严格解析会失败，脚本可以自己兜
    JS_DefinePropertyValueStr(ctx, out, "bodyText", to_js(ctx, r.body), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, out, "raw", JS_NewUint8ArrayCopy(ctx,
        reinterpret_cast<const uint8_t*>(r.body.data()), r.body.size()), JS_PROP_C_W_E);
    JSValue h = JS_NewObject(ctx);
    for (const auto& field : r.headers) JS_SetPropertyStr(ctx, h, field.first.c_str(), to_js(ctx, field.second));
    if (!r.set_cookies.empty()) {
        JSValue cookies = JS_NewArray(ctx);
        for (size_t i = 0; i < r.set_cookies.size(); ++i) JS_SetPropertyUint32(ctx, cookies, uint32_t(i), to_js(ctx, r.set_cookies[i]));
        JS_SetPropertyStr(ctx, h, "set-cookie", cookies);
    }
    JS_DefinePropertyValueStr(ctx, h, "content-type", to_js(ctx, r.content_type), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, out, "headers", h, JS_PROP_C_W_E);
    return out;
}

// 平台目录仍提供同步 ctx.http；LX 的 request 单独使用异步桥接，支持 Promise.any / all。
inline JSValue cb_http(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    net::Request request;
    if (!http_request(ctx, argc, argv, request)) return JS_ThrowTypeError(ctx, "invalid HTTP options");
    return http_response(ctx, net::execute(request));
}
inline JSValue cb_http_start(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    net::Request request;
    if (!current || !http_request(ctx, argc, argv, request)) return JS_ThrowTypeError(ctx, "invalid LX request");
    js::Value options;
    js::parse(from_js(ctx, argv[1]), options);
    request.timeout_ms = int(std::clamp(options["timeout"].num(60000), 100.0, 60000.0));
    try {
        const auto id = current->start_http(std::move(request));
        if (!id) return JS_ThrowTypeError(ctx, "too many pending LX requests");
        return JS_NewUint32(ctx, id);
    } catch (const std::exception& e) { return JS_ThrowTypeError(ctx, "%s", e.what()); }
}
inline JSValue cb_http_poll(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    uint32_t id = 0;
    if (!current || argc < 1 || JS_ToUint32(ctx, &id, argv[0]) < 0) return JS_UNDEFINED;
    net::Response response;
    return current->poll_http(id, response) ? http_response(ctx, response) : JS_UNDEFINED;
}
inline JSValue cb_http_cancel(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    uint32_t id = 0;
    if (current && argc && JS_ToUint32(ctx, &id, argv[0]) == 0) current->cancel_http(id);
    return JS_UNDEFINED;
}

inline JSValue cb_binary(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    if (!argc) return JS_NULL;
    size_t size = 0;
    const auto* data = JS_GetUint8Array(ctx, &size, argv[0]);
    if (!data) return JS_EXCEPTION;
    const std::string bytes(reinterpret_cast<const char*>(data), size);
    return to_js(ctx, magic ? md5_hex(bytes) : base64_encode(bytes));
}

inline JSValue cb_md5(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    return to_js(ctx, md5_hex(from_js(ctx, argv[0])));
}

inline JSValue cb_b64e(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    return to_js(ctx, base64_encode(from_js(ctx, argv[0])));
}

inline JSValue cb_b64d(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    const std::string raw = base64_decode(from_js(ctx, argv[0]));
    return JS_NewUint8ArrayCopy(ctx, reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
}

inline JSValue cb_send(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc >= 2 && current) current->on_send(from_js(ctx, argv[0]), from_js(ctx, argv[1]));
    return JS_UNDEFINED;
}

inline JSValue cb_log(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string line;
    for (int i = 0; i < argc; ++i) {
        if (i) line += ' ';
        line += from_js(ctx, argv[i]);
    }
    log_line(line);
    return JS_UNDEFINED;
}

// 脚本死循环使用当前操作的实际时间截止点中断。
inline int interrupt_cb(JSContext*, void* opaque) {
    (void)opaque;
    return net::cancelled() ? 1 : 0;
}

}  // namespace detail

// ---------------------------------------------------------------- lx 垫片（JS 侧）

inline const char* bootstrap_js() {
    return R"JS(
(function () {
  if (globalThis.lx) return;
  const EN = { request: 'request', inited: 'inited', updateAlert: 'updateAlert' };
  const handlers = Object.create(null);
  const timers = new Map();
  const requests = new Map();
  let nextTimer = 1;
  let scope = 0, nextScope = 1, initialized = false;
  function schedule(fn, delay, args, repeat) {
    if (typeof fn !== 'function') throw new TypeError('timer callback must be a function');
    if (timers.size >= 1024) throw new Error('too many pending timers');
    const id = nextTimer++;
    const ms = Math.max(repeat ? 1 : 0, Math.min(2147483647, Number(delay) || 0));
    timers.set(id, { fn, args, due: Date.now() + ms, repeat: repeat ? ms : 0, scope });
    return id;
  }
  // 每次执行一个到期任务，随后由宿主排空 microtask，保持 Promise / 定时器顺序。
  globalThis.__lxRunNext = function () {
    let chosen, task;
    for (const [id, timer] of timers) {
      if (!task || timer.due < task.due) { chosen = id; task = timer; }
    }
    if (!task) return -1;
    const wait = task.due - Date.now();
    if (wait > 0) return Math.min(wait, 20);
    if (task.repeat) task.due = Date.now() + task.repeat;
    else timers.delete(chosen);
    const previous = scope;
    scope = task.scope;
    try { task.fn.apply(undefined, task.args); } finally { scope = previous; }
    return 0;
  };
  globalThis.__lxBeginRequest = function () { scope = nextScope++; };
  globalThis.__lxClearTasks = function () {
    for (const [id, request] of requests) if (request.scope === scope) request.abort();
    for (const [id, timer] of timers) if (timer.scope === scope) timers.delete(id);
    scope = 0;
  };

  function utf8Encode(str) {
    const out = [];
    for (const ch of str) {
      const c = ch.codePointAt(0);
      if (c < 0x80) out.push(c);
      else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 63));
      else if (c < 0x10000) out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
      else out.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
    }
    return new Uint8Array(out);
  }
  function utf8Decode(bytes) {
    let out = '';
    for (let i = 0; i < bytes.length;) {
      const b = bytes[i++];
      let c;
      if (b < 0x80) c = b;
      else if (b < 0xe0) c = ((b & 31) << 6) | (bytes[i++] & 63);
      else if (b < 0xf0) c = ((b & 15) << 12) | ((bytes[i++] & 63) << 6) | (bytes[i++] & 63);
      else c = ((b & 7) << 18) | ((bytes[i++] & 63) << 12) | ((bytes[i++] & 63) << 6) | (bytes[i++] & 63);
      out += String.fromCodePoint(c);
    }
    return out;
  }
  function bytesOf(input, encoding) {
    const enc = (encoding || 'utf8').toLowerCase();
    if (input instanceof Uint8Array) return input;
    if (input instanceof ArrayBuffer) return new Uint8Array(input);
    if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
    if (Array.isArray(input)) return new Uint8Array(input);
    const s = String(input == null ? '' : input);
    if (enc === 'base64') return __lx.b64decode(s);
    if (enc === 'hex') {
      const out = new Uint8Array(Math.floor(s.length / 2));
      for (let i = 0; i < out.length; i++) out[i] = parseInt(s.substr(i * 2, 2), 16);
      return out;
    }
    if (enc === 'binary' || enc === 'latin1') {
      const out = new Uint8Array(s.length);
      for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i) & 0xff;
      return out;
    }
    return utf8Encode(s);
  }
  function binOf(bytes) {
    let s = '';
    for (let i = 0; i < bytes.length; i += 4096)
      s += String.fromCharCode.apply(null, bytes.subarray(i, i + 4096));
    return s;
  }
  function textOf(bytes, encoding) {
    const enc = (encoding || 'utf8').toLowerCase();
    if (enc === 'base64') return __lx.b64bytes(bytes);
    if (enc === 'hex') return Array.from(bytes).map(b => b.toString(16).padStart(2, '0')).join('');
    if (enc === 'binary' || enc === 'latin1') return binOf(bytes);
    return utf8Decode(bytes);
  }

  const utils = {
    buffer: {
      from: function (input, encoding) { return new Uint8Array(bytesOf(input, encoding)); },
      bufToString: function (buf, encoding) { return textOf(bytesOf(buf), encoding); }
    },
    crypto: {
      md5: function (s) { return __lx.md5bytes(bytesOf(s)); },
      randomBytes: function (n) {
        const a = new Uint8Array(n | 0);
        for (let i = 0; i < a.length; i++) a[i] = Math.floor(Math.random() * 256);
        return a;
      },
      aesEncrypt: function () { throw new Error('lx: 本运行时未实现 aesEncrypt'); },
      aesDecrypt: function () { throw new Error('lx: 本运行时未实现 aesDecrypt'); },
      rsaEncrypt: function () { throw new Error('lx: 本运行时未实现 rsaEncrypt'); }
    }
  };

  const lx = {
    EVENT_NAMES: EN,
    version: '2.0.0',
    currentScriptInfo: (function () { const info = __lx.scriptInfo; delete __lx.scriptInfo; return info; })(),
    env: 'desktop',
    on: function (name, fn) {
      if (__lx.debug) __lx.log('TRACE on(' + name + ') fn=' + (typeof fn));
      if (typeof fn !== 'function') return Promise.reject(new TypeError('event handler must be a function'));
      handlers[name] = fn;
      return Promise.resolve();
    },
    send: function (name, data) {
      if (__lx.debug) __lx.log('TRACE send(' + name + ')');
      return new Promise(function (resolve, reject) {
        if (name !== EN.inited && name !== EN.updateAlert) return reject(new Error('unknown event: ' + name));
        if (name === EN.inited) {
          if (initialized) return reject(new Error('already initialized'));
          initialized = true;
        }
        __lx.send(String(name), JSON.stringify(data));
        resolve();
      });
    },
    request: function (url, options, callback) {
      if (typeof options === 'function') { callback = options; options = {}; }
      let aborted = false, id = 0, timer = 0;
      const requestScope = scope;
      const abort = function () {
        aborted = true; timers.delete(timer);
        if (id) { __lx.httpCancel(id); requests.delete(id); }
      };
      abort.abort = abort;
      try {
        const opts = Object.assign({}, options || {});
        if (opts.headers) {
          opts.headers = Object.assign({}, opts.headers);
          Object.keys(opts.headers).forEach(k => { opts.headers[k] = String(opts.headers[k]); });
        }
        if (opts.form && typeof opts.form === 'object') {
          opts.body = Object.keys(opts.form).map(k => encodeURIComponent(k) + '=' + encodeURIComponent(opts.form[k])).join('&');
          opts.headers = Object.assign({}, opts.headers || {});
          if (!Object.keys(opts.headers).some(k => k.toLowerCase() === 'content-type'))
            opts.headers['Content-Type'] = 'application/x-www-form-urlencoded';
          if (!opts.method) opts.method = 'POST';
        }
        if (ArrayBuffer.isView(opts.body) || opts.body instanceof ArrayBuffer) {
          opts.bodyBase64 = textOf(bytesOf(opts.body), 'base64'); delete opts.body;
        } else if (opts.body && typeof opts.body === 'object') {
          opts.body = JSON.stringify(opts.body);
          opts.headers = Object.assign({'Content-Type': 'application/json'}, opts.headers || {});
        }
        id = __lx.httpStart(String(url), JSON.stringify(opts));
        requests.set(id, {scope: requestScope, abort});
      } catch (e) { schedule(function () { if (!aborted && callback) callback(e); }, 0, [], false); return abort; }
      function poll() {
        if (aborted) return;
        const res = __lx.httpPoll(id);
        if (res === undefined) { timer = schedule(poll, 5, [], false); return; }
        requests.delete(id);
        if (res && res.error) { if (callback) callback(new Error(String(res.error))); return; }
        if (callback) callback(null, res, res && res.body);
      }
      timer = schedule(poll, 0, [], false);
      return abort;
    },
    utils: utils
  };

  globalThis.__lxHandlers = handlers;
  function traced(obj, prefix) {
    return new Proxy(obj, {
      get: function (t, k) {
        if (typeof k === 'string') {
          const v = t[k];
          __lx.log('TRACE ' + prefix + '.' + k + ' -> ' + (v === undefined ? 'undefined' : typeof v));
          if (v && typeof v === 'object' && !(v instanceof Uint8Array)) return traced(v, prefix + '.' + k);
        }
        return t[k];
      }
    });
  }
  if (__lx.debug) {
    lx.utils = traced(lx.utils, 'lx.utils');
    globalThis.lx = new Proxy(lx, {
      get: function (t, k) {
        if (typeof k === 'string') {
          const v = t[k];
          __lx.log('TRACE lx.' + k + ' -> ' + (v === undefined ? 'undefined' : typeof v));
        }
        return t[k];
      }
    });
  } else {
    globalThis.lx = lx;
  }
  function __logArgs() { __lx.log(Array.prototype.join.call(arguments, ' ')); }
  globalThis.console = {
    log: __logArgs, warn: __logArgs, error: __logArgs, info: __logArgs,
    debug: __logArgs, trace: __logArgs, dir: __logArgs,
    group: function () {}, groupEnd: function () {}, groupCollapsed: function () {},
    table: function () {}, time: function () {}, timeEnd: function () {}, timeLog: function () {},
    count: function () {}, countReset: function () {}, clear: function () {},
    assert: function (cond) { if (!cond) __logArgs('Assertion failed'); }
  };
  // 最小 DOM 垫片：不少音源把注册逻辑挂在 DOMContentLoaded / load 上，
  // 而它们跑在浏览器的 BrowserWindow 里。这里不模拟 DOM，只是把回调立刻执行。
  function fakeEventTarget() {
    const map = Object.create(null);
    return {
      addEventListener: function (name, fn) {
        if (typeof fn === 'function') { (map[name] || (map[name] = [])).push(fn); }
      },
      removeEventListener: function (name, fn) {
        const list = map[name];
        if (!list) return;
        const i = list.indexOf(fn);
        if (i >= 0) list.splice(i, 1);
      },
      dispatch: function (name, ev) {
        const list = map[name];
        if (!list) return;
        for (const fn of list.slice()) { try { fn(ev || { type: name }); } catch (e) { __lx.log('listener ' + name + ': ' + e); } }
      }
    };
  }
  const domDoc = fakeEventTarget();
  domDoc.readyState = 'complete';
  domDoc.hidden = false;
  domDoc.visibilityState = 'visible';
  domDoc.createElement = function () { return { style: {}, appendChild: function () {}, setAttribute: function () {}, addEventListener: function () {} }; };
  domDoc.querySelector = function () { return null; };
  domDoc.body = { appendChild: function () {}, style: {} };
  domDoc.documentElement = { style: {} };
  domDoc.head = { appendChild: function () {} };
  globalThis.document = domDoc;
  globalThis.window = globalThis;
  globalThis.navigator = { userAgent: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AinoPlayer', platform: 'Win32', language: 'zh-CN' };
  globalThis.location = { href: 'file:///user-api.html', hostname: '', origin: 'null' };
  globalThis.self = globalThis;
  globalThis.addEventListener = domDoc.addEventListener;
  globalThis.removeEventListener = domDoc.removeEventListener;
  if (typeof globalThis.window !== 'undefined') {
    globalThis.window.addEventListener = domDoc.addEventListener;
    globalThis.window.removeEventListener = domDoc.removeEventListener;
  }
  globalThis.__lxDomEvents = domDoc;   // 脚本加载完后由宿主派发事件（顺序关键）

  globalThis.setTimeout = function (fn, delay, ...args) { return schedule(fn, delay, args, false); };
  globalThis.clearTimeout = function (id) { timers.delete(id); };
  globalThis.setInterval = function (fn, delay, ...args) { return schedule(fn, delay, args, true); };
  globalThis.clearInterval = globalThis.clearTimeout;
  globalThis.atob = function (s) { return binOf(__lx.b64decode(String(s))); };
  globalThis.btoa = function (s) { return __lx.b64encode(String(s)); };
})();
)JS";
}

// ---------------------------------------------------------------- 实现

inline void Script::on_send(const std::string& event, const std::string& json) {
    if (std::getenv("LX_TRACE")) std::fprintf(stderr, "[lx][send] %s %.400s\n", event.c_str(), json.c_str());
    // 发布后声明只读，防止取流线程再次发送 inited 与其它消费者并发修改它。
    if (event != "inited" || declarations_frozen_) return;
    js::Value root;
    if (!js::parse(json, root, /*lenient=*/true)) {
        error_ = "inited 消息不是合法 JSON";
        return;
    }
    // status 是可选的：lx 真机只在"明确 false"时才算失败，
    // 六音这里干脆不带 status，只看 sources 有没有东西。
    const js::Value& status = root["status"];
    if (!status.is_null() && !status.truthy()) {
        sources_.clear(); inited_ = false;
        const std::string msg = root["message"].str();
        error_ = "脚本报告初始化失败" + (msg.empty() ? "" : (": " + msg));
        return;
    }
    const js::Value& srcs = root["sources"];
    if (!srcs.is_object()) {
        error_ = "inited 里没有 sources";
        return;
    }
    sources_.clear();
    for (const auto& kv : srcs.object) {
        Decl d;
        d.key = kv.first;
        d.name = kv.second["name"].str(kv.first);
        for (size_t i = 0; i < kv.second["actions"].size(); ++i)
            if (!kv.second["actions"][i].str().empty()) d.actions.push_back(kv.second["actions"][i].str());
        for (size_t i = 0; i < kv.second["qualitys"].size(); ++i)
            if (!kv.second["qualitys"][i].str().empty()) d.qualitys.push_back(kv.second["qualitys"][i].str());
        if (d.actions.empty()) d.actions.push_back("musicUrl");
        sources_.push_back(std::move(d));
    }
    inited_ = true;
}

inline void Script::close() {
    std::lock_guard<std::timed_mutex> lock(execution_);
    for (auto& task : http_) task.second.cancel->store(true);
    http_.clear(); // 网络线程只持有自身请求，退出后再释放运行时。
    if (ctx_) {
        JS_UpdateStackTop(rt_);
        // 销毁不再执行用户任务：挂起任务可能再次联网，移除和退出不能因此阻塞。
        JS_FreeValue(ctx_, handlers_);
        handlers_ = JS_UNDEFINED;
        JS_FreeContext(ctx_);
        ctx_ = nullptr;
    }
    if (rt_) {
        JS_FreeRuntime(rt_);
        rt_ = nullptr;
    }
}

inline bool Script::eval(const std::string& code, const char* name, std::string& err) {
    JSValue v = JS_Eval(ctx_, code.c_str(), code.size(), name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        err = detail::take_exception(ctx_);
        JS_FreeValue(ctx_, v);
        return false;
    }
    JS_FreeValue(ctx_, v);
    return true;
}

inline bool Script::load(const std::string& path, const std::string& script, std::string& err) {
    Operation operation(*this, 12000);
    if (!operation) { err = "脚本加载已取消或超时"; return false; }
    info_.file = path;
    info_.fingerprint = detail::md5_hex(script);
    const auto head = parse_header(script);
    info_.name = head.count("name") ? head.at("name") : path;
    info_.version = head.count("version") ? head.at("version") : "";
    info_.author = head.count("author") ? head.at("author") : "";
    info_.description = head.count("description") ? head.at("description") : "";
    info_.homepage = head.count("homepage") ? head.at("homepage") : "";

    rt_ = JS_NewRuntime();
    if (!rt_) { err = "创建 JS 运行时失败"; return false; }
    JS_SetMemoryLimit(rt_, 512u * 1024u * 1024u);
    JS_SetInterruptHandler(rt_, detail::interrupt_cb, nullptr);

    ctx_ = JS_NewContext(rt_);
    if (!ctx_) { err = "创建 JS 上下文失败"; return false; }

    detail::current = this;
    struct Guard {
        ~Guard() { detail::current = nullptr; }
    } guard;

    JSValue gl = JS_GetGlobalObject(ctx_);
    JSValue api = JS_NewObject(ctx_);
    JS_SetPropertyStr(ctx_, api, "http", JS_NewCFunction(ctx_, detail::cb_http, "http", 2));
    JS_SetPropertyStr(ctx_, api, "httpStart", JS_NewCFunction(ctx_, detail::cb_http_start, "httpStart", 2));
    JS_SetPropertyStr(ctx_, api, "httpPoll", JS_NewCFunction(ctx_, detail::cb_http_poll, "httpPoll", 1));
    JS_SetPropertyStr(ctx_, api, "httpCancel", JS_NewCFunction(ctx_, detail::cb_http_cancel, "httpCancel", 1));
    JS_SetPropertyStr(ctx_, api, "b64bytes", JS_NewCFunctionMagic(ctx_, detail::cb_binary, "b64bytes", 1, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx_, api, "md5bytes", JS_NewCFunctionMagic(ctx_, detail::cb_binary, "md5bytes", 1, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx_, api, "md5", JS_NewCFunction(ctx_, detail::cb_md5, "md5", 1));
    JS_SetPropertyStr(ctx_, api, "b64encode", JS_NewCFunction(ctx_, detail::cb_b64e, "b64encode", 1));
    JS_SetPropertyStr(ctx_, api, "b64decode", JS_NewCFunction(ctx_, detail::cb_b64d, "b64decode", 1));
    JS_SetPropertyStr(ctx_, api, "send", JS_NewCFunction(ctx_, detail::cb_send, "send", 2));
    JS_SetPropertyStr(ctx_, api, "log", JS_NewCFunction(ctx_, detail::cb_log, "log", 1));
    JS_SetPropertyStr(ctx_, api, "debug", JS_NewBool(ctx_, std::getenv("LX_TRACE") != nullptr));
    {
        // 直接传对象，不再转义整份源代码、构造大 JSON 字符串后再解析一遍。
        JSValue info = JS_NewObject(ctx_);
        for (const auto& field : {std::make_pair("name", &info_.name), std::make_pair("version", &info_.version),
             std::make_pair("author", &info_.author), std::make_pair("description", &info_.description), std::make_pair("homepage", &info_.homepage)})
            JS_SetPropertyStr(ctx_, info, field.first, detail::to_js(ctx_, *field.second));
        JS_SetPropertyStr(ctx_, info, "rawScript", detail::to_js(ctx_, script));
        JS_SetPropertyStr(ctx_, api, "scriptInfo", info);
    }
    JS_SetPropertyStr(ctx_, gl, "__lx", api);
    JS_FreeValue(ctx_, gl);

    if (!eval(bootstrap_js(), "<lx-bootstrap>", err)) return false;
    if (!eval(script, path.c_str(), err)) {
        err = "脚本执行失败：" + err;
        return false;
    }
    // 脚本注册完监听器之后再派发，顺序反了就一个回调都不会触发
    eval("try{__lxDomEvents.dispatch('DOMContentLoaded');__lxDomEvents.dispatch('load');"
         "__lxDomEvents.dispatch('readystatechange');}catch(e){__lx.log('dom dispatch: '+e)}",
         "<lx-dom>", err);
    if (net::cancelled()) { err = "脚本加载已取消或超时"; return false; }
    return true;
}

// 1=处理了任务或等待到下一定时器，0=无任务，-1=执行异常。
inline int Script::pump(std::string& err) {
    JSContext* context = nullptr;
    const int jobs = JS_ExecutePendingJob(rt_, &context);
    if (jobs < 0) { err = detail::take_exception(context ? context : ctx_); return -1; }
    if (jobs > 0) return 1;
    JSValue global = JS_GetGlobalObject(ctx_);
    JSValue fn = JS_GetPropertyStr(ctx_, global, "__lxRunNext");
    JS_FreeValue(ctx_, global);
    JSValue result = JS_Call(ctx_, fn, JS_UNDEFINED, 0, nullptr);
    JS_FreeValue(ctx_, fn);
    if (JS_IsException(result)) { err = detail::take_exception(ctx_); JS_FreeValue(ctx_, result); return -1; }
    int32_t wait = -1;
    JS_ToInt32(ctx_, &wait, result);
    JS_FreeValue(ctx_, result);
    if (wait < 0) return 0;
    if (wait > 0) std::this_thread::sleep_for(std::chrono::milliseconds(std::min(wait, 20)));
    return 1;
}

inline bool Script::call_handler(const std::string& event, JSValueConst arg, JSValue& out, std::string& err) {
    JSValue fn = JS_GetPropertyStr(ctx_, handlers_, event.c_str());
    if (!JS_IsFunction(ctx_, fn)) {
        JS_FreeValue(ctx_, fn);
        err = "脚本没有注册 " + event + " 处理器";
        return false;
    }
    // 注意：有的音源只在加载阶段直接 send('inited')，根本不注册 on('inited')，
    // 所以调用方要把"没有 handler"当成正常情况处理。
    JSValue r = JS_Call(ctx_, fn, JS_UNDEFINED, 1, &arg);
    JS_FreeValue(ctx_, fn);
    if (JS_IsException(r)) {
        err = detail::take_exception(ctx_);
        JS_FreeValue(ctx_, r);
        return false;
    }
    out = r;
    return settle(out, err);
}

inline bool Script::settle(JSValue& value, std::string& err) {
    if (!JS_IsPromise(value)) return true;
    for (;;) {
        if (net::cancelled()) break;
        if (JS_PromiseState(ctx_, value) != JS_PROMISE_PENDING) break;
        const int step = pump(err);
        if (step < 0) { JS_FreeValue(ctx_, value); value = JS_UNDEFINED; return false; }
        if (!step) break;
    }
    const JSPromiseStateEnum st = JS_PromiseState(ctx_, value);
    if (st == JS_PROMISE_REJECTED) {
        JSValue e = JS_PromiseResult(ctx_, value);
        err = detail::from_js(ctx_, e);
        if (err.empty()) err = "脚本返回的 Promise 被拒绝";
        JS_FreeValue(ctx_, e);
        JS_FreeValue(ctx_, value);
        value = JS_UNDEFINED;
        return false;
    }
    if (st == JS_PROMISE_PENDING) {
        err = net::cancelled() ? "脚本请求已取消或超时" : "脚本的 Promise 未完成（可能依赖尚未支持的异步事件）";
        JS_FreeValue(ctx_, value);
        value = JS_UNDEFINED;
        return false;
    }
    JSValue r = JS_PromiseResult(ctx_, value);
    JS_FreeValue(ctx_, value);
    value = r;
    return true;
}

inline bool Script::init(std::string& err) {
    Operation operation(*this, 12000);
    if (!operation) { err = "脚本初始化已取消或超时"; return false; }
    if (!ctx_) { err = "脚本未加载"; return false; }
    // 上下文可能在别的线程创建；QuickJS 的栈上限是按创建线程算的，
    // 不刷新就会在调用线程上误判"Maximum call stack size exceeded"。
    JS_UpdateStackTop(rt_);
    detail::current = this;
    struct Guard {
        ~Guard() { detail::current = nullptr; }
    } guard;

    if (!JS_IsUndefined(handlers_)) JS_FreeValue(ctx_, handlers_);
    JSValue gl = JS_GetGlobalObject(ctx_);
    handlers_ = JS_GetPropertyStr(ctx_, gl, "__lxHandlers");
    JS_FreeValue(ctx_, gl);

    JSValue arg = JS_NewObject(ctx_);
    JS_DefinePropertyValueStr(ctx_, arg, "status", JS_NewBool(ctx_, true), JS_PROP_C_W_E);
    JSValue noop = JS_NewCFunction(ctx_, [](JSContext*, JSValueConst, int, JSValueConst*) { return JS_UNDEFINED; },
                                  "openDevTools", 0);
    JS_DefinePropertyValueStr(ctx_, arg, "openDevTools", noop, JS_PROP_C_W_E);

    // 两种风格都要支持：
    //   A. 注册 on('inited')，由客户端触发（六音早期版本就是这种）
    //   B. 加载时自己 send('inited')，不注册 handler（六音 v1.2.1 这种）
    JSValue out = JS_UNDEFINED;
    std::string call_err;
    JSValue fn = JS_GetPropertyStr(ctx_, handlers_, "inited");
    if (JS_IsFunction(ctx_, fn)) {
        call_handler("inited", arg, out, call_err);
        JS_FreeValue(ctx_, out);
        out = JS_UNDEFINED;
    }
    JS_FreeValue(ctx_, fn);
    JS_FreeValue(ctx_, arg);

    // 已声明就绪的脚本不必等待可选的更新检查；异步初始化则处理网络任务和真实定时器。
    // send(inited).then(() => on(request)) 也合法，先排空 microtask 再判断是否初始化完成。
    while (!net::cancelled()) {
        JSContext* job_context = nullptr;
        const int jobs = JS_ExecutePendingJob(rt_, &job_context);
        if (jobs < 0) { call_err = detail::take_exception(job_context ? job_context : ctx_); break; }
        if (jobs > 0) continue;
        fn = JS_GetPropertyStr(ctx_, handlers_, "request");
        const bool complete = ready() && JS_IsFunction(ctx_, fn);
        JS_FreeValue(ctx_, fn);
        if (complete) break;
        if (pump(call_err) <= 0) break;
    }
    if (net::cancelled()) { err = "脚本初始化已取消或超时"; return false; }

    if (!ready()) {
        err = !error_.empty() ? error_ : (call_err.empty() ? "脚本没有声明任何平台" : call_err);
        return false;
    }
    fn = JS_GetPropertyStr(ctx_, handlers_, "request");
    const bool has_request = JS_IsFunction(ctx_, fn);
    JS_FreeValue(ctx_, fn);
    if (!has_request) { err = "脚本未注册取流处理器"; return false; }
    inited_ = true;
    declarations_frozen_ = true;
    JS_RunGC(rt_);
    return true;
}

inline bool Script::music_url(const std::string& platform, const std::string& quality,
                              const std::string& music_info_json, std::string& url, std::string& err, bool background) {
    struct Priority {
        std::atomic<unsigned>& counter; bool active;
        Priority(std::atomic<unsigned>& c, bool a) : counter(c), active(a) { if (active) ++counter; }
        ~Priority() { if (active) --counter; }
    } priority(foreground_, !background);
    Operation operation(*this, background ? 6500 : 12000);
    if (!operation) { err = "脚本繁忙、已移除或取流超时"; return false; }
    if (!ctx_ || !inited_) { err = "脚本未就绪"; return false; }
    JS_UpdateStackTop(rt_);   // 同上：解析常常发生在别的线程
    if (!eval("__lxBeginRequest()", "<begin-request>", err)) return false;
    struct Tasks { Script& script; ~Tasks() { script.clear_tasks(); } } tasks{*this};

    const std::string payload = "{\"source\":" + detail::js_quote(platform) + ",\"action\":\"musicUrl\",\"info\":{\"type\":" +
        detail::js_quote(quality) + ",\"musicInfo\":" + (music_info_json.empty() ? "{}" : music_info_json) + "}}";
    JSValue arg = JS_ParseJSON(ctx_, payload.c_str(), payload.size(), "<musicUrl>");
    if (JS_IsException(arg)) {
        err = "构造 musicUrl 参数失败: " + detail::take_exception(ctx_);
        JS_FreeValue(ctx_, arg);
        return false;
    }
    JSValue out = JS_UNDEFINED;
    const bool ok = call_handler("request", arg, out, err);
    JS_FreeValue(ctx_, arg);
    if (!ok) { JS_FreeValue(ctx_, out); return false; }

    if (JS_IsString(out)) {
        url = detail::from_js(ctx_, out);
    } else if (JS_IsObject(out)) {
        JSValue u = JS_GetPropertyStr(ctx_, out, "url");
        url = detail::from_js(ctx_, u);
        JS_FreeValue(ctx_, u);
    }
    JS_FreeValue(ctx_, out);

    if (net::cancelled()) { err = "脚本请求已取消或超过 12 秒"; return false; }
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        if (err.empty()) err = url.empty() ? "脚本没有返回直链" : ("无效直链: " + url);
        return false;
    }
    return true;
}


// 音源脚本目录：AINO_LX_DIR 优先；其余平台统一使用用户配置目录。
inline std::string default_dir() {
    if (const char* e = std::getenv("AINO_LX_DIR")) {
        if (*e) return e;
    }
    const auto config = platform::config_dir();
    return config.empty() ? std::string{} : platform::join(config, "sources");
}

// ---------------------------------------------------------------- 音源集合

// 一个目录里的音源脚本；运行时逐个串行执行，集合支持后台热替换与应用内移除。
class Engine {
public:
    struct Check { std::string platform, info; };
    struct Health {
        // 0 等待检测，1 检测中，2 样本通过，3 样本失败，4 未声明支持，5 超时未完成。
        int state = 0;
        std::string reason;
        int resolve_ms = 0, load_ms = 0;
        std::string quality;
        unsigned long long checked_revision = 0;
    };
    struct Timing {
        int resolve_ms = 0, load_ms = 0, ready_ms = 0;
        unsigned samples = 0, failures = 0;
        unsigned long long failed_at = 0;
        unsigned long long verified_revision = 0;
        bool verified = false;
        bool cached = false;
    };
    // 消费方只报告“已收到有效音频 / 首段可播”，不把整首下载或转码耗时当加载速度。
    struct LoadTiming { int ready_ms = 0; bool verified = false; };
    using Timings = std::map<std::string, std::map<std::string, Timing>>; // 平台 → 实际音质 → 测量
    using Checks = std::map<std::string, Health>;
    struct Selection {
        std::string file, name, quality;
        // 0 已验证，1 未验证，2 样本失败，3 不支持音质，4 临时网络冷却。
        int availability = 1, latency_ms = 300001;
        bool estimated = false;
    };
    struct FixedSource {
        std::string file, name;
        int verified_platforms = 0, supported_platforms = 0, latency_ms = 300001;
    };
    static bool fixed_source_better(const FixedSource& candidate, const FixedSource& best) {
        if (candidate.verified_platforms != best.verified_platforms) return candidate.verified_platforms > best.verified_platforms;
        if (!candidate.verified_platforms && candidate.supported_platforms != best.supported_platforms)
            return candidate.supported_platforms > best.supported_platforms;
        if (candidate.latency_ms != best.latency_ms) return candidate.latency_ms < best.latency_ms;
        if (candidate.supported_platforms != best.supported_platforms) return candidate.supported_platforms > best.supported_platforms;
        return candidate.file < best.file;
    }
    // 排序与界面共用。历史成功不能覆盖更新的失败；不同音质的样本只作明确标注的估算。
    static Selection selection_score(const std::string& quality, const Health* health,
                                     const Timing* exact, const Timing* fallback, unsigned long long now) {
        (void)now;
        Selection out;
        out.quality = quality;
        if (quality.empty()) { out.availability = 3; return out; }
        // 样本失败影响平台排名；单曲错误和未完成检测不能判定整个平台失效。
        const bool failed_check = health && health->state == 3 &&
            health->quality == quality && (!exact || exact->verified_revision <= health->checked_revision);
        const bool passed = (exact && exact->verified) || (health && health->state == 2 && health->quality == quality);
        out.availability = failed_check ? 2 : passed ? 0 : 1;
        // 先比较同一口径的样本延迟，不能把一个源的首段解码时间与另一个的 64 KiB 下载直接混排。
        if (exact && exact->load_ms > 0) { out.latency_ms = exact->load_ms; out.estimated = exact->cached; }
        else if (health && health->state == 2 && health->load_ms > 0) {
            out.latency_ms = health->load_ms;
            out.estimated = health->quality != quality;
        } else if (fallback && fallback->load_ms > 0) {
            out.latency_ms = fallback->load_ms;
            out.estimated = true;
        } else if (exact && exact->ready_ms > 0) {
            out.latency_ms = exact->ready_ms;
            out.estimated = true;
        }
        return out;
    }
    std::vector<Selection> selection_order(const std::string& platform, int wanted) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<Selection> result;
        for (const auto& script : list_) if (!script->retired() && script->supports(platform, "musicUrl"))
            result.push_back(selection_locked(script, platform, wanted));
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
            return std::tie(a.availability, a.latency_ms, a.file) < std::tie(b.availability, b.latency_ms, b.file);
        });
        return result;
    }
    explicit Engine(std::string preferences_file = {}) : preferences_file_(std::move(preferences_file)) {}
    std::vector<std::string> manual_order() const {
        std::lock_guard<std::mutex> lock(mu_);
        auto result = manual_order_;
        if (result.empty() && !fixed_source_.file.empty()) result.push_back(fixed_source_.file);
        for (const auto& script : list_) {
            const auto file = file_key(script->path());
            if (!script->retired() && std::find(result.begin(), result.end(), file) == result.end()) result.push_back(file);
        }
        return result;
    }
    bool set_manual_order(const std::vector<std::string>& files, std::string& error) {
        if (!read_preferences()) { error = preferences_error(); return false; }
        std::lock_guard<std::mutex> state(state_io_);
        std::vector<std::string> order;
        for (const auto& file : files) {
            const auto key = file_key(file);
            if (!key.empty() && std::find(order.begin(), order.end(), key) == order.end()) order.push_back(key);
        }
        std::string text = "[";
        for (const auto& file : order) { if (text.size() > 1) text += ','; text += detail::js_quote(file); }
        text += "]\n";
        const auto path = order_path();
        std::FILE* output = path.empty() ? nullptr : platform::fopen(path + ".tmp", "wb");
        bool ok = false;
        if (output) {
            const bool written = std::fwrite(text.data(), 1, text.size(), output) == text.size();
            const bool closed = std::fclose(output) == 0;
            ok = written && closed && platform::replace_file(path + ".tmp", path);
        }
        if (!ok) { if (!path.empty()) platform::remove(path + ".tmp"); error = "保存音源顺序失败，顺序未改变"; return false; }
        std::lock_guard<std::mutex> lock(mu_);
        manual_order_ = std::move(order); fixed_source_ = {}; ++revision_;
        return true;
    }
    FixedSource fixed_source() const { std::lock_guard<std::mutex> lock(mu_); return fixed_source_; }
    FixedSource choose_fixed_source(int wanted, bool replace = false) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!replace && !fixed_source_.file.empty() && std::any_of(list_.begin(), list_.end(), [&](const auto& script) {
            return !script->retired() && file_key(script->path()) == fixed_source_.file;
        })) return fixed_source_;
        FixedSource best;
        for (const auto& script : list_) {
            if (script->retired()) continue;
            FixedSource choice; choice.file = file_key(script->path()); choice.name = script->info().name;
            long long verified_ms = 0, estimated_ms = 0;
            int estimates = 0;
            for (const auto* platform : {"wy", "tx", "kw", "kg", "mg"}) {
                if (!script->supports(platform, "musicUrl")) continue;
                const auto score = selection_locked(script, platform, wanted);
                if (score.availability >= 2 && std::find(manual_order_.begin(), manual_order_.end(), choice.file) == manual_order_.end()) continue;
                ++choice.supported_platforms;
                if (score.availability == 0) { ++choice.verified_platforms; verified_ms += score.latency_ms; }
                else if (score.availability == 1 && score.latency_ms < 300001) { ++estimates; estimated_ms += score.latency_ms; }
            }
            if (!choice.supported_platforms) continue;
            if (choice.verified_platforms) choice.latency_ms = int(verified_ms / choice.verified_platforms);
            else if (estimates) choice.latency_ms = int(estimated_ms / estimates);
            const auto position = [&](const auto& file) { return std::find(manual_order_.begin(), manual_order_.end(), file) - manual_order_.begin(); };
            if (best.file.empty() || (!manual_order_.empty() && position(choice.file) < position(best.file)) ||
                (position(choice.file) == position(best.file) && fixed_source_better(choice, best))) best = std::move(choice);
        }
        fixed_source_ = best;
        ++revision_;
        return fixed_source_;
    }
    void update_network(const std::string& network) {
        if (network.empty()) return;
        const auto signature = detail::md5_hex(network);
        std::lock_guard<std::mutex> lock(mu_);
        if (!network_hash_.empty() && network_hash_ != signature) {
            timings_.clear(); health_.clear(); song_failures_.clear(); platform_retry_at_.clear(); ++revision_;
        }
        network_hash_ = signature;
    }
    void save_metrics(const std::string& network) {
        if (network.empty()) return;
        update_network(network);
        std::string text = "{\"version\":1,\"network\":" + detail::js_quote(detail::md5_hex(network)) +
            ",\"time\":" + std::to_string(std::time(nullptr)) + ",\"scripts\":[";
        {
            std::lock_guard<std::mutex> lock(mu_);
            bool first = true;
            for (const auto& script : list_) {
                if (!first) text += ','; first = false;
                text += "{\"file\":" + detail::js_quote(file_key(script->path())) + ",\"hash\":" + detail::js_quote(script->info().fingerprint) + ",\"timings\":[";
                bool first_timing = true;
                const auto found = timings_.find(file_key(script->path()));
                if (found != timings_.end()) for (const auto& platform : found->second) for (const auto& quality : platform.second) {
                    const auto& value = quality.second;
                    if (!value.verified || value.failures) continue;
                    if (!first_timing) text += ','; first_timing = false;
                    text += "{\"platform\":" + detail::js_quote(platform.first) + ",\"quality\":" + detail::js_quote(quality.first) +
                        ",\"load\":" + std::to_string(value.load_ms) + ",\"ready\":" + std::to_string(value.ready_ms) + ",\"resolve\":" + std::to_string(value.resolve_ms) + "}";
                }
                text += "]}";
            }
        }
        text += "]}";
        if (text.size() > 1024 * 1024) return;
        const auto path = platform::join(platform::config_dir(), "source-timings.json"), temporary = path + ".tmp";
        auto* output = platform::fopen(temporary, "wb");
        if (!output) return;
        const bool written = std::fwrite(text.data(), 1, text.size(), output) == text.size();
        const bool closed = std::fclose(output) == 0;
        if (!written || !closed || !platform::replace_file(temporary, path)) platform::remove(temporary);
    }
    void restore_metrics(const std::string& network) {
        if (network.empty()) return;
        update_network(network);
        const auto path = platform::join(platform::config_dir(), "source-timings.json");
        const auto size = platform::file_size(path);
        if (size <= 0 || size > 1024 * 1024) return;
        std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(input)), {});
        js::Value root;
        if (!js::parse(text, root) || root["version"].integer() != 1 || root["network"].str() != detail::md5_hex(network)) return;
        const auto age = static_cast<long long>(std::time(nullptr)) - root["time"].integer();
        if (age < 0 || age > 6 * 3600 || root["scripts"].size() > 256) return;
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& row : root["scripts"].array) for (const auto& script : list_) {
            const auto file = file_key(script->path());
            if (row["file"].str() != file || row["hash"].str() != script->info().fingerprint || row["timings"].size() > 32) continue;
            for (const auto& metric : row["timings"].array) {
                const auto platform = metric["platform"].str(), quality = metric["quality"].str();
                if (!script->supports(platform, "musicUrl")) continue;
                if (quality != "128k" && quality != "320k" && quality != "flac" && quality != "flac24bit") continue;
                auto& timing = timings_[file][platform][quality];
                if (timing.samples || timing.failures) continue;
                timing.load_ms = int(std::clamp(metric["load"].integer(), 0LL, 300000LL));
                timing.ready_ms = int(std::clamp(metric["ready"].integer(), 0LL, 300000LL));
                timing.resolve_ms = int(std::clamp(metric["resolve"].integer(), 0LL, 300000LL));
                // 跨启动只作速度估算，不伪造当前网络已通过的音频验证。
                timing.samples = 1;
                timing.cached = true;
            }
        }
        ++revision_;
    }
    static Engine& instance() {
        static Engine e;
        return e;
    }

    // 扫目录里所有 .js，逐个加载 + 初始化。返回成功个数。
    // 加载全程【不持 mu_】：脚本 init 会发同步网络请求（单个超时可达十余秒，
    // 19 个源可能要几十秒），期间界面每帧都在读 entries()/failures()——
    // 若加载持锁，UI 线程会死等这把锁，窗口假死。所以先在局部建好新列表，
    // 最后一刻才持锁换入；加载期间 resolve() 继续用旧列表（热替换语义）。
    int load_dir(const std::string& dir) {
        if (dir.empty()) throw std::runtime_error("无法访问用户音源目录");
        if (!read_preferences()) return -1;
        std::vector<std::string> files;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(std::filesystem::u8path(dir), ec), end;
             !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && script_file(it->path()))
                files.push_back(it->path().filename().u8string());
        }
        if (ec && ec != std::errc::no_such_file_or_directory) throw std::runtime_error("无法读取音源目录：" + ec.message());
        return load_files_impl(dir, std::move(files), true);
    }

    // 导入只初始化这批文件，其余已加载音源保留原运行时，不重复执行联网初始化。
    int load_files(const std::string& dir, std::vector<std::string> files) {
        if (dir.empty()) throw std::runtime_error("无法访问用户音源目录");
        if (!read_preferences()) return -1;
        {
            std::lock_guard<std::mutex> state(state_io_);
            std::set<std::string> next;
            { std::lock_guard<std::mutex> lock(mu_); next = removed_; }
            for (const auto& file : files) next.erase(file_key(file));
            if (!save_preferences(next)) return -1;
            { std::lock_guard<std::mutex> lock(mu_); removed_ = std::move(next); }
        }
        return load_files_impl(dir, std::move(files), false);
    }

    // 普通刷新不解除移除记录；用户主动获取和导入走 load_files。
    int refresh_files(const std::string& dir, std::vector<std::string> files) {
        if (!read_preferences()) return -1;
        return files.empty() ? 0 : load_files_impl(dir, std::move(files), false);
    }

    // 只移除应用里的登记。持久化成功后立刻摘下，绝不删除 JS 或等待全目录初始化。
    bool remove_source(const std::string& file, std::string& error) {
        if (!read_preferences()) { error = preferences_error(); return false; }
        std::lock_guard<std::mutex> state(state_io_);
        const auto name = file_key(file);
        std::set<std::string> next;
        { std::lock_guard<std::mutex> lock(mu_); next = removed_; }
        next.insert(name);
        if (!save_preferences(next)) { error = preferences_error(); return false; }
        std::vector<std::shared_ptr<Script>> retired;
        {
            std::lock_guard<std::mutex> lock(mu_);
            removed_ = std::move(next);
            ++revision_;
            runtime_errors_.erase(name);
            health_.erase(name);
            timings_.erase(name);
            platform_retry_at_.erase(name);
            for (auto it = song_failures_.begin(); it != song_failures_.end();) {
                if (it->second.file == name) it = song_failures_.erase(it); else ++it;
            }
            for (auto it = list_.begin(); it != list_.end();) {
                if (file_key((*it)->path()) == name) {
                    (*it)->retire(); retired.push_back(std::move(*it)); it = list_.erase(it);
                } else ++it;
            }
            failed_.erase(std::remove_if(failed_.begin(), failed_.end(), [&](const auto& f) {
                return file_key(f.first) == name;
            }), failed_.end());
        }
        return true;
    }

    std::string preferences_error() const {
        std::lock_guard<std::mutex> lock(mu_);
        return preferences_error_;
    }
    unsigned revision() const { return revision_.load(); }
    void set_restoring(bool value) { restoring_.store(value); }
    bool restoring() const { return restoring_.load(); }
    void collect_idle() {
        std::vector<std::shared_ptr<Script>> scripts;
        { std::lock_guard<std::mutex> lock(mu_); scripts = list_; }
        for (const auto& script : scripts) { if (net::cancelled()) break; script->collect_idle(); }
    }
    std::map<std::string, std::map<std::string, std::string>> runtime_errors() const {
        std::lock_guard<std::mutex> lock(mu_);
        return runtime_errors_;
    }
    std::map<std::string, Checks> health() const {
        std::lock_guard<std::mutex> lock(mu_); return health_;
    }
    std::map<std::string, Timings> timings() const {
        std::lock_guard<std::mutex> lock(mu_); return timings_;
    }
    std::vector<std::string> removed_files() const {
        std::lock_guard<std::mutex> lock(mu_); return {removed_.begin(), removed_.end()};
    }

    // 由应用后台任务调用；直接检查各脚本，绝不经过会切到其它源的 resolve()。
    void check_sources(const std::vector<Check>& cases,
                       const std::function<bool()>& cancel, const std::function<void()>& wake,
                       int wanted = 1, int budget_ms = 6000, const std::string& only_file = {}) {
        const auto overall = budget_ms > 0 ? std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms) :
            std::chrono::steady_clock::time_point::max();
        const unsigned epoch = ++check_epoch_;
        std::lock_guard<std::mutex> checking(check_mu_);
        if (epoch != check_epoch_.load() || (cancel && cancel())) return;
        std::vector<std::shared_ptr<Script>> scripts;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& script : list_)
                if (only_file.empty() || file_key(script->path()) == file_key(only_file)) scripts.push_back(script);
            // 手动/启动的一轮检测是显式恢复入口，不沿用这些平台的临时冷却和单曲失败。
            for (const auto& sample : cases) {
                for (auto it = song_failures_.begin(); it != song_failures_.end();) {
                    if (it->second.platform == sample.platform && (only_file.empty() || it->second.file == file_key(only_file))) it = song_failures_.erase(it); else ++it;
                }
                for (auto& file : platform_retry_at_)
                    if (only_file.empty() || file.first == file_key(only_file)) file.second.erase(sample.platform);
            }
            if (cases.size() == 1) std::stable_sort(scripts.begin(), scripts.end(), [&](const auto& a, const auto& b) {
                const auto first = selection_locked(a, cases.front().platform, wanted);
                const auto second = selection_locked(b, cases.front().platform, wanted);
                return std::tie(first.availability, first.latency_ms, first.file) < std::tie(second.availability, second.latency_ms, second.file);
            });
        }
        std::atomic<size_t> next{0};
        auto worker = [&] {
            for (;;) {
                const size_t i = next.fetch_add(1);
                if (i >= scripts.size()) return;
                const auto s = scripts[i];
                const auto file = file_key(s->path());
                const auto stopped = [&] { return epoch != check_epoch_.load() || s->retired() || (cancel && cancel()); };
                for (const auto& sample : cases) {
                    if (stopped() || std::chrono::steady_clock::now() >= overall) return;
                    const auto quality = s->pick_quality(sample.platform, wanted);
                    auto publish = [&](Health state) {
                        { std::lock_guard<std::mutex> lock(mu_);
                          if (stopped() || std::find(list_.begin(), list_.end(), s) == list_.end()) return;
                          // 检测的中间状态保留上一次已验证结果，避免检测时排序和颜色反复跳变。
                          auto& previous = health_[file][sample.platform];
                          if (state.state < 2 && previous.state == 2 && previous.quality == quality) return;
                          if (state.state >= 2) state.checked_revision = ++outcome_revision_;
                          if (state.quality.empty()) state.quality = quality;
                          if (state.state == 2 || state.state == 3)
                              record_timing(file, sample.platform, state.quality, state.resolve_ms, state.load_ms, state.state == 2);
                          health_[file][sample.platform] = std::move(state); ++revision_; }
                        if (wake) wake();
                    };
                    if (!s->supports(sample.platform, "musicUrl")) { publish({4, "未声明支持"}); continue; }
                    if (quality.empty()) { publish({4, "不支持所选音质或更低档位"}); continue; }
                    // 点播优先。检测被打断后保留“未检测”，不会把抢占记成失败。
                    if (s->playback_waiting()) { publish({0, "等待播放请求完成"}); continue; }
                    publish({1, "静默取址并测速，最多加载 64 KiB，不播放声音"});
                    std::string url, error;
                    bool playable = false;
                    std::atomic<bool> preempted{false};
                    bool timed_out = false;
                    int resolve_ms = 0, load_ms = 0;
                    const auto begin = std::chrono::steady_clock::now();
                    {
                        const auto deadline = std::min(overall, std::chrono::steady_clock::now() + std::chrono::seconds(8));
                        net::CancelScope budget([&] {
                            if (s->playback_waiting()) preempted.store(true);
                            timed_out = std::chrono::steady_clock::now() >= deadline;
                            return stopped() || preempted.load() || timed_out;
                        });
                        const auto& q = quality;
                        if (s->music_url(sample.platform, q, sample.info, url, error, true)) {
                            resolve_ms = elapsed_ms(begin);
                            const auto probe = net::probe_audio(url, {"User-Agent: Mozilla/5.0"}, 2500, 64 * 1024);
                            playable = probe.playable;
                            load_ms = elapsed_ms(begin);
                            if (!playable) error = probe.reason;
                        }
                    }
                    if (stopped()) {
                        std::lock_guard<std::mutex> lock(mu_);
                        const auto found = health_.find(file);
                        if (found != health_.end()) {
                            const auto item = found->second.find(sample.platform);
                            if (item != found->second.end() && item->second.state == 1) {
                                item->second.state = 0;
                                item->second.reason = "检测已取消，可手动重新检测";
                                ++revision_;
                            }
                        }
                        return;
                    }
                    timed_out = timed_out || transient_error(error);
                    if (preempted.load() || s->playback_waiting()) publish({0, "检测让位于播放，需要时可手动重新检测"});
                    else publish({playable ? 2 : timed_out ? 5 : 3, playable ? "静默加载音频样本有效（最多 64 KiB）" : error.empty() ? "样本请求失败" : error,
                                  resolve_ms, load_ms, quality});
                }
            }
        };
        background::ThreadGroup pool;
        for (size_t i = 0; i < std::min<size_t>(2, scripts.size()); ++i) pool.emplace_back(worker);
        pool.join();
    }

private:
    struct SongFailure {
        std::string file, platform;
        unsigned long long until = 0;
    };
    static std::string song_failure_key(const std::string& file, const std::string& platform,
                                       const std::string& quality, const std::string& info) {
        return file + ":" + platform + ":" + quality + ":" + detail::md5_hex(info);
    }
    static bool transient_error(std::string error) {
        std::transform(error.begin(), error.end(), error.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        for (const auto* word : {"timeout", "timed out", "超时", "connection", "connect failed", "连接失败", "网络", "dns",
                                 "winhttp", "http 5", "http 429", "could not resolve", "couldn't resolve", "couldn't connect"})
            if (error.find(word) != std::string::npos) return true;
        return false;
    }
    static int elapsed_ms(std::chrono::steady_clock::time_point begin) {
        return int(std::clamp<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count(), 1, 300000));
    }
    // mu_ 已持有；平滑波动，取消不调用此入口，单曲失败不覆盖样本状态。
    void record_timing(const std::string& file, const std::string& platform, const std::string& quality,
                       int resolve_ms, int load_ms, bool success, int ready_ms = 0, bool verified = false) {
        auto& value = timings_[file][platform][quality];
        const auto smooth = [](int previous, int current) { return previous > 0 ? (previous * 3 + current + 2) / 4 : current; };
        if (success) {
            if (resolve_ms > 0) value.resolve_ms = smooth(value.resolve_ms, resolve_ms);
            if (load_ms > 0) value.load_ms = smooth(value.load_ms, load_ms);
            if (ready_ms > 0) value.ready_ms = smooth(value.ready_ms, ready_ms);
            if (load_ms > 0 || ready_ms > 0) value.samples = std::min(value.samples + 1, 100000u);
            if (verified || load_ms > 0 || ready_ms > 0) {
                value.verified = true;
                value.cached = false;
                value.verified_revision = ++outcome_revision_;
                value.failures = 0; value.failed_at = 0;
            }
        } else {
            if (platform::ticks() >= value.failed_at + 60000) value.failures = 0;
            value.failures = std::min(value.failures + 1, 3u); value.failed_at = platform::ticks();
            // 一首歌的版权、试听或格式问题不撤销已验证的其它歌曲/样本。
        }
    }
    Selection selection_locked(const std::shared_ptr<Script>& script, const std::string& platform, int wanted) const {
        const auto file = file_key(script->path());
        const auto actual = script->pick_quality(platform, wanted);
        const Health* health = nullptr;
        const Timing *exact = nullptr, *fallback = nullptr;
        const auto checks = health_.find(file);
        if (checks != health_.end()) {
            const auto item = checks->second.find(platform);
            if (item != checks->second.end()) health = &item->second;
        }
        const auto timings = timings_.find(file);
        if (timings != timings_.end()) {
            const auto data = timings->second.find(platform);
            if (data != timings->second.end()) for (const auto& item : data->second) {
                if (item.first == actual) exact = &item.second;
                if (item.second.verified && item.second.load_ms > 0 &&
                    (!fallback || item.second.verified_revision > fallback->verified_revision)) fallback = &item.second;
            }
        }
        auto result = selection_score(actual, health, exact, fallback, platform::ticks());
        result.file = file;
        result.name = script->info().name;
        const auto retry = platform_retry_at_.find(file);
        if (retry != platform_retry_at_.end()) {
            const auto until = retry->second.find(platform);
            if (until != retry->second.end() && platform::ticks() < until->second) result.availability = 4;
        }
        return result;
    }
    static std::string file_key(const std::string& file) {
        auto name = std::filesystem::u8path(file).filename().u8string();
#ifdef _WIN32
        for (char& c : name) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
#endif
        return name;
    }
    std::string preferences_path() const {
        if (!preferences_file_.empty()) return preferences_file_;
        const auto directory = platform::config_dir();
        return directory.empty() ? std::string() : platform::join(directory, "removed-sources.json");
    }
    std::string order_path() const {
        if (!preferences_file_.empty()) return preferences_file_ + ".order.json";
        const auto directory = platform::config_dir();
        return directory.empty() ? std::string() : platform::join(directory, "source-order.json");
    }
    bool read_preferences() {
        std::call_once(order_once_, [this] {
            std::ifstream input(std::filesystem::u8path(order_path()), std::ios::binary);
            if (!input.is_open()) return;
            std::string text(1024 * 1024 + 1, '\0');
            input.read(text.data(), std::streamsize(text.size())); text.resize(size_t(input.gcount()));
            js::Value root;
            if (text.size() > 1024 * 1024 || !js::parse(text, root) || !root.is_array()) return;
            std::vector<std::string> order;
            for (const auto& item : root.array) {
                if (item.type != js::Type::String || item.string.empty()) return;
                const auto file = file_key(item.string);
                if (std::find(order.begin(), order.end(), file) == order.end()) order.push_back(file);
            }
            std::lock_guard<std::mutex> lock(mu_); manual_order_ = std::move(order);
        });
        std::call_once(preferences_once_, [this] {
            const auto path = preferences_path();
            std::error_code ec;
            if (!path.empty() && !std::filesystem::exists(std::filesystem::u8path(path), ec) && !ec) return;
            std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
            std::string text(1024 * 1024 + 1, '\0');
            input.read(text.data(), std::streamsize(text.size())); text.resize(size_t(input.gcount()));
            js::Value root;
            std::lock_guard<std::mutex> lock(mu_);
            if (!input.is_open() || text.size() > 1024 * 1024 || !js::parse(text, root) || !root.is_array()) {
                preferences_valid_ = false; preferences_error_ = "无法读取音源移除记录，请检查设置中的数据目录"; return;
            }
            for (const auto& item : root.array) {
                if (item.type != js::Type::String || item.string.empty()) {
                    preferences_valid_ = false; preferences_error_ = "音源移除记录格式错误，保留原文件"; return;
                }
                removed_.insert(file_key(item.string));
            }
        });
        std::lock_guard<std::mutex> lock(mu_);
        return preferences_valid_;
    }
    bool save_preferences(const std::set<std::string>& removed) {
        const auto path = preferences_path();
        std::string text = "[";
        for (const auto& file : removed) {
            if (text.size() > 1) text += ',';
            text += detail::js_quote(file);
        }
        text += "]\n";
        std::FILE* output = path.empty() ? nullptr : platform::fopen(path + ".tmp", "wb");
        bool ok = false;
        if (output) {
            const bool written = std::fwrite(text.data(), 1, text.size(), output) == text.size();
            const bool closed = std::fclose(output) == 0;
            ok = written && closed && platform::replace_file(path + ".tmp", path);
        }
        if (!ok && !path.empty()) platform::remove(path + ".tmp");
        std::lock_guard<std::mutex> lock(mu_);
        preferences_error_ = ok ? "" : "保存音源移除记录失败，操作未生效";
        return ok;
    }
    int load_files_impl(const std::string& dir, std::vector<std::string> files, bool replace_all) {
        std::lock_guard<std::mutex> loading(load_mu_);   // 在线获取与手动重载可能并发
        busy_.store(true);
        struct Done { std::atomic<bool>& flag; ~Done() { flag.store(false); } } done{busy_};
        const auto same_name = [](const std::string& a, const std::string& b) {
#ifdef _WIN32
            return _stricmp(a.c_str(), b.c_str()) == 0;
#else
            return a == b;
#endif
        };
        files.erase(std::remove_if(files.begin(), files.end(), [](const std::string& name) {
            const auto path = std::filesystem::u8path(name);
            return path.has_root_path() || path.filename() != path || !script_file(path);
        }), files.end());
        std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) {
#ifdef _WIN32
            return _stricmp(a.c_str(), b.c_str()) < 0;
#else
            return a < b;
#endif
        });
        files.erase(std::unique(files.begin(), files.end(), same_name), files.end());
        {
            std::lock_guard<std::mutex> lock(mu_);
            files.erase(std::remove_if(files.begin(), files.end(), [&](const auto& f) {
                return removed_.count(file_key(f)) != 0;
            }), files.end());
        }
        // 并行加载：每个脚本是独立的 QuickJS 运行时，互不共享状态；
        // 慢的是 init 的同步网络请求（死源每个要吃满 15s 超时），
        // 串行 19 个可能几分钟，并行只要等最慢的那个。
        std::vector<std::shared_ptr<Script>> new_list;
        std::vector<std::pair<std::string, std::string>> new_failed;
        std::mutex result_mu;
        std::atomic<size_t> next{0};
        std::atomic<int> ok_count{0};
        auto worker = [&] {
            net::CancelScope network([] { return net::shutdown_signal && net::shutdown_signal->load(); });
            for (;;) {
                const size_t i = next.fetch_add(1);
                if (i >= files.size()) return;
                const auto& name = files[i];
                const auto file = std::filesystem::u8path(dir) / std::filesystem::u8path(name);
                const std::string path = file.u8string();
                std::ifstream in(file, std::ios::binary);
                if (!in) {
                    std::lock_guard<std::mutex> lk(result_mu);
                    new_failed.push_back({name, "无法读取音源脚本"});
                    continue;
                }
                std::string err;
                std::shared_ptr<Script> script;
                bool loaded = false;
                try {
                    std::string code;
                    char bytes[8192];
                    while (in) {
                        in.read(bytes, sizeof bytes);
                        code.append(bytes, size_t(in.gcount()));
                        if (code.size() > 8u * 1024u * 1024u) break;
                    }
                    if (code.size() > 8u * 1024u * 1024u) err = "音源脚本超过 8 MiB";
                    else if (in.bad()) err = "读取音源脚本失败";
                    else {
                        script = std::make_shared<Script>();
                        loaded = script->load(path, code, err) && script->init(err);
                    }
                } catch (const std::exception& error) { err = error.what(); }
                catch (...) { err = "初始化音源脚本失败"; }
                if (loaded) {
                    std::lock_guard<std::mutex> lk(result_mu);
                    new_list.push_back(std::move(script));
                    ok_count.fetch_add(1);
                } else {
                    std::lock_guard<std::mutex> lk(result_mu);
                    new_failed.push_back({name, err});
                }
            }
        };
        {
            background::ThreadGroup pool;
            const int threads = std::min<int>(6, int(files.size()));
            for (int t = 0; t < threads; ++t) pool.emplace_back(worker);
            pool.join();
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!replace_all) {
                const auto replaced = [&](const std::string& name) {
                    return std::any_of(files.begin(), files.end(), [&](const std::string& file) {
                        return same_name(name, file);
                    });
                };
                for (const auto& script : list_)
                    if (!replaced(std::filesystem::u8path(script->info().file).filename().u8string()))
                        new_list.push_back(script);
                for (const auto& failure : failed_)
                    if (!replaced(failure.first)) new_failed.push_back(failure);
            }
            new_list.erase(std::remove_if(new_list.begin(), new_list.end(), [&](const auto& s) {
                if (!removed_.count(file_key(s->path()))) return false;
                s->retire(); return true;
            }), new_list.end());
            new_failed.erase(std::remove_if(new_failed.begin(), new_failed.end(), [&](const auto& f) {
                return removed_.count(file_key(f.first)) != 0;
            }), new_failed.end());
            std::sort(new_list.begin(), new_list.end(), [](const auto& a, const auto& b) {
                return a->info().file < b->info().file;
            });
            std::sort(new_failed.begin(), new_failed.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            list_.swap(new_list);
            for (const auto& old : new_list)
                if (std::find(list_.begin(), list_.end(), old) == list_.end()) old->retire();
            ++revision_;
            failed_.swap(new_failed);
            for (const auto& file : files) { runtime_errors_.erase(file_key(file)); health_.erase(file_key(file)); timings_.erase(file_key(file)); }
            for (const auto& file : files) {
                const auto key = file_key(file);
                platform_retry_at_.erase(key);
                for (auto it = song_failures_.begin(); it != song_failures_.end();) {
                    if (it->second.file == key) it = song_failures_.erase(it); else ++it;
                }
            }
        }
        // 被替换的运行时在 mu_ 锁外释放，避免清理脚本时阻塞界面读取状态。
        return ok_count.load();
    }

public:
    // 正在后台加载（界面上用来显示"加载中"，而不是等锁卡住）
    bool loading() const { return busy_.load() || restoring_.load(); }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mu_);
        return list_.empty();
    }

    std::vector<std::string> names() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<std::string> out;
        for (const auto& s : list_) out.push_back(s->info().name);
        return out;
    }

    // {显示名, 磁盘文件名}。界面删除音源要按文件名删，不能靠显示名猜。
    std::vector<std::pair<std::string, std::string>> entries() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<std::pair<std::string, std::string>> out;
        for (const auto& s : list_) out.push_back({s->info().name, s->info().file});
        return out;
    }

    std::vector<std::pair<std::string, std::string>> failures() const {
        std::lock_guard<std::mutex> lock(mu_);
        return failed_;
    }

    bool supports(const std::string& key, const std::string& only_file = {}) const {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& s : list_)
            if (!s->retired() && (only_file.empty() || file_key(s->path()) == file_key(only_file)) && s->supports(key, "musicUrl")) return true;
        return false;
    }

    // 按平台和实际音质选择已验证且加载快的源；失败后继续后续候选。
    // by 回传真正给出链接的脚本名 —— 界面上要显示"哪个音源"，不能只说"音源"。
    // 持锁只做拷贝：music_url 是同步网络请求（秒级），跑在锁外，否则取流期间
    // 界面每帧读状态会卡死窗口。
    bool resolve(const std::string& key, const std::string& quality, const std::string& info_json,
                 std::string& url, std::string& err, std::string* by = nullptr,
                 const std::function<bool(const std::string&, const std::string&, const std::string&, std::string&)>& accept = {},
                 LoadTiming* load_timing = nullptr, bool smart_switch = true) {
        const int wanted = quality == "128k" ? 0 : quality == "flac" || quality == "flac24bit" ? 2 : 1;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        const auto fixed = smart_switch ? FixedSource{} : choose_fixed_source(wanted);
        const auto report = [&](const std::shared_ptr<Script>& script, const std::string& quality, const std::string& error, int resolve_ms = 0, int ready_ms = 0) {
            std::lock_guard<std::mutex> lock(mu_);
            const auto file = file_key(script->path());
            if (!removed_.count(file) && std::find(list_.begin(), list_.end(), script) != list_.end()) {
                const bool verified = error.empty() && (load_timing ? load_timing->verified : bool(accept));
                const auto failure_key = song_failure_key(file, key, quality, info_json);
                if (verified) {
                    runtime_errors_[file].erase(key);
                    song_failures_.erase(failure_key);
                    platform_retry_at_[file].erase(key);
                } else if (!error.empty()) {
                    runtime_errors_[file][key] = error;
                    if (song_failures_.size() >= 512) {
                        auto oldest = std::min_element(song_failures_.begin(), song_failures_.end(), [](const auto& a, const auto& b) {
                            return a.second.until < b.second.until;
                        });
                        song_failures_.erase(oldest);
                    }
                    const bool transient = transient_error(error);
                    song_failures_[failure_key] = {file, key, platform::ticks() + (transient ? 30000 : 600000)};
                    if (transient) platform_retry_at_[file][key] = platform::ticks() + 30000;
                }
                record_timing(file, key, quality, resolve_ms, 0, error.empty(), ready_ms, verified);
                ++revision_;
            }
        };
        std::string last;
        std::set<std::string> attempted;
        bool excluded_failed = false, skipped_song = false, cooling_down = false;
        for (;;) {
            std::shared_ptr<Script> s;
            Selection score;
            {
                std::lock_guard<std::mutex> lock(mu_);
                // 每次换源重新排序，不能继续使用检测完成前的旧候选顺序。
                for (const auto& candidate : list_) {
                    if (!smart_switch && file_key(candidate->path()) != fixed.file) continue;
                    if (candidate->retired() || !candidate->supports(key, "musicUrl") ||
                        attempted.count(file_key(candidate->path()))) continue;
                    const auto rank = selection_locked(candidate, key, wanted);
                    if (rank.availability == 2) { excluded_failed = true; continue; }
                    if (rank.availability == 4) { cooling_down = true; continue; }
                    if (rank.availability == 3) continue;
                    const auto failure = song_failures_.find(song_failure_key(rank.file, key, rank.quality, info_json));
                    if (failure != song_failures_.end() && platform::ticks() < failure->second.until) { skipped_song = true; continue; }
                    if (!s || std::tie(rank.availability, rank.latency_ms, rank.file) <
                        std::tie(score.availability, score.latency_ms, score.file)) { s = candidate; score = rank; }
                }
            }
            if (!s) break;
            if (!smart_switch) last.clear();
            attempted.insert(score.file);
            if (net::cancelled()) { err = "取流已取消"; return false; }
            if (std::chrono::steady_clock::now() >= deadline) { last += smart_switch ? "；音源解析已达到 30 秒，尝试其他渠道" : "；固定音源解析超时"; break; }
            if (s->retired()) continue;
            std::string u, e;
            const std::string q = s->pick_quality(key, wanted);
            if (q.empty()) { last = s->info().name + ": 不支持所选音质或更低档位"; continue; }
            bool resolved;
            const auto begin = std::chrono::steady_clock::now();
            const auto candidate_deadline = std::min(deadline, begin + std::chrono::seconds(15));
            const auto owner_cancelled = net::cancel_current;
            net::CancelScope candidate_budget([parent = owner_cancelled, candidate_deadline] {
                return (parent && parent()) || std::chrono::steady_clock::now() >= candidate_deadline;
            });
            if (load_timing) *load_timing = {};
            {
                const auto source_deadline = std::min(deadline, begin + std::chrono::seconds(8));
                net::CancelScope time_limit([parent = net::cancel_current, source_deadline] {
                    return (parent && parent()) || std::chrono::steady_clock::now() >= source_deadline;
                });
                resolved = s->music_url(key, q, info_json, u, e);
                if (!resolved && !(owner_cancelled && owner_cancelled()) && std::chrono::steady_clock::now() >= source_deadline)
                    e = "音源取址超时" + (e.empty() ? std::string() : "：" + e);
            }
            const int resolve_ms = elapsed_ms(begin);
            if (resolved && !s->retired()) {
                // URL 只是候选：探测、下载或解码拒绝它后，继续同平台的下一个脚本。
                net::CancelScope network([s, parent = net::cancel_current] {
                    return s->retired() || (parent && parent());
                });
                if (accept && !accept(u, s->info().name, q, e)) {
                    last = s->info().name + ": " + (e.empty() ? "候选音频不可用" : e);
                    // 候选预算到期属于临时超时，不能当成该曲长期不可用。
                    if (!s->retired() && !(owner_cancelled && owner_cancelled()) && std::chrono::steady_clock::now() >= candidate_deadline) report(s, q, "音源启动超时：" + last);
                    else if (!net::cancelled()) report(s, q, last);
                    continue;
                }
                if (net::cancelled()) { err = "取流已取消"; return false; }
                report(s, q, "", resolve_ms, load_timing && load_timing->ready_ms > 0 ? resolve_ms + load_timing->ready_ms : 0);
                url = u;
                if (by) *by = s->info().name;
                return true;
            }
            if (!resolved && e.empty()) e = "音源未返回有效播放地址";
            if (!e.empty()) {
                if (!net::cancelled() || (!(owner_cancelled && owner_cancelled()) && std::chrono::steady_clock::now() >= candidate_deadline)) report(s, q, e);
                if (!last.empty()) last += "；";
                last += s->info().name + ": " + e;
            }
        }
        if (last.empty()) {
            if (cooling_down) last = "当前平台部分音源因临时网络错误冷却中，可稍后再播或手动重新检测";
            else if (skipped_song) last = "已跳过近期无法播放这首歌曲的音源，可稍后重试或手动重新检测";
            else if (excluded_failed) last = "当前平台或音质的样本失败音源已排除，请手动重新检测";
            else if (!smart_switch) last = fixed.file.empty() ? "没有可固定使用的音源" :
                "固定音源「" + fixed.name + "」不支持当前平台或音质；可手动重新检测或开启智能切换";
        }
        err = last.empty() ? "没有支持该平台的音源" : last;
        return false;
    }

private:
    mutable std::mutex mu_;          // 只保护集合和测速状态，网络及解码在锁外。
    std::mutex state_io_;
    std::once_flag preferences_once_;
    std::once_flag order_once_;
    std::vector<std::string> manual_order_;
    std::set<std::string> removed_;
    std::string preferences_error_;
    std::string preferences_file_;
    std::string network_hash_;
    std::map<std::string, std::map<std::string, std::string>> runtime_errors_;
    std::map<std::string, Checks> health_;
    std::map<std::string, Timings> timings_;
    std::map<std::string, SongFailure> song_failures_; // 有界单曲失败缓存；不影响该源其它歌曲的可用状态。
    std::map<std::string, std::map<std::string, unsigned long long>> platform_retry_at_; // 临时网络错误冷却；不触发后台复测。
    unsigned long long outcome_revision_ = 0;
    std::mutex check_mu_;
    std::atomic<unsigned> check_epoch_{0};
    FixedSource fixed_source_; // mu_；仅启动选择、手动重选或移除后重选，不随播放失败改变。
    bool preferences_valid_ = true;
    std::atomic<unsigned> revision_{0};
    std::mutex load_mu_;             // 串行化 load_dir（网络在锁外，见 load_dir 注释）
    std::atomic<bool> busy_{false};  // 正在后台加载
    std::atomic<bool> restoring_{false};
    std::vector<std::shared_ptr<Script>> list_;   // shared：resolve 锁外持引用时列表可能被整体换掉
    std::vector<std::pair<std::string, std::string>> failed_;
};

}  // namespace lx
