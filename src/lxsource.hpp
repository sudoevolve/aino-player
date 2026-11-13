// lxsource.hpp —— 落雪（LX Music）音源脚本运行时
//
// 干什么：把 lx-music 的 .js 音源脚本跑起来，用它的 musicUrl 解析播放直链。
// 怎么跑：嵌入 QuickJS（vendor/quickjs），注入一个最小的 `globalThis.lx`
//         （on / send / request / utils / EVENT_NAMES），脚本里的 HTTP 请求
//         同步交给 aino 自己的 net::get / net::post，回调立即返回。
//
// 设计取舍：
//   * HTTP 走同步。音源脚本对 request 的调用本来就是"发出去等回调"，
//     同步实现省掉一整层事件循环，也不会和 UI 线程抢资源（解析在后台线程做）。
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
#include <set>
#include <string>
#include <thread>
#include <vector>

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

inline thread_local Script* current = nullptr;  // 供 C 回调取回当前脚本  // 供 C 回调取回当前脚本

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

    bool load(const std::string& path, const std::string& script, std::string& err);
    bool init(std::string& err);
    bool music_url(const std::string& platform, const std::string& quality,
                   const std::string& music_info_json, std::string& url, std::string& err);
    void close();

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
    bool call_handler(const std::string& event, JSValueConst arg, JSValue& out, std::string& err);
    bool settle(JSValue& value, std::string& err);

    std::timed_mutex execution_;
    std::atomic<bool> retired_{false};
    JSRuntime* rt_ = nullptr;
    JSContext* ctx_ = nullptr;
    JSValue handlers_ = JS_UNDEFINED;
    Info info_;
    std::vector<Decl> sources_;
    std::string error_;
    bool inited_ = false;
    bool declarations_frozen_ = false;
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

inline JSValue cb_http(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JSValue out = JS_NewObject(ctx);
    if (argc < 2) {
        JS_DefinePropertyValueStr(ctx, out, "error", to_js(ctx, "lx.http: 参数不足"), JS_PROP_C_W_E);
        return out;
    }
    const std::string url = from_js(ctx, argv[0]);
    const std::string opt = from_js(ctx, argv[1]);

    js::Value options;
    if (!js::parse(opt, options) || !options.is_object()) {
        JS_FreeValue(ctx, out);
        return JS_ThrowTypeError(ctx, "lx.request options must be an object");
    }
    std::string method = options["method"].str("GET");
    for (char& c : method) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    net::Request request;
    request.url = url;
    request.method = method;
    request.body = options["body"].str();
    request.timeout_ms = int(std::clamp(options["timeout"].num(8000), 100.0, 12000.0));
    for (const auto& header : options["headers"].object)
        if (header.second.type == js::Type::String)
            request.headers.push_back(header.first + ": " + header.second.string);
    net::Response r = net::execute(request);
    if (std::getenv("LX_TRACE")) {
        std::fprintf(stderr, "[lx][http] %s %s -> status=%d err=%s body=%.200s\n", method.c_str(),
                     url.c_str(), r.status, r.error.c_str(), r.body.c_str());
    }
    if (!r.error.empty()) {
        JS_DefinePropertyValueStr(ctx, out, "error", to_js(ctx, r.error), JS_PROP_C_W_E);
        return out;
    }
    // lx 的响应对象用 statusCode；顺手也给 status，省得脚本挑食
    JS_DefinePropertyValueStr(ctx, out, "statusCode", JS_NewInt32(ctx, r.status), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, out, "status", JS_NewInt32(ctx, r.status), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, out, "statusMessage", to_js(ctx, "OK"), JS_PROP_C_W_E);
    {
        js::Value parsed;
        const bool looks_json = !r.body.empty() && (r.body.front() == '{' || r.body.front() == '[');
        if (looks_json && js::parse(r.body, parsed)) {
            JS_DefinePropertyValueStr(ctx, out, "body", to_js_value(ctx, parsed), JS_PROP_C_W_E);
        } else {
            JS_DefinePropertyValueStr(ctx, out, "body", to_js(ctx, r.body), JS_PROP_C_W_E);
        }
    }
    JS_DefinePropertyValueStr(ctx, out, "bytes", JS_NewInt32(ctx, int(r.body.size())), JS_PROP_C_W_E);
    // 原始文本也留一份：有些接口返回单引号 JSON，严格解析会失败，脚本可以自己兜
    JS_DefinePropertyValueStr(ctx, out, "bodyText", to_js(ctx, r.body), JS_PROP_C_W_E);
    JSValue h = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, h, "content-type", to_js(ctx, r.content_type), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, out, "headers", h, JS_PROP_C_W_E);
    return out;
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

