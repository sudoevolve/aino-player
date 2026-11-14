// provider.hpp —— 平台 API 的 JS 宿主
//
// 为什么要有这层：各平台的搜索 / 榜单 / 取流接口变得比发版快得多。
// 把这类代码留在 C++ 里，等于每次接口一变就要重新编译发版；放在 .js 里，
// 用户改一个文件就能救回来。
//
// 契约（每平台一个文件，providers/<key>.js）：
//
//   globalThis.provider = {
//     key: 'netease', name: '网易云',
//     search(keyword, page, limit) -> [song...],
//     board(boardId, page, limit)  -> [song...],
//     resolve(song, quality)       -> 'http://...',   // 可选：不给就走 C++ 内置接口
//   }
//
//   song = { id, title, artist, album, duration_ms, bitrate, native_id, cover }
//
// 宿主注入 ctx：
//   ctx.http(url, opts)            同步 HTTP，返回 {statusCode, body(已解析), headers}
//   ctx.md5 / ctx.b64encode / ctx.b64decode / ctx.log
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <stdexcept>
#include <vector>

#include "json.hpp"
#include "lxsource.hpp"
#include "net.hpp"
#if __has_include("aino_platforms.hpp")
#include "aino_platforms.hpp"
#define AINO_HAS_BUNDLED_PLATFORMS 1
#endif

namespace prov {
inline std::string default_dir();

// provider 与音源共享 HTTP 桥，但每次执行必须有独立的实际时间截止点。
struct ExecutionTime {
    net::CancelScope network;
    explicit ExecutionTime(int milliseconds = 8000)
        : network([parent = net::cancel_current,
                   deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds)] {
            return (parent && parent()) || std::chrono::steady_clock::now() >= deadline;
        }) {}
};

struct Song {  // 和 music::Variant 一一对应，隔离掉上层类型
    std::string id, title, artist, album, native_id, cover;
    int duration_ms = 0;
    int bitrate = 0;
    js::Value metadata; // 保留平台返回的目录 ID、付费标记与音质 hash。
};

// ---------------------------------------------------------------- JS 侧桥

namespace detail {

struct JsValueOwner {
    JSContext* ctx;
    JSValue value;
    JsValueOwner(JSContext* context, JSValue input) : ctx(context), value(input) {}
    JsValueOwner(const JsValueOwner&) = delete;
    JsValueOwner& operator=(const JsValueOwner&) = delete;
    ~JsValueOwner() { JS_FreeValue(ctx, value); }
};

inline JSValue to_js_song(JSContext* ctx, const Song& s) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "id", lx::detail::to_js(ctx, s.id));
    JS_SetPropertyStr(ctx, o, "title", lx::detail::to_js(ctx, s.title));
    JS_SetPropertyStr(ctx, o, "artist", lx::detail::to_js(ctx, s.artist));
    JS_SetPropertyStr(ctx, o, "album", lx::detail::to_js(ctx, s.album));
    JS_SetPropertyStr(ctx, o, "native_id", lx::detail::to_js(ctx, s.native_id));
    JS_SetPropertyStr(ctx, o, "cover", lx::detail::to_js(ctx, s.cover));
    JS_SetPropertyStr(ctx, o, "duration_ms", JS_NewInt32(ctx, s.duration_ms));
    JS_SetPropertyStr(ctx, o, "bitrate", JS_NewInt32(ctx, s.bitrate));
    return o;
}

