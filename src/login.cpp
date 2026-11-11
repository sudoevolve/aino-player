// login.cpp —— WebView2 登录窗口的实现（QQ 音乐 / 网易云音乐）
//
// 流程：开一个独立小窗口 → 加载站点首页 → 用户扫码/密码登录 →
// 每 1.5 秒用 ICoreWebView2CookieManager 抓一次该域的 cookie →
// 看到站点自己的登录凭据（QQ: qm_keyst，网易云: MUSIC_U）就认为登录成功，
// 把 cookie 串回传并自动关窗。
//
// 为什么要独立线程 + 独立窗口：aino 的主循环靠 WaitMessage 阻塞，如果把 WebView2
// 塞进主窗口，消息循环会互相打架（而且 WebView2 要求在自己线程里 pump 消息）。
#pragma once

#include "login.h"

#include <windows.h>
#include <wrl.h>

#include <cstdarg>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "../vendor/webview2/include/WebView2.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace login {
namespace {

// 每个站点只需要这几条差异，剩下的窗口/线程/抓 cookie 逻辑完全共用
struct SiteCfg {
    const wchar_t* title;
    const wchar_t* url;
    const wchar_t* cookie_uri;
    const wchar_t* cls;
    const char* host;            // 只用于日志和提示文案
    const char* success_cookie;  // 出现这个 cookie 值 ≠ 空 就认定登录成功
    const char* uid_key;         // 能从 cookie 直接读到的用户 id（网易云没有，填空）
    bool strip_leading_o;        // QQ 的 uin 可能是 "o0123456789"
};

const SiteCfg& cfg_of(Site s) {
    static const SiteCfg kQq = {L"登录 QQ 音乐 — 登录成功后窗口会自动关闭", L"https://y.qq.com/",
                                L"https://y.qq.com/", L"aino_login_qq", "y.qq.com", "qm_keyst", "uin", true};
    static const SiteCfg kNe = {L"登录网易云音乐 — 登录成功后窗口会自动关闭", L"https://music.163.com/",
                                L"https://music.163.com/", L"aino_login_netease", "music.163.com", "MUSIC_U",
                                nullptr, false};
    return s == Site::QQ ? kQq : kNe;
}

void logf(const SiteCfg& cfg, const char* fmt, ...) {
    wchar_t tmp[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring name = L"aino-login-";
    for (const char* p = cfg.host; *p; ++p) name += wchar_t(*p);
    std::wstring wpath = std::wstring(tmp) + name + L".log";
    std::FILE* f = _wfopen(wpath.c_str(), L"a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fclose(f);
}

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

std::wstring take_lpstr(LPWSTR p) {
    std::wstring s = p ? p : L"";
    if (p) CoTaskMemFree(p);
    return s;
}

// 两个站点各用一份 user data 目录：登录态互不干扰，退出登录也好清
std::string user_data_dir(Site s) {
    wchar_t buf[MAX_PATH] = {0};
    std::string base;
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) base = to_utf8(buf);
    if (base.empty()) base = ".";
    return base + "\\aino-player\\webview2\\" + (s == Site::QQ ? "qq" : "netease");
}

struct Ctx {
    Site site = Site::QQ;
    HWND hwnd = nullptr;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    ComPtr<ICoreWebView2CookieManager> cookies;
    bool finished = false;
};

void set_note(Site s, const std::string& note) {
    auto& st = state(s);
    std::lock_guard<std::mutex> lk(st.m);
    st.note = note;
}

void finish(const std::shared_ptr<Ctx>& ctx, const std::string& cookie, const std::string& uid,
            const std::string& note) {
    if (ctx->finished) return;
    ctx->finished = true;
    auto& st = state(ctx->site);
    {
        std::lock_guard<std::mutex> lk(st.m);
        st.cookie = cookie;
        st.uid = uid;
        st.note = note;
    }
    st.done.store(true);
    if (ctx->hwnd) PostMessageW(ctx->hwnd, WM_CLOSE, 0, 0);
}

void poll_cookies(const std::shared_ptr<Ctx>& ctx) {
    if (!ctx->cookies) return;
    const SiteCfg& cfg = cfg_of(ctx->site);
    ctx->cookies->GetCookies(cfg.cookie_uri, Callback<ICoreWebView2GetCookiesCompletedHandler>(
                                                 [ctx](HRESULT hr, ICoreWebView2CookieList* list) -> HRESULT {
                                                     if (FAILED(hr) || !list) return S_OK;
                                                     if (ctx->finished) return S_OK;
                                                     const SiteCfg& c = cfg_of(ctx->site);

                                                     UINT count = 0;
                                                     list->get_Count(&count);
                                                     std::string jar;
                                                     std::string uid_raw;
                                                     std::string token;
                                                     for (UINT i = 0; i < count; ++i) {
                                                         ComPtr<ICoreWebView2Cookie> ck;
                                                         if (FAILED(list->GetValueAtIndex(i, &ck)) || !ck) continue;
                                                         const std::wstring name = take_lpstr([&] {
                                                             LPWSTR p = nullptr;
                                                             ck->get_Name(&p);
                                                             return p;
                                                         }());
                                                         const std::wstring value = take_lpstr([&] {
                                                             LPWSTR p = nullptr;
                                                             ck->get_Value(&p);
                                                             return p;
                                                         }());
                                                         if (name.empty()) continue;
                                                         const std::string n8 = to_utf8(name), v8 = to_utf8(value);
                                                         if (!jar.empty()) jar += "; ";
                                                         jar += n8 + "=" + v8;
                                                         if (n8 == c.success_cookie) token = v8;
                                                         if (c.uid_key && n8 == c.uid_key) uid_raw = v8;
                                                     }

                                                     set_note(ctx->site,
                                                              (token.empty() ? std::string("已加载 ") + c.host +
                                                                                   "，等待登录…"
                                                                             : std::string("已加载 ") + c.host +
                                                                                   "，检测到登录态"));

                                                     if (token.size() > 8) {
                                                         std::string uid = uid_raw;
                                                         if (c.strip_leading_o && uid.size() > 1 &&
                                                                     (uid[0] == 'o' || uid[0] == 'O'))
                                                             uid = uid.substr(1);
                                                         finish(ctx, jar, uid, "登录成功");
                                                     }
                                                     return S_OK;
                                                 })
                                                 .Get());
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<Ctx*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_SIZE:
            if (ctx && ctx->controller) {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                ctx->controller->put_Bounds(rc);
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void run_window(Site site) {
    const SiteCfg& cfg = cfg_of(site);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto ctx = std::make_shared<Ctx>();
    ctx->site = site;

    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = cfg.cls;
    RegisterClassExW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int w = 620, h = 780;
    const int x = work.left + (work.right - work.left - w) / 2;
    const int y = work.top + (work.bottom - work.top - h) / 2;

    HWND hwnd = CreateWindowExW(0, cfg.cls, cfg.title, WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, inst,
                                nullptr);
    if (!hwnd) {
        logf(cfg, "CreateWindowExW failed err=%lu", GetLastError());
        set_note(site, "创建登录窗口失败");
        state(site).running.store(false);
        state(site).done.store(true);
        CoUninitialize();
        return;
    }
    ctx->hwnd = hwnd;
    logf(cfg, "window created hwnd=%p", (void*)hwnd);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx.get()));
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    set_note(site, "正在启动内置浏览器…");

    const std::wstring data_dir = to_wide(user_data_dir(site));
    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, data_dir.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [ctx, cfg](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                logf(cfg, "env hr=0x%08lx env=%p", (unsigned long)hr, (void*)env);
                if (FAILED(hr) || !env) {
                    finish(ctx, std::string(), std::string(), "WebView2 初始化失败（没装 Edge WebView2 运行时？）");
                    return S_OK;
                }
                env->CreateCoreWebView2Controller(
                    ctx->hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [ctx, cfg](HRESULT hr2, ICoreWebView2Controller* ctrl) -> HRESULT {
                            logf(cfg, "controller hr=0x%08lx ctrl=%p", (unsigned long)hr2, (void*)ctrl);
                            if (FAILED(hr2) || !ctrl) {
                                finish(ctx, std::string(), std::string(), "创建浏览器控件失败");
                                return S_OK;
                            }
                            ctx->controller = ctrl;
                            ctrl->get_CoreWebView2(&ctx->webview);
                            ComPtr<ICoreWebView2_2> newer;
                            if (SUCCEEDED(ctx->webview.As(&newer)) && newer) newer->get_CookieManager(&ctx->cookies);

                            RECT rc{};
                            GetClientRect(ctx->hwnd, &rc);
                            ctrl->put_Bounds(rc);
                            ctx->webview->Navigate(cfg.url);
                            SetTimer(ctx->hwnd, 1, 1500, nullptr);
                            set_note(ctx->site, std::string("已打开 ") + cfg.host + "，请扫码或账号登录");
                            return S_OK;
                        })
                        .Get());
                return S_OK;
            })
            .Get());

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        // 定时器 → 抓 cookie
        if (msg.message == WM_TIMER && msg.hwnd == hwnd && msg.wParam == 1) {
            poll_cookies(ctx);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    KillTimer(hwnd, 1);
    if (ctx->controller) ctx->controller->Close();

    auto& st = state(site);
    st.running.store(false);
    if (!st.done.load()) {
        // 用户直接关窗 = 取消
        std::lock_guard<std::mutex> lk(st.m);
        st.note = "已取消登录";
        st.done.store(true);
    }
    CoUninitialize();
}

State g_states[kSiteCount];

}  // namespace

State& state(Site s) { return g_states[int(s)]; }

bool runtime_available() {
    LPWSTR ver = nullptr;
    const HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &ver);
    if (ver) CoTaskMemFree(ver);
    return SUCCEEDED(hr);
}

void start(Site site) {
    auto& st = state(site);
    if (st.running.exchange(true)) return;  // 已经开着
    {
        std::lock_guard<std::mutex> lk(st.m);
        st.cookie.clear();
        st.uid.clear();
        st.note = "准备打开登录窗口…";
    }
    st.done.store(false);
    std::thread([site] { run_window(site); }).detach();
}

}  // namespace login
