// 外部默认浏览器打开官网，独立临时会话回传登录态；不读取日常浏览器数据库。
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <functional>
#include <vector>

namespace login {

enum class Site { QQ = 0, NetEase = 1, YouTube = 2 };
constexpr int kSiteCount = 3;

struct State {
    std::mutex m;
    std::atomic<bool> running{false};   // 本机登录服务是否运行
    std::atomic<bool> done{false};      // 一次尝试是否结束（成功 / 取消 / 失败）
    std::atomic<bool> return_requested{false};
    std::string cookie;                 // 成功时：形如 "a=b; c=d"
    std::string uid;                    // 成功时：QQ 号 / 网易云 userId（拿不到就空着）
    std::string note;                   // 阶段说明，给界面显示
    std::string portal_url;
    std::string context;                // YouTube 当前频道、访客和账号索引，不含额外站点凭据。
};

State& state(Site s);

// 后台线程打开默认浏览器官网；false 仅启动备用导入入口。未完成时忽略重复调用。
void start(Site s, bool launch_browser = true);
// 独立会话的浏览器启动参数；空路径自动选择默认浏览器，headless 仅供离线回归。
struct BrowserOptions {
    std::string executable;
    bool headless = false;
    std::function<void(const std::string&)> on_ready;
};
void start_browser(Site site, BrowserOptions options);
void cancel(Site s);
void forget(Site s);
void shutdown();

// 纯解析；不回显凭据，返回缺失字段信息。
bool parse_credentials(Site s, const std::string& input, std::string& cookie, std::string& uid, std::string& error);

struct Stream {
    std::string url, error;
    std::vector<std::string> headers;
};
// 由系统浏览器的官网播放器处理签名与平台校验，只返回音轨地址；不嵌入 WebView。
Stream youtube_stream(const std::string& video_id, const std::string& cookie,
                      const std::function<bool()>& cancel = {}, const BrowserOptions& options = {});

}  // namespace login
