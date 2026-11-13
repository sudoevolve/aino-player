// provider.hpp —— 平台 API 的 JS 宿主
//
// 为什么要有这层：各平台的搜索 / 榜单 / 取流接口变得比发版快得多。
// 把这类代码留在 C++ 里，等于每次接口一变就要重新编译发版；放在 .js 里，
// 用户改一个文件就能救回来。
//
// 契约（每平台一个文件，providers/<key>.js）：
//
//   globalThis.provider = {
//     key: 'kugou', name: '酷狗',
//     search(keyword, page, limit) -> [song...],
//     board(boardId, page, limit)  -> [song...],
//     resolve(song, quality)       -> 'http://...',   // 可选：不给就走落雪音源
//   }
//
//   song = { id, title, artist, album, duration_ms, bitrate, native_id, cover }
//
// 宿主注入 ctx：
//   ctx.http(url, opts)            同步 HTTP，返回 {statusCode, body(已解析), headers}
//   ctx.lxUrl(key, song, quality)  让落雪音源去取直链
//   ctx.md5 / ctx.b64encode / ctx.b64decode / ctx.log
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "json.hpp"
#include "lxsource.hpp"
#include "net.hpp"

namespace prov {

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
};

// ---------------------------------------------------------------- JS 侧桥

namespace detail {

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
    auto str = [&](const char* k) {
        JSValue x = JS_GetPropertyStr(ctx, v, k);
        std::string r = lx::detail::from_js(ctx, x);
        JS_FreeValue(ctx, x);
        return r;
    };
    auto num = [&](const char* k) {
        JSValue x = JS_GetPropertyStr(ctx, v, k);
        int32_t r = 0;
        JS_ToInt32(ctx, &r, x);   // pres 不能给 nullptr，QuickJS 会直接写它
        JS_FreeValue(ctx, x);
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
    return s;
}

// ctx.lxUrl(key, song, quality)：转手问落雪音源要直链
inline JSValue cb_lx_url(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JSValue out = JS_NewObject(ctx);
    if (argc < 3) {
        JS_DefinePropertyValueStr(ctx, out, "error", lx::detail::to_js(ctx, "lxUrl: 参数不足"), JS_PROP_C_W_E);
        return out;
    }
    const std::string key = lx::detail::from_js(ctx, argv[0]);
    const Song song = song_from_js(ctx, argv[1]);
    const std::string quality = lx::detail::from_js(ctx, argv[2]);

    // 音源要的是 lx 的 musicInfo 形状；这里把 Song 摊平成最常见的那几个字段
    std::string info = "{\"songmid\":" + lx::detail::js_quote(song.id);
    info += ",\"hash\":" + lx::detail::js_quote(song.native_id.empty() ? song.id : song.native_id);
    info += ",\"name\":" + lx::detail::js_quote(song.title);
    info += ",\"singer\":" + lx::detail::js_quote(song.artist);
    info += ",\"albumName\":" + lx::detail::js_quote(song.album);
    info += ",\"source\":" + lx::detail::js_quote(key) + "}";

    std::string url, err;
    if (!lx::Engine::instance().resolve(key, quality, info, url, err)) {
        JS_DefinePropertyValueStr(ctx, out, "error", lx::detail::to_js(ctx, err), JS_PROP_C_W_E);
        return out;
    }
    JS_DefinePropertyValueStr(ctx, out, "url", lx::detail::to_js(ctx, url), JS_PROP_C_W_E);
    return out;
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
    bool board(const std::string& board_id, int page, int limit, std::vector<Song>& out, std::string& err);
    bool resolve(const Song& song, const std::string& quality, std::string& url, std::string& err);

    void close();

private:
    bool eval(const std::string& code, const char* name, std::string& err);
    bool call(const char* fn, int argc, JSValue* argv, std::vector<Song>& out, std::string& err);
    bool call_url(const char* fn, int argc, JSValue* argv, std::string& url, std::string& err);

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
    lxUrl: function (key, song, quality) { return __lxhost.lxUrl(String(key), song, String(quality || '320k')); },
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

// provider 拿不到应用设置，凭据由 Host 在检索前写进来
inline std::mutex g_cred_mutex;
inline std::string g_cred_netease, g_cred_qq;

// ctx.cookie('netease'|'qq') —— 从宿主拿登录 Cookie
inline JSValue host_cookie(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    const std::string key = argc > 0 ? lx::detail::from_js(ctx, argv[0]) : std::string();
    // 由 Host::set_credentials 在每次检索前写入
    std::lock_guard<std::mutex> lock(g_cred_mutex);
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
    JS_SetPropertyStr(ctx_, api, "lxUrl", JS_NewCFunction(ctx_, detail::cb_lx_url, "lxUrl", 3));
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

    JSValue p = JS_GetPropertyStr(ctx_, JS_GetGlobalObject(ctx_), "provider");
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
inline bool Provider::call(const char* fn, int argc, JSValue* argv, std::vector<Song>& out, std::string& err) {
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
    if (!JS_IsArray(r)) {
        err = std::string(fn) + " 没有返回数组";
        JS_FreeValue(ctx_, r);
        return false;
    }
    JSValue lenv = JS_GetPropertyStr(ctx_, r, "length");
    int32_t n = 0;
    JS_ToInt32(ctx_, &n, lenv);
    JS_FreeValue(ctx_, lenv);
    out.clear();
    out.reserve(size_t(n > 0 ? n : 0));
    for (int i = 0; i < n; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx_, r, uint32_t(i));
        if (JS_IsObject(item)) out.push_back(detail::song_from_js(ctx_, item));
        JS_FreeValue(ctx_, item);
    }
    JS_FreeValue(ctx_, r);
    return true;
}

inline JSValue mk_str(JSContext* ctx, const std::string& s) { return lx::detail::to_js(ctx, s); }

inline bool Provider::search(const std::string& keyword, int page, int limit, std::vector<Song>& out,
                             std::string& err) {
    if (!ctx_) { err = "provider 未加载"; return false; }
    JS_UpdateStackTop(rt_);
    JSValue args[3] = {mk_str(ctx_, keyword), JS_NewInt32(ctx_, page), JS_NewInt32(ctx_, limit)};
    const bool ok = call("search", 3, args, out, err);
    for (auto& a : args) JS_FreeValue(ctx_, a);
    return ok;
}

inline bool Provider::board(const std::string& board_id, int page, int limit, std::vector<Song>& out,
                            std::string& err) {
    if (!ctx_) { err = "provider 未加载"; return false; }
    JS_UpdateStackTop(rt_);
    JSValue args[3] = {mk_str(ctx_, board_id), JS_NewInt32(ctx_, page), JS_NewInt32(ctx_, limit)};
    const bool ok = call("board", 3, args, out, err);
    for (auto& a : args) JS_FreeValue(ctx_, a);
    return ok;
}

inline bool Provider::resolve(const Song& song, const std::string& quality, std::string& url, std::string& err) {
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

class Host {
public:
    static Host& instance() {
        static Host h;
        return h;
    }

    int load_dir(const std::string& dir) {
        std::lock_guard<std::mutex> lock(mu_);
        list_.clear();
        fail_.clear();
        std::vector<std::string> files;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(std::filesystem::u8path(dir), ec), end;
             !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && lx::script_file(it->path()))
                files.push_back(it->path().filename().u8string());
        }
        std::sort(files.begin(), files.end());
        int ok = 0;
        for (const auto& name : files) {
            const auto file = std::filesystem::u8path(dir) / std::filesystem::u8path(name);
            const auto path = file.u8string();
            std::ifstream in(file, std::ios::binary);
            if (!in) continue;
            std::stringstream ss;
            ss << in.rdbuf();
            auto p = std::make_unique<Provider>();
            std::string err;
            if (!p->load(path, ss.str(), err)) {
                fail_.push_back({name, err});
                continue;
            }
            list_.push_back(std::move(p));
            ++ok;
        }
        return ok;
    }

    Provider* get(const std::string& key) {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& p : list_)
            if (p->key() == key) return p.get();
        return nullptr;
    }

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