inline Song song_from_js(JSContext* ctx, JSValueConst v) {
    Song s;
    const auto text = [&](JSValueConst object, const char* k) {
        JsValueOwner x(ctx, JS_GetPropertyStr(ctx, object, k));
        if (JS_IsException(x.value)) throw std::runtime_error(lx::detail::take_exception(ctx));
        std::string r = JS_IsString(x.value) || JS_IsNumber(x.value)
            ? lx::detail::from_js(ctx, x.value) : std::string();
        if (r.size() > 8192) throw std::runtime_error("平台曲目字段超过 8 KiB");
        return r;
    };
    auto str = [&](const char* k) { return text(v, k); };
    auto num = [&](const char* k) {
        JsValueOwner x(ctx, JS_GetPropertyStr(ctx, v, k));
        if (JS_IsException(x.value)) throw std::runtime_error(lx::detail::take_exception(ctx));
        int32_t r = 0;
        if ((JS_IsNumber(x.value) || JS_IsString(x.value)) && JS_ToInt32(ctx, &r, x.value) < 0)
            throw std::runtime_error(lx::detail::take_exception(ctx));
        return int(r);
    };
    s.id = str("id");
    s.title = str("title");
    s.artist = str("artist");
    s.album = str("album");
    s.native_id = str("native_id");
    s.cover = str("cover");
    s.duration_ms = num("duration_ms");
    s.bitrate = num("bitrate");
    // 只保留跨平台模型实际消费的字段，不再把整首原始 JSON 编码、解析后重复保存。
    const auto string_value = [](std::string text) {
        js::Value value; value.type = js::Type::String; value.string = std::move(text); return value;
    };
    s.metadata.type = js::Type::Object;
    for (const auto* key : {"album_id", "album_audio_id"}) s.metadata.object.emplace_back(key, string_value(str(key)));
    JsValueOwner needs_login(ctx, JS_GetPropertyStr(ctx, v, "needs_login"));
    if (JS_IsException(needs_login.value)) throw std::runtime_error(lx::detail::take_exception(ctx));
    js::Value flag; flag.type = js::Type::Bool; flag.boolean = JS_ToBool(ctx, needs_login.value) > 0;
    s.metadata.object.emplace_back("needs_login", std::move(flag));
    JsValueOwner artists(ctx, JS_GetPropertyStr(ctx, v, "artists"));
    if (JS_IsException(artists.value)) throw std::runtime_error(lx::detail::take_exception(ctx));
    js::Value list; list.type = js::Type::Array;
    if (JS_IsArray(artists.value)) for (uint32_t i = 0; i < 32; ++i) {
        JsValueOwner item(ctx, JS_GetPropertyUint32(ctx, artists.value, i));
        if (JS_IsException(item.value)) throw std::runtime_error(lx::detail::take_exception(ctx));
        if (JS_IsUndefined(item.value)) break;
        if (!JS_IsObject(item.value)) continue;
        js::Value artist; artist.type = js::Type::Object;
        artist.object.emplace_back("id", string_value(text(item.value, "id")));
        artist.object.emplace_back("name", string_value(text(item.value, "name")));
        list.array.push_back(std::move(artist));
    }
    s.metadata.object.emplace_back("artists", std::move(list));
    JsValueOwner hashes(ctx, JS_GetPropertyStr(ctx, v, "quality_hashes"));
    if (JS_IsException(hashes.value)) throw std::runtime_error(lx::detail::take_exception(ctx));
    js::Value qualities; qualities.type = js::Type::Object;
    if (JS_IsObject(hashes.value)) for (const auto* quality : {"128k", "320k", "flac", "flac24bit"}) {
        auto hash = text(hashes.value, quality);
        if (!hash.empty()) qualities.object.emplace_back(quality, string_value(std::move(hash)));
    }
    s.metadata.object.emplace_back("quality_hashes", std::move(qualities));
    return s;
}

}  // namespace detail

// ---------------------------------------------------------------- 一个 provider

class Provider {
public:
    Provider() = default;
    Provider(const Provider&) = delete;
    ~Provider() { close(); }

    const std::string& key() const { return key_; }
    const std::string& name() const { return name_; }
    const std::string& file() const { return file_; }
    bool has_search() const { return has_search_; }
    bool has_board() const { return has_board_; }
    bool has_resolve() const { return has_resolve_; }

    bool load(const std::string& path, const std::string& code, std::string& err);
    bool search(const std::string& keyword, int page, int limit, std::vector<Song>& out, std::string& err);
    bool board(const std::string& board_id, int page, int limit, std::vector<Song>& out, std::string& err,
               bool* more = nullptr, int* total = nullptr);
    bool resolve(const Song& song, const std::string& quality, std::string& url, std::string& err);
    bool request(const char* method, const std::string& args, js::Value& out, std::string& err);

