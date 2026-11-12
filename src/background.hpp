// 后台任务由应用拥有；退出时等待完成，避免线程继续访问已销毁的窗口与状态。
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
namespace background {
struct Task { std::thread thread; std::shared_ptr<std::atomic<bool>> done; };
inline std::mutex mutex;
inline std::vector<Task> tasks;
inline bool accepting = true;
// 及时回收已结束线程的可连接状态；锁外 join，避免阻塞任务注册。
inline void reap() {
    std::vector<std::thread> finished;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = tasks.begin(); it != tasks.end();) {
            if (it->done->load()) { finished.push_back(std::move(it->thread)); it = tasks.erase(it); }
            else ++it;
        }
    }
    for (auto& thread : finished) if (thread.joinable()) thread.join();
}
template<class F, class... Args> void start(F&& fn, Args&&... args) {
    reap();
    std::lock_guard<std::mutex> lock(mutex);
    if (!accepting) return;
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto action = std::bind(std::forward<F>(fn), std::forward<Args>(args)...);
    tasks.push_back({std::thread([action = std::move(action), done]() mutable { action(); done->store(true); }), done});
}
inline void stop() {
    std::vector<Task> pending;
    { std::lock_guard<std::mutex> lock(mutex); accepting = false; pending.swap(tasks); }
    for (auto& task : pending) if (task.thread.joinable()) task.thread.join();
}
} // namespace background
