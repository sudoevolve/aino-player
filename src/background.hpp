// 后台任务由应用拥有；退出时等待完成，避免线程继续访问已销毁的窗口与状态。
#pragma once
#include <atomic>
#include <functional>
#include <cstdio>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include <condition_variable>
#include <stdexcept>
namespace background {
struct Task { std::thread thread; std::shared_ptr<std::atomic<bool>> done; };
inline std::mutex mutex;
inline std::vector<Task> tasks;
inline bool accepting = true;
inline std::mutex error_mutex;
inline std::deque<std::string> errors;
inline std::atomic<bool> error_pending{false};
inline void report_error(const char* message) noexcept {
    std::fprintf(stderr, "Background task failed: %s\n", message);
    try {
        std::lock_guard<std::mutex> lock(error_mutex);
        if (errors.size() == 16) errors.pop_front();
        errors.emplace_back(message);
        error_pending.store(true);
    } catch (...) {} // 内存不足时至少保留 stderr，不让错误报告再次终止线程。
}
inline std::deque<std::string> take_errors() {
    std::deque<std::string> result;
    std::lock_guard<std::mutex> lock(error_mutex);
    result.swap(errors);
    error_pending.store(false);
    return result;
}
/** @brief 拥有一组短期线程；创建中途失败和提前返回也会回收已创建的线程。 */
class ThreadGroup {
public:
    ThreadGroup() = default;
    ThreadGroup(const ThreadGroup&) = delete;
    ~ThreadGroup() { join(); }
    template<class F> void emplace_back(F&& action) {
        // 先分配空槽，避免容器分配失败时析构临时的可连接线程。
        threads_.emplace_back();
        try {
            threads_.back() = std::thread([fn = std::forward<F>(action)]() mutable {
                try { fn(); }
                catch (const std::exception& error) { report_error(error.what()); }
                catch (...) { report_error("Worker failed"); }
            });
        } catch (...) { threads_.pop_back(); throw; }
    }
    size_t size() const { return threads_.size(); }
    void join() { for (auto& thread : threads_) if (thread.joinable()) thread.join(); }
private:
    std::vector<std::thread> threads_;
};
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
template<class F, class... Args> void service(F&& fn, Args&&... args) {
    reap();
    std::lock_guard<std::mutex> lock(mutex);
    if (!accepting) return;
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto action = std::bind(std::forward<F>(fn), std::forward<Args>(args)...);
    // 先分配容器位置，再创建线程，避免 push_back 失败时销毁仍可连接的临时线程。
    tasks.push_back({{}, done});
    try {
        tasks.back().thread = std::thread([action = std::move(action), done]() mutable {
            try { action(); }
            catch (const std::exception& error) { report_error(error.what()); }
            catch (...) { report_error("Unknown exception"); }
            done->store(true);
        });
    } catch (...) { tasks.pop_back(); throw; }
}
// 普通任务最多三个并行；第四个工作线程保留给点播，常驻写入 / 唤醒任务另行管理。
struct WorkQueue {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::function<void()>> normal, interactive;
    bool closing = false;
    ThreadGroup workers;
    ~WorkQueue() {
        { std::lock_guard<std::mutex> lock(mutex); closing = true; }
        changed.notify_all(); workers.join();
    }
    void run(bool reserved) {
        for (;;) {
            std::function<void()> action;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [&] { return closing || !interactive.empty() || (!reserved && !normal.empty()); });
                if (interactive.empty() && (reserved || normal.empty())) {
                    if (closing) return;
                    continue;
                }
                auto& queue = interactive.empty() ? normal : interactive;
                action = std::move(queue.front()); queue.pop_front();
            }
            try { action(); }
            catch (const std::exception& error) { report_error(error.what()); }
            catch (...) { report_error("Background job failed"); }
        }
    }
    void submit(std::function<void()> action, bool urgent) {
        std::lock_guard<std::mutex> lock(mutex);
        if (closing) throw std::runtime_error("Background queue stopped");
        if (normal.size() + interactive.size() >= 256) throw std::runtime_error("Too many background tasks");
        if (workers.size() == 0) {
            // 即使后续创建失败，已创建线程仍由 workers 持有；下次提交可继续使用。
            workers.emplace_back([this] { run(false); });
            try {
                workers.emplace_back([this] { run(false); });
                workers.emplace_back([this] { run(false); });
                workers.emplace_back([this] { run(true); });
            } catch (const std::exception& error) { report_error(error.what()); }
        }
        (urgent ? interactive : normal).push_back(std::move(action)); changed.notify_all();
    }
};
inline WorkQueue work_queue;
template<class F, class... Args> void job(F&& fn, Args&&... args) {
    work_queue.submit(std::bind(std::forward<F>(fn), std::forward<Args>(args)...), false);
}
// 保留旧调用契约供尚未迁移的常驻任务；新短任务使用 job / guarded。
template<class F, class... Args> void start(F&& fn, Args&&... args) {
    service(std::forward<F>(fn), std::forward<Args>(args)...);
}
template<class F, class... Args> void foreground(F&& fn, Args&&... args) {
    work_queue.submit(std::bind(std::forward<F>(fn), std::forward<Args>(args)...), true);
}
/** @brief 操作失败时执行自己的状态回收；排队 / 创建失败与执行失败走同一路径。 */
template<class F, class E> void guarded(F action, E failed, bool urgent = false) {
    auto run = [action = std::move(action), failed]() mutable {
        try { action(); }
        catch (const std::exception& error) { failed(error.what()); }
        catch (...) { failed("Unknown background error"); }
    };
    try { work_queue.submit(std::move(run), urgent); }
    catch (const std::exception& error) { failed(error.what()); }
}
inline void stop() {
    { std::lock_guard<std::mutex> lock(work_queue.mutex); work_queue.closing = true; }
    work_queue.changed.notify_all(); work_queue.workers.join();
    std::vector<Task> pending;
    { std::lock_guard<std::mutex> lock(mutex); accepting = false; pending.swap(tasks); }
    for (auto& task : pending) if (task.thread.joinable()) task.thread.join();
}
} // namespace background