    void close();

private:
    struct Operation {
        ExecutionTime time;
        net::CancelScope priority;
        std::unique_lock<std::timed_mutex> lock;
        explicit Operation(Provider& p, bool background = false)
            : priority([&p, background, parent = net::cancel_current] {
                return (parent && parent()) || (background && p.foreground_waiters_.load() != 0);
            }), lock(p.execution_, std::defer_lock) {
            struct Waiting {
                std::atomic<unsigned>& count;
                bool active;
                Waiting(std::atomic<unsigned>& value, bool enabled) : count(value), active(enabled) {
                    if (active) ++count;
                }
                ~Waiting() { if (active) --count; }
            } waiting(p.foreground_waiters_, !background);
            while (!net::cancelled() && !lock.try_lock_for(std::chrono::milliseconds(20))) {}
            if (lock.owns_lock() && p.rt_) JS_UpdateStackTop(p.rt_);
        }
        explicit operator bool() const { return lock.owns_lock() && !net::cancelled(); }
    };
    std::timed_mutex execution_;
    std::atomic<unsigned> foreground_waiters_{0};
    bool eval(const std::string& code, const char* name, std::string& err);
    bool call(const char* fn, int argc, JSValue* argv, std::vector<Song>& out, std::string& err,
              bool* more = nullptr, int* total = nullptr);

    JSRuntime* rt_ = nullptr;
    JSContext* ctx_ = nullptr;
    JSValue provider_ = JS_UNDEFINED;
    std::string key_, name_, file_;
    bool has_search_ = false, has_board_ = false, has_resolve_ = false;
};

inline bool Provider::eval(const std::string& code, const char* name, std::string& err) {
    JSValue v = JS_Eval(ctx_, code.c_str(), code.size(), name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        err = lx::detail::take_exception(ctx_);
        JS_FreeValue(ctx_, v);
        return false;
    }
    JS_FreeValue(ctx_, v);
    return true;
}

inline const char* provider_bootstrap() {
    return R"JS(
(function () {
  const ctx = {
    http: function (url, opts) { return __lxhost.http(String(url), JSON.stringify(opts || {})); },
    cookie: function (name) { return __lxhost.cookie(String(name)); },
    md5: function (s) { return __lx.md5(String(s)); },
    b64encode: function (s) { return __lx.b64encode(String(s)); },
    b64decode: function (s) { return __lx.b64decode(String(s)); },
    log: function () { __lx.log(Array.prototype.join.call(arguments, ' ')); }
  };
  globalThis.ctx = ctx;
  globalThis.console = { log: ctx.log, warn: ctx.log, error: ctx.log, info: ctx.log, group: function () {}, groupEnd: function () {} };
  globalThis.setTimeout = function (fn) { try { fn(); } catch (e) { ctx.log('setTimeout: ' + e); } return 0; };
})();
)JS";
}

// 每次调用线程持有自己的凭据快照，跨平台并发和旧搜索不会互相覆盖。
inline thread_local std::string g_cred_netease, g_cred_qq;

// ctx.cookie('netease'|'qq') —— 从宿主拿登录 Cookie
inline JSValue host_cookie(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    const std::string key = argc > 0 ? lx::detail::from_js(ctx, argv[0]) : std::string();
    // 调用者在本线程创建请求前写入
    std::string v;
    if (key == "netease") v = g_cred_netease;
    else if (key == "qq") v = g_cred_qq;
    return lx::detail::to_js(ctx, v);
}

inline JSValue host_http(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (std::getenv("PROV_TRACE")) std::fprintf(stderr, "[prov] http...\n");
    JSValue r = lx::detail::cb_http(ctx, JS_UNDEFINED, argc, argv);
    if (std::getenv("PROV_TRACE")) std::fprintf(stderr, "[prov] http ok\n");
    return r;
}