// 脚本死循环的兜底：超过 20 秒直接中断
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
    if (enc === 'base64') return __lx.b64encode(binOf(bytes));
    if (enc === 'hex') return Array.from(bytes).map(b => b.toString(16).padStart(2, '0')).join('');
    if (enc === 'binary' || enc === 'latin1') return binOf(bytes);
    return utf8Decode(bytes);
  }

  const utils = {
    buffer: {
      from: function (input, encoding) { return bytesOf(input, encoding); },
      bufToString: function (buf, encoding) { return textOf(bytesOf(buf), encoding); }
    },
    crypto: {
      md5: function (s) { return __lx.md5(String(s)); },
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
    version: (typeof __lx.lxVersion === 'string' ? __lx.lxVersion : '2.12.6'),
    currentScriptInfo: (function () { try { return JSON.parse(__lx.scriptInfo); } catch (e) { return {}; } })(),
    env: 'desktop',
    on: function (name, fn) {
      if (__lx.debug) __lx.log('TRACE on(' + name + ') fn=' + (typeof fn));
      if (typeof fn === 'function') handlers[name] = fn;
    },
    send: function (name, data) {
      if (__lx.debug) __lx.log('TRACE send(' + name + ')');
      try { __lx.send(String(name), JSON.stringify(data)); }
      catch (e) { __lx.log('send(' + name + ') 失败: ' + e); }
    },
    request: function (url, options, callback) {
      if (typeof options === 'function') { callback = options; options = {}; }
      let res;
      try {
        const opts = Object.assign({}, options || {});
        if (opts.body && typeof opts.body === 'object') opts.body = JSON.stringify(opts.body);
        res = __lx.http(String(url), JSON.stringify(opts));
      }
      catch (e) { if (callback) callback(e); return; }
      if (res && res.error) { if (callback) callback(new Error(String(res.error))); return; }
      if (callback) callback(null, res);
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

  globalThis.setTimeout = function (fn) { try { fn(); } catch (e) { __lx.log('setTimeout: ' + e); } return 0; };
  globalThis.clearTimeout = function () {};
  globalThis.setInterval = function () { return 0; };
  globalThis.clearInterval = function () {};
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
    JS_SetPropertyStr(ctx_, api, "md5", JS_NewCFunction(ctx_, detail::cb_md5, "md5", 1));
    JS_SetPropertyStr(ctx_, api, "b64encode", JS_NewCFunction(ctx_, detail::cb_b64e, "b64encode", 1));
    JS_SetPropertyStr(ctx_, api, "b64decode", JS_NewCFunction(ctx_, detail::cb_b64d, "b64decode", 1));
    JS_SetPropertyStr(ctx_, api, "send", JS_NewCFunction(ctx_, detail::cb_send, "send", 2));
    JS_SetPropertyStr(ctx_, api, "log", JS_NewCFunction(ctx_, detail::cb_log, "log", 1));
    JS_SetPropertyStr(ctx_, api, "debug", JS_NewBool(ctx_, std::getenv("LX_TRACE") != nullptr));
    if (const char* v = std::getenv("LX_VERSION")) JS_SetPropertyStr(ctx_, api, "lxVersion", detail::to_js(ctx_, v));
    {
        const std::string json = "{\"name\":" + detail::js_quote(info_.name) + ",\"version\":" +
            detail::js_quote(info_.version) + ",\"author\":" + detail::js_quote(info_.author) +
            ",\"description\":" + detail::js_quote(info_.description) + ",\"homepage\":" +
            detail::js_quote(info_.homepage) + "}";
        JS_SetPropertyStr(ctx_, api, "scriptInfo", detail::to_js(ctx_, json));
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
    JSContext* jc = nullptr;
    for (int i = 0; i < 256 && JS_ExecutePendingJob(rt_, &jc) > 0; ++i) {}
    if (net::cancelled()) { err = "脚本加载已取消或超时"; return false; }
    return true;
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
    for (int i = 0; i < 100000; ++i) {
        if (net::cancelled()) break;
        if (JS_PromiseState(ctx_, value) != JS_PROMISE_PENDING) break;
        JSContext* c = nullptr;
        if (JS_ExecutePendingJob(rt_, &c) <= 0) break;  // 没有待执行任务，promise 卡住
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

    // 把加载阶段挂起的 microtask 跑完（有的源在 Promise 里才 send）
    JSContext* c = nullptr;
    for (int i = 0; i < 512 && JS_ExecutePendingJob(rt_, &c) > 0; ++i) {}
    if (net::cancelled()) { err = "脚本初始化已取消或超时"; return false; }

    if (sources_.empty()) {
        err = !error_.empty() ? error_ : (call_err.empty() ? "脚本没有声明任何平台" : call_err);
        return false;
    }
    inited_ = true;
    declarations_frozen_ = true;
    return true;
}

inline bool Script::music_url(const std::string& platform, const std::string& quality,
                              const std::string& music_info_json, std::string& url, std::string& err) {
    Operation operation(*this, 8000);
    if (!operation) { err = "脚本繁忙、已移除或取流超时"; return false; }
    if (!ctx_ || !inited_) { err = "脚本未就绪"; return false; }
    JS_UpdateStackTop(rt_);   // 同上：解析常常发生在别的线程
    detail::current = this;
    struct Guard {
        ~Guard() { detail::current = nullptr; }
    } guard;

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

    if (net::cancelled()) { err = "脚本请求已取消或超过 8 秒"; return false; }
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        if (err.empty()) err = url.empty() ? "脚本没有返回直链" : ("无效直链: " + url);
        return false;
    }
    return true;
}


// 音源脚本目录：AINO_LX_DIR 优先，否则用可执行文件旁边的 sources/
inline std::string default_dir() {
    if (const char* e = std::getenv("AINO_LX_DIR")) {
        if (*e) return e;
    }
#ifdef _WIN32
    char buf[MAX_PATH]{};
    if (GetModuleFileNameA(nullptr, buf, MAX_PATH)) {
        std::string p(buf);
        const size_t cut = p.find_last_of("\\/");
        if (cut != std::string::npos) return p.substr(0, cut) + "\\sources";
    }
#endif
    return "sources";
}

// ---------------------------------------------------------------- 音源集合

// 一个目录里的音源脚本；运行时逐个串行执行，集合支持后台热替换与应用内移除。
class Engine {
public:
    explicit Engine(std::string preferences_file = {}) : preferences_file_(std::move(preferences_file)) {}
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
        if (!read_preferences()) return -1;
        std::vector<std::string> files;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(std::filesystem::u8path(dir), ec), end;
             !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && script_file(it->path()))
                files.push_back(it->path().filename().u8string());
        }
        return load_files_impl(dir, std::move(files), true);
    }

    // 导入只初始化这批文件，其余已加载音源保留原运行时，不重复执行联网初始化。
    int load_files(const std::string& dir, std::vector<std::string> files) {
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
            for (auto it = list_.begin(); it != list_.end();) {
                if (file_key((*it)->path()) == name) {
                    (*it)->retire(); retired.push_back(std::move(*it)); it = list_.erase(it);
                } else ++it;
            }
            failed_.erase(std::remove_if(failed_.begin(), failed_.end(), [&](const auto& f) {
                return file_key(f.first) == name;
            }), failed_.end());
            if (file_key(preferred_) == name) preferred_.clear();
        }
        return true;
    }

    std::string preferences_error() const {
        std::lock_guard<std::mutex> lock(mu_);
        return preferences_error_;
    }
    unsigned revision() const { return revision_.load(); }
    std::map<std::string, std::string> runtime_errors() const {
        std::lock_guard<std::mutex> lock(mu_);
        return runtime_errors_;
    }

