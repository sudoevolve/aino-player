// login.h —— 内置浏览器登录（QQ 音乐 / 网易云音乐共用一套流程）
//
// 为什么不读浏览器的 cookie：
//   Chrome 127+ / Edge 用的是 App-Bound Encryption，cookie 值用应用绑定的密钥加密，
//   外部进程拿不到主密钥（拿了也解不开），而且还要处理 DPAPI 的 user scope。
//   与其做一堆"可能明天就失效"的破解，不如用系统自带的 WebView2 开一个真浏览器窗口：
//   用户扫一次码，cookie 存在自己的 user data 目录里，之后启动就是登录态。
//
// 这个头故意不 include WebView2.h（那个头 6 万行），实现放在 login.cpp 里，
// 免得每次改主程序都要重编一遍 2.5MB 的头。
#pragma once

#include <atomic>
#include <mutex>
#include <string>

namespace login {

enum class Site { QQ = 0, NetEase = 1 };
constexpr int kSiteCount = 2;

struct State {
    std::mutex m;
    std::atomic<bool> running{false};   // 窗口是否开着
    std::atomic<bool> done{false};      // 一次尝试是否结束（成功 / 取消 / 失败）
    std::string cookie;                 // 成功时：形如 "a=b; c=d"
    std::string uid;                    // 成功时：QQ 号 / 网易云 userId（拿不到就空着）
    std::string note;                   // 阶段说明，给界面显示
};

State& state(Site s);

// 后台线程打开登录窗口并等待登录；不阻塞调用方。重复调用会忽略。
void start(Site s);

// 检查是否装了 WebView2 运行时
bool runtime_available();

}  // namespace login