inline bool Provider::load(const std::string& path, const std::string& code, std::string& err) {
    ExecutionTime time_limit(12000);
    file_ = path;
    rt_ = JS_NewRuntime();
    if (!rt_) { err = "创建 JS 运行时失败"; return false; }
    JS_SetMemoryLimit(rt_, 256u * 1024u * 1024u);
    ctx_ = JS_NewContext(rt_);
    if (!ctx_) { err = "创建 JS 上下文失败"; return false; }
    JS_SetInterruptHandler(rt_, lx::detail::interrupt_cb, nullptr);

    JSValue gl = JS_GetGlobalObject(ctx_);
    JSValue api = JS_NewObject(ctx_);
    JS_SetPropertyStr(ctx_, api, "http", JS_NewCFunction(ctx_, host_http, "http", 2));
    JS_SetPropertyStr(ctx_, api, "cookie", JS_NewCFunction(ctx_, host_cookie, "cookie", 1));
    JS_SetPropertyStr(ctx_, api, "md5", JS_NewCFunction(ctx_, lx::detail::cb_md5, "md5", 1));
    JS_SetPropertyStr(ctx_, api, "b64encode", JS_NewCFunction(ctx_, lx::detail::cb_b64e, "b64encode", 1));
    JS_SetPropertyStr(ctx_, api, "b64decode", JS_NewCFunction(ctx_, lx::detail::cb_b64d, "b64decode", 1));
    JS_SetPropertyStr(ctx_, api, "log", JS_NewCFunction(ctx_, lx::detail::cb_log, "log", 1));
    JS_SetPropertyStr(ctx_, gl, "__lxhost", api);
    // 音源那套 __lx 也要在，bootstrap 里用到了 __lx.log / md5 / b64*
    JS_SetPropertyStr(ctx_, gl, "__lx", JS_DupValue(ctx_, api));
    JS_FreeValue(ctx_, gl);

    if (!eval(provider_bootstrap(), "<provider-bootstrap>", err)) return false;
    if (!eval(code, path.c_str(), err)) return false;

    gl = JS_GetGlobalObject(ctx_);
    JSValue p = JS_GetPropertyStr(ctx_, gl, "provider");
    JS_FreeValue(ctx_, gl);
    if (!JS_IsObject(p)) {
        JS_FreeValue(ctx_, p);
        err = "脚本没有定义 globalThis.provider";
        return false;
    }
    provider_ = p;

    auto field = [&](const char* k) {
        JSValue v = JS_GetPropertyStr(ctx_, provider_, k);
        std::string r = lx::detail::from_js(ctx_, v);
        JS_FreeValue(ctx_, v);
        return r;
    };
    key_ = field("key");
    name_ = field("name");
    auto has = [&](const char* k) {
        JSValue v = JS_GetPropertyStr(ctx_, provider_, k);
        const bool ok = JS_IsFunction(ctx_, v);
        JS_FreeValue(ctx_, v);
        return ok;
    };
    has_search_ = has("search");
    has_board_ = has("board");
    has_resolve_ = has("resolve");
    if (key_.empty()) key_ = path;
    if (name_.empty()) name_ = key_;
    return true;
}

inline void Provider::close() {
    if (ctx_) {
        JS_UpdateStackTop(rt_);
        JS_FreeValue(ctx_, provider_);
        provider_ = JS_UNDEFINED;
        JS_FreeContext(ctx_);
        ctx_ = nullptr;
    }
    if (rt_) {
        JS_FreeRuntime(rt_);
        rt_ = nullptr;
    }
}