    // 对外三个动作：内部加锁，QuickJS 上下文不能并发调用
    bool search(const std::string& key, const std::string& kw, int page, int limit, std::vector<Song>& out,
                std::string& err) {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& p : list_)
            if (p->key() == key) return p->search(kw, page, limit, out, err);
        err = "没有 " + key + " 的 provider";
        return false;
    }

    bool board(const std::string& key, const std::string& board_id, int page, int limit, std::vector<Song>& out,
               std::string& err) {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& p : list_)
            if (p->key() == key) return p->board(board_id, page, limit, out, err);
        err = "没有 " + key + " 的 provider";
        return false;
    }

    bool resolve(const std::string& key, const Song& song, const std::string& quality, std::string& url,
                 std::string& err) {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& p : list_)
            if (p->key() == key) return p->resolve(song, quality, url, err);
        err = "没有 " + key + " 的 provider";
        return false;
    }

    void set_credentials(const std::string& netease, const std::string& qq) {
        std::lock_guard<std::mutex> lock(g_cred_mutex);
        g_cred_netease = netease;
        g_cred_qq = qq;
    }

    bool has(const std::string& key) {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& p : list_)
            if (p->key() == key) return true;
        return false;
    }

private:
    mutable std::mutex mu_;
    std::vector<std::unique_ptr<Provider>> list_;
    std::vector<std::pair<std::string, std::string>> fail_;
};

inline std::string default_dir() {
    if (const char* e = std::getenv("AINO_PROVIDER_DIR")) {
        if (*e) return e;
    }
#ifdef _WIN32
    char buf[MAX_PATH]{};
    if (GetModuleFileNameA(nullptr, buf, MAX_PATH)) {
        std::string p(buf);
        const size_t cut = p.find_last_of("\\/");
        if (cut != std::string::npos) return p.substr(0, cut) + "\\providers";
    }
#endif
    return "providers";
}

}  // namespace prov
