#pragma once
#include "platform.hpp"

namespace platform {
/** @brief UI 线程独占的原生歌词窗口；共享主窗口字体绘制器，最多保留三个 SHM 缓冲。 */
class WaylandLyrics {
  public:
    WaylandLyrics();
    ~WaylandLyrics();
    WaylandLyrics(const WaylandLyrics &) = delete;
    WaylandLyrics &operator=(const WaylandLyrics &) = delete;
    bool update(LyricOverlaySettings &, const std::string &, const std::string &, const LyricOverlayProgress &, float,
                const LyricPainter &);
    void close(bool final = false);
    bool event_ready() const;
    bool ready() const;
    bool uses_layer_shell() const;
    int event_wait_ms() const { return 100; }
    std::function<void()> wake;
    std::string error;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace platform