// 调用返回"歌曲数组"的函数
inline bool Provider::call(const char* fn, int argc, JSValue* argv, std::vector<Song>& out, std::string& err,
                           bool* more, int* total) {
    ExecutionTime time_limit;
    if (net::cancelled()) { err = "provider 请求已取消"; return false; }
    if (!ctx_) { err = "provider 未加载"; return false; }
    // 宿主在启动线程加载、在检索线程调用，栈基准必须按当前线程刷新
    JS_UpdateStackTop(rt_);
    if (std::getenv("PROV_TRACE")) std::fprintf(stderr, "[prov] call %s\n", fn);
    JSValue f = JS_GetPropertyStr(ctx_, provider_, fn);
    if (!JS_IsFunction(ctx_, f)) {
        JS_FreeValue(ctx_, f);
        err = std::string("provider 没有实现 ") + fn;
        return false;
    }
    JSValue r = JS_Call(ctx_, f, provider_, argc, argv);
    JS_FreeValue(ctx_, f);
    if (std::getenv("PROV_TRACE")) std::fprintf(stderr, "[prov] %s returned\n", fn);
    if (JS_IsException(r)) {
        err = lx::detail::take_exception(ctx_);
        JS_FreeValue(ctx_, r);
        return false;
    }
    // 允许返回 Promise
    for (int i = 0; i < 100000 && JS_IsPromise(r) && JS_PromiseState(ctx_, r) == JS_PROMISE_PENDING; ++i) {
        JSContext* c = nullptr;
        if (net::cancelled() || JS_ExecutePendingJob(rt_, &c) <= 0) break;
    }
    if (JS_IsPromise(r)) {
        const JSPromiseStateEnum st = JS_PromiseState(ctx_, r);
        if (st == JS_PROMISE_PENDING) {
            err = "provider 异步任务未完成或已超时"; JS_FreeValue(ctx_, r); return false;
        }
        if (st == JS_PROMISE_REJECTED) {
            JSValue e = JS_PromiseResult(ctx_, r);
            err = lx::detail::from_js(ctx_, e);
            JS_FreeValue(ctx_, e);
            JS_FreeValue(ctx_, r);
            return false;
        }
        JSValue v = JS_PromiseResult(ctx_, r);
        JS_FreeValue(ctx_, r);
        r = v;
    }
    bool explicit_more = false;
    if (more) *more = false;
    if (total) *total = -1;
    if (more && JS_IsObject(r) && !JS_IsArray(r)) {
        const auto value = JS_GetPropertyStr(ctx_, r, "more");
        explicit_more = !JS_IsUndefined(value);
        *more = JS_ToBool(ctx_, value) > 0;
        JS_FreeValue(ctx_, value);
        const auto count = JS_GetPropertyStr(ctx_, r, "total");
        int32_t parsed = -1;
        if (total && !JS_IsUndefined(count) && JS_ToInt32(ctx_, &parsed, count) == 0 && parsed >= 0) *total = parsed;
        JS_FreeValue(ctx_, count);
        const auto items = JS_GetPropertyStr(ctx_, r, "items");
        JS_FreeValue(ctx_, r);
        r = items;
    }
    if (!JS_IsArray(r)) {
        err = std::string(fn) + " 没有返回数组";
        JS_FreeValue(ctx_, r);
        return false;
    }
    JSValue lenv = JS_GetPropertyStr(ctx_, r, "length");
    int32_t n = 0;
    JS_ToInt32(ctx_, &n, lenv);
    JS_FreeValue(ctx_, lenv);
    if (n < 0 || n > 10000) { err = "平台返回曲目数量超限"; JS_FreeValue(ctx_, r); return false; }
    if (more && !explicit_more) {
        int32_t limit = 0;
        if (argc == 3) JS_ToInt32(ctx_, &limit, argv[2]);
        *more = limit > 0 && n >= limit;
    }
    out.clear();
    out.reserve(size_t(n > 0 ? n : 0));
    try {
        for (int i = 0; i < n; ++i) {
            detail::JsValueOwner item(ctx_, JS_GetPropertyUint32(ctx_, r, uint32_t(i)));
            if (JS_IsException(item.value)) throw std::runtime_error(lx::detail::take_exception(ctx_));
            if (JS_IsObject(item.value)) out.push_back(detail::song_from_js(ctx_, item.value));
        }
    } catch (const std::exception& e) {
        err = std::string("平台曲目解析失败：") + e.what();
        out.clear();
        JS_FreeValue(ctx_, r);
        return false;
    }
    JS_FreeValue(ctx_, r);
    return true;
}

inline JSValue mk_str(JSContext* ctx, const std::string& s) { return lx::detail::to_js(ctx, s); }

inline bool Provider::search(const std::string& keyword, int page, int limit, std::vector<Song>& out,
                             std::string& err) {
    Operation operation(*this);
    if (!operation) { err = "平台请求已取消或等待超时"; return false; }
    if (!ctx_) { err = "provider 未加载"; return false; }
    JS_UpdateStackTop(rt_);
    JSValue args[3] = {mk_str(ctx_, keyword), JS_NewInt32(ctx_, page), JS_NewInt32(ctx_, limit)};
    const bool ok = call("search", 3, args, out, err);
    for (auto& a : args) JS_FreeValue(ctx_, a);
    return ok;
}

inline bool Provider::board(const std::string& board_id, int page, int limit, std::vector<Song>& out,
                            std::string& err, bool* more, int* total) {
    Operation operation(*this, true);
    if (!operation) { err = "平台请求已取消或等待超时"; return false; }
    if (!ctx_) { err = "provider 未加载"; return false; }
    JS_UpdateStackTop(rt_);
    JSValue args[3] = {mk_str(ctx_, board_id), JS_NewInt32(ctx_, page), JS_NewInt32(ctx_, limit)};
    bool ignored_more = false;
    int ignored_total = -1;
    const bool ok = call("board", 3, args, out, err, more ? more : &ignored_more, total ? total : &ignored_total);
    for (auto& a : args) JS_FreeValue(ctx_, a);
    return ok;
}