private:
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
    bool read_preferences() {
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
            std::vector<std::thread> pool;
            const int threads = std::min<int>(6, int(files.size()));
            for (int t = 0; t < threads; ++t) pool.emplace_back(worker);
            for (auto& th : pool) th.join();
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
            ++revision_;
            failed_.swap(new_failed);
            for (const auto& file : files) runtime_errors_.erase(file_key(file));
        }
        // 被替换的运行时在 mu_ 锁外释放，避免清理脚本时阻塞界面读取状态。
        return ok_count.load();
    }

public:
    // 正在后台加载（界面上用来显示"加载中"，而不是等锁卡住）
    bool loading() const { return busy_.load(); }

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

    bool supports(const std::string& key) const {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& s : list_)
            if (!s->retired() && s->supports(key, "musicUrl")) return true;
        return false;
    }

    // 依次尝试：先选中的源，再其它源。任一成功即返回。
    // by 回传真正给出链接的脚本名 —— 界面上要显示"哪个音源"，不能只说"音源"。
    // 持锁只做拷贝：music_url 是同步网络请求（秒级），跑在锁外，否则取流期间
    // 界面每帧读状态会卡死窗口。
    bool resolve(const std::string& key, const std::string& quality, const std::string& info_json,
                 std::string& url, std::string& err, std::string* by = nullptr,
                 const std::function<bool(const std::string&, const std::string&, const std::string&, std::string&)>& accept = {}) {
        std::vector<std::shared_ptr<Script>> order;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& s : list_)
                if (!s->retired() && s->supports(key, "musicUrl")) order.push_back(s);
            const std::string preferred = preferred_;
            if (!preferred.empty()) {
                std::stable_sort(order.begin(), order.end(), [&preferred](const std::shared_ptr<Script>& a,
                                                                           const std::shared_ptr<Script>& b) {
                    return a->info().file == preferred && b->info().file != preferred;
                });
            }
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        const auto report = [&](const std::shared_ptr<Script>& script, const std::string& error) {
            std::lock_guard<std::mutex> lock(mu_);
            const auto key = file_key(script->path());
            if (!removed_.count(key)) {
                if (error.empty()) runtime_errors_.erase(key);
                else runtime_errors_[key] = error;
            }
        };
        std::string last;
        for (auto& s : order) {
            if (net::cancelled()) { err = "取流已取消"; return false; }
            if (std::chrono::steady_clock::now() >= deadline) { last += "；音源解析已达到 30 秒，尝试其他渠道"; break; }
            if (s->retired()) continue;
            std::string u, e;
            const int wanted = quality == "128k" ? 0 : quality == "flac" || quality == "flac24bit" ? 2 : 1;
            const std::string q = s->pick_quality(key, wanted);
            if (q.empty()) { last = s->info().name + ": 不支持所选音质或更低档位"; report(s, last); continue; }
            bool resolved;
            {
                net::CancelScope time_limit([parent = net::cancel_current, deadline] {
                    return (parent && parent()) || std::chrono::steady_clock::now() >= deadline;
                });
                resolved = s->music_url(key, q, info_json, u, e);
            }
            if (resolved && !s->retired()) {
                // URL 只是候选：探测、下载或解码拒绝它后，继续同平台的下一个脚本。
                net::CancelScope network([s, parent = net::cancel_current] {
                    return s->retired() || (parent && parent());
                });
                if (accept && !accept(u, s->info().name, q, e)) {
                    last = s->info().name + ": " + (e.empty() ? "候选音频不可用" : e);
                    if (!net::cancelled()) report(s, last);
                    continue;
                }
                report(s, "");
                url = u;
                if (by) *by = s->info().name;
                return true;
            }
            if (!e.empty()) {
                if (!net::cancelled()) report(s, e);
                if (!last.empty()) last += "；";
                last += s->info().name + ": " + e;
            }
        }
        err = last.empty() ? "没有支持该平台的音源" : last;
        return false;
    }

    void set_preferred(const std::string& file) {
        std::lock_guard<std::mutex> lock(mu_);
        preferred_ = file;
    }

private:
    mutable std::mutex mu_;          // 只保护 list_/failed_/preferred_ 的读写（都应是瞬时操作）
    std::mutex state_io_;
    std::once_flag preferences_once_;
    std::set<std::string> removed_;
    std::string preferences_error_;
    std::string preferences_file_;
    std::map<std::string, std::string> runtime_errors_;
    bool preferences_valid_ = true;
    std::atomic<unsigned> revision_{0};
    std::mutex load_mu_;             // 串行化 load_dir（网络在锁外，见 load_dir 注释）
    std::atomic<bool> busy_{false};  // 正在后台加载
    std::vector<std::shared_ptr<Script>> list_;   // shared：resolve 锁外持引用时列表可能被整体换掉
    std::vector<std::pair<std::string, std::string>> failed_;
    std::string preferred_;
};

}  // namespace lx