inline bool Provider::resolve(const Song& song, const std::string& quality, std::string& url, std::string& err) {
    Operation operation(*this);
    if (!operation) { err = "平台请求已取消或等待超时"; return false; }
    ExecutionTime time_limit;
    if (net::cancelled()) { err = "provider 请求已取消"; return false; }
    if (!ctx_ || !has_resolve_) { err = "该 provider 不提供取流"; return false; }
    JS_UpdateStackTop(rt_);
    JSValue f = JS_GetPropertyStr(ctx_, provider_, "resolve");
    JSValue arg = detail::to_js_song(ctx_, song);
    JSValue aq = mk_str(ctx_, quality);
    JSValue args[2] = {arg, aq};
    JSValue r = JS_Call(ctx_, f, provider_, 2, args);
    JS_FreeValue(ctx_, f);
    JS_FreeValue(ctx_, arg);
    JS_FreeValue(ctx_, aq);
    if (JS_IsException(r)) {
        err = lx::detail::take_exception(ctx_);
        JS_FreeValue(ctx_, r);
        return false;
    }
    if (JS_IsObject(r)) {
        JSValue u = JS_GetPropertyStr(ctx_, r, "url");
        url = lx::detail::from_js(ctx_, u);
        JSValue e = JS_GetPropertyStr(ctx_, r, "error");
        if (url.empty()) err = lx::detail::from_js(ctx_, e);
        JS_FreeValue(ctx_, u);
        JS_FreeValue(ctx_, e);
    } else {
        url = lx::detail::from_js(ctx_, r);
    }
    JS_FreeValue(ctx_, r);
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        if (err.empty()) err = url.empty() ? "provider 没有返回直链" : ("无效直链: " + url);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- 集合

// 平台目录接口统一返回 JSON；平台字段解释只保留在相应 JS 中。
inline bool Provider::request(const char* method, const std::string& args, js::Value& out, std::string& err) {
    const std::string name(method);
    Operation operation(*this, name == "lyric" || name == "playlists" || name == "heats");
    if (!operation || !ctx_) { err = "平台未加载、请求已取消或等待超时"; return false; }
    JSValue f = JS_GetPropertyStr(ctx_, provider_, method);
    JSValue arg = JS_ParseJSON(ctx_, args.c_str(), args.size(), "<platform-args>");
    if (!JS_IsFunction(ctx_, f) || JS_IsException(arg)) {
        if (JS_IsException(arg)) err = lx::detail::take_exception(ctx_);
        else err = std::string("平台没有实现 ") + method;
        JS_FreeValue(ctx_, f); JS_FreeValue(ctx_, arg); return false;
    }
    JSValue r = JS_Call(ctx_, f, provider_, 1, &arg);
    JS_FreeValue(ctx_, f); JS_FreeValue(ctx_, arg);
    if (JS_IsException(r)) { err = lx::detail::take_exception(ctx_); JS_FreeValue(ctx_, r); return false; }
    JSValue json = JS_JSONStringify(ctx_, r, JS_UNDEFINED, JS_UNDEFINED);
    JS_FreeValue(ctx_, r);
    const bool ok = !JS_IsException(json) && js::parse(lx::detail::from_js(ctx_, json), out);
    if (!ok) err = JS_IsException(json) ? lx::detail::take_exception(ctx_) : "平台没有返回 JSON 数据";
    JS_FreeValue(ctx_, json);
    return ok;
}

class Host {
public:
    static Host& instance() {
        static Host h;
        return h;
    }

    int load_dir(const std::string& dir, bool only_if_missing = false) {
        std::lock_guard<std::mutex> loading(load_mu_);
        if (only_if_missing && loaded_.load()) return 0;
        // 首次搜索可能被用户的新搜索取消；不能因此把共享默认平台全部加载成失败。
        net::CancelScope initialization([] { return false; }); // 应用退出信号仍由 net::cancelled 检查。
        std::vector<std::shared_ptr<Provider>> next;
        std::vector<std::pair<std::string, std::string>> failures;
        std::map<std::string, std::string> scripts;
#ifdef AINO_HAS_BUNDLED_PLATFORMS
        for (const auto& script : bundled_scripts) scripts[script.first] = script.second;
#endif
        std::error_code ec;
        for (std::filesystem::directory_iterator it(std::filesystem::u8path(dir), ec), end;
             !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && lx::script_file(it->path())) {
                std::ifstream in(it->path(), std::ios::binary);
                if (!in) { failures.push_back({it->path().filename().u8string(), "无法读取平台脚本"}); continue; }
                std::stringstream code; code << in.rdbuf();
                scripts[it->path().filename().u8string()] = code.str();
            }
        }
        int ok = 0;
        for (const auto& script : scripts) {
            const auto& name = script.first;
            const auto file = std::filesystem::u8path(dir) / std::filesystem::u8path(name);
            const auto path = file.u8string();
            auto p = std::make_shared<Provider>();
            std::string err;
            if (!p->load(path, script.second, err)) {
                failures.push_back({name, err});
                continue;
            }
            next.push_back(std::move(p));
            ++ok;
        }
        { std::lock_guard<std::mutex> lock(mu_); list_.swap(next); fail_.swap(failures); }
        loaded_.store(true);
        return ok;
    }

    std::shared_ptr<Provider> get(const std::string& key) {
        ensure_loaded();
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& p : list_)
            if (p->key() == key) return p;
        return nullptr;
    }

    void ensure_loaded() { if (!loaded_.load()) load_dir(default_dir(), true); }

    std::vector<std::string> keys() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<std::string> out;
        for (auto& p : list_) out.push_back(p->key());
        return out;
    }

    std::vector<std::pair<std::string, std::string>> failures() const {
        std::lock_guard<std::mutex> lock(mu_);
        return fail_;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mu_);
        return list_.empty();
    }

    // 集合只提供 shared_ptr 快照。Provider 锁包含参数构造、JS 调用及释放。
    bool search(const std::string& key, const std::string& kw, int page, int limit, std::vector<Song>& out,
                std::string& err) {
        if (auto p = get(key)) return p->search(kw, page, limit, out, err);
        err = "没有 " + key + " 的 provider";
        return false;
    }

    bool board(const std::string& key, const std::string& board_id, int page, int limit, std::vector<Song>& out,
               std::string& err, bool* more = nullptr, int* total = nullptr) {
        if (auto p = get(key)) return p->board(board_id, page, limit, out, err, more, total);
        err = "没有 " + key + " 的 provider";
        return false;
    }

    bool resolve(const std::string& key, const Song& song, const std::string& quality, std::string& url,
                 std::string& err) {
        if (auto p = get(key)) return p->resolve(song, quality, url, err);
        err = "没有 " + key + " 的 provider";
        return false;
    }

    void set_credentials(const std::string& netease, const std::string& qq) {
        g_cred_netease = netease;
        g_cred_qq = qq;
    }

    bool request(const std::string& key, const char* method, const std::string& args, js::Value& out, std::string& err) {
        if (auto p = get(key)) return p->request(method, args, out, err);
        err = "没有 " + key + " 的平台适配器"; return false;
    }

    bool has(const std::string& key) {
        return bool(get(key));
    }

private:
    mutable std::mutex mu_;
    std::mutex load_mu_;
    std::atomic<bool> loaded_{false};
    std::vector<std::shared_ptr<Provider>> list_;
    std::vector<std::pair<std::string, std::string>> fail_;
};

inline std::string default_dir() {
    if (const char* e = std::getenv("AINO_PROVIDER_DIR")) {
        if (*e) return e;
    }
#ifdef _WIN32
    wchar_t buf[32768]{};
    if (GetModuleFileNameW(nullptr, buf, 32768)) {
        return (std::filesystem::path(buf).parent_path() / "providers").u8string();
    }
#else
    char buf[4096]{};
    const auto n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        const auto dir = std::filesystem::path(std::string(buf, size_t(n))).parent_path() / "providers";
        std::error_code ec;
        if (std::filesystem::is_directory(dir, ec)) return dir.u8string();
    }
    return "/usr/share/aino-player/providers";
#endif
    return "providers";
}

}  // namespace prov
