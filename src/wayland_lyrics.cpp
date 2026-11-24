#include "wayland_lyrics.hpp"
#include "xdg-shell-client.h"
#include "wlr-layer-shell-client.h"
#include <wayland-client.h>
#include <sys/mman.h>
#include <poll.h>
#include <cmath>
#include <cstring>
#include <list>
#include <tuple>

namespace platform {
struct WaylandLyrics::Impl {
    WaylandLyrics *owner;
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_compositor *compositor = nullptr;
    wl_shm *shm = nullptr;
    xdg_wm_base *shell = nullptr;
    zwlr_layer_shell_v1 *layer_shell = nullptr;
    wl_surface *surface = nullptr;
    xdg_surface *xdg = nullptr;
    xdg_toplevel *toplevel = nullptr;
    zwlr_layer_surface_v1 *layer = nullptr;
    wl_seat *seat = nullptr;
    wl_pointer *pointer = nullptr;
    wl_callback *frame = nullptr;
    std::atomic<int> fd{-1}; // worker 只探测可读性，不读取或派发 Wayland 连接。
    struct Output {
        Impl *parent;
        wl_output *object;
        uint32_t name;
        int scale = 1, width = 0, height = 0;
        bool entered = false;
    };
    std::list<Output> outputs;
    struct Buffer {
        wl_buffer *object = nullptr;
        void *pixels = MAP_FAILED;
        size_t bytes = 0;
        int width = 0, height = 0;
        bool busy = false;
        ~Buffer() {
            if (object)
                wl_buffer_destroy(object);
            if (pixels != MAP_FAILED)
                munmap(pixels, bytes);
        }
    };
    std::vector<std::unique_ptr<Buffer>> buffers;
    LyricOverlaySettings settings;
    std::string first, second;
    LyricOverlayProgress progress;
    int width = 640, height = 100, scale = 1, requested_w = 0, requested_h = 0;
    int x = 0, y = 32;
    double pointer_x = 0, pointer_y = 0;
    bool configured = false, dirty = true, closed = false, moved = false, dragging = false;
    std::atomic<bool> flush_blocked{false};
    unsigned long long retry_at = 0;
    explicit Impl(WaylandLyrics *parent) : owner(parent) {}
    ~Impl() { close(); }
    void notify() {
        dirty = true;
        if (owner->wake)
            owner->wake();
    }
    void close() {
        fd.store(-1);
        if (frame)
            wl_callback_destroy(frame);
        frame = nullptr;
        buffers.clear();
        if (pointer)
            wl_pointer_destroy(pointer);
        if (seat)
            wl_seat_destroy(seat);
        if (layer)
            zwlr_layer_surface_v1_destroy(layer);
        if (toplevel)
            xdg_toplevel_destroy(toplevel);
        if (xdg)
            xdg_surface_destroy(xdg);
        if (surface)
            wl_surface_destroy(surface);
        if (layer_shell) {
            if (zwlr_layer_shell_v1_get_version(layer_shell) >= 3)
                zwlr_layer_shell_v1_destroy(layer_shell);
            else
                wl_proxy_destroy(reinterpret_cast<wl_proxy *>(layer_shell));
        }
        if (shell)
            xdg_wm_base_destroy(shell);
        for (auto &output : outputs)
            wl_output_destroy(output.object);
        outputs.clear();
        if (shm)
            wl_shm_destroy(shm);
        if (compositor)
            wl_compositor_destroy(compositor);
        if (registry)
            wl_registry_destroy(registry);
        if (display)
            wl_display_disconnect(display);
        display = nullptr;
        registry = nullptr;
        compositor = nullptr;
        shm = nullptr;
        shell = nullptr;
        layer_shell = nullptr;
        surface = nullptr;
        xdg = nullptr;
        toplevel = nullptr;
        layer = nullptr;
        seat = nullptr;
        pointer = nullptr;
        configured = false;
        dragging = false;
        dirty = true;
        flush_blocked = false;
        requested_w = requested_h = 0;
    }
    void update_scale() {
        int next = 1;
        for (const auto &output : outputs)
            if (output.entered)
                next = std::max(next, output.scale);
        if (next != scale) {
            scale = next;
            notify();
        }
    }
    void geometry() {
        const int next_w = std::clamp(settings.width, 240, 1600);
        const int next_h =
            int(std::ceil(std::clamp(settings.font_size, 14, 64) * 1.65f)) * (settings.double_line ? 2 : 1) +
            (settings.placement ? 4 : 14);
        if (next_w != requested_w || next_h != requested_h) {
            requested_w = next_w;
            requested_h = next_h;
            if (layer)
                zwlr_layer_surface_v1_set_size(layer, next_w, next_h);
            else {
                width = next_w;
                height = next_h;
            }
            dirty = true;
        }
        if (layer) {
            const bool docked = settings.placement != 0;
            for (const auto &output : outputs)
                if ((output.entered || outputs.size() == 1) && output.width > 0 && output.height > 0) {
                    if (docked) {
                        x = (output.width / output.scale - next_w) / 2 + settings.panel_offset;
                        y = 0;
                    }
                    x = std::clamp(x, 0, std::max(0, output.width / output.scale - next_w));
                    y = std::clamp(y, 0, std::max(0, output.height / output.scale - next_h));
                    break;
                }
            zwlr_layer_surface_v1_set_anchor(layer,
                                             ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
            zwlr_layer_surface_v1_set_margin(layer, docked ? 0 : y, 0, 0, x);
            zwlr_layer_surface_v1_set_exclusive_zone(layer, docked ? -1 : 0);
            if (zwlr_layer_surface_v1_get_version(layer) >= 2)
                zwlr_layer_surface_v1_set_layer(layer, settings.topmost || docked ? ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY
                                                                                  : ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM);
        }
        if (surface) {
            wl_region *region = settings.locked ? wl_compositor_create_region(compositor) : nullptr;
            wl_surface_set_input_region(surface, region);
            if (region)
                wl_region_destroy(region);
            wl_surface_commit(surface);
        }
    }
    void add_output(uint32_t name, uint32_t version) {
        auto *object =
            static_cast<wl_output *>(wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 2u)));
        outputs.push_back({this, object, name});
        static const wl_output_listener listener{
            [](void *, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t, const char *, const char *, int32_t) {
            },
            [](void *data, wl_output *, uint32_t flags, int32_t w, int32_t h, int32_t) {
                auto *output = static_cast<Output *>(data);
                if (flags & WL_OUTPUT_MODE_CURRENT) {
                    output->width = w;
                    output->height = h;
                    if (output->parent->layer)
                        output->parent->geometry();
                }
            },
            [](void *, wl_output *) {},
            [](void *data, wl_output *, int32_t value) {
                auto *output = static_cast<Output *>(data);
                output->scale = std::clamp(value, 1, 8);
                output->parent->update_scale();
            },
            nullptr,
            nullptr};
        wl_output_add_listener(object, &listener, &outputs.back());
    }
    void attach_pointer() {
        if (pointer || !seat)
            return;
        pointer = wl_seat_get_pointer(seat);
        static const wl_pointer_listener listener = [] {
            wl_pointer_listener value{};
            value.enter = [](void *data, wl_pointer *, uint32_t, wl_surface *, wl_fixed_t x, wl_fixed_t y) {
                auto *self = static_cast<Impl *>(data);
                self->pointer_x = wl_fixed_to_double(x);
                self->pointer_y = wl_fixed_to_double(y);
            };
            value.leave = [](void *data, wl_pointer *, uint32_t, wl_surface *) {
                static_cast<Impl *>(data)->dragging = false;
            };
            value.motion = [](void *data, wl_pointer *, uint32_t, wl_fixed_t x, wl_fixed_t y) {
                auto *self = static_cast<Impl *>(data);
                const double px = wl_fixed_to_double(x), py = wl_fixed_to_double(y);
                if (self->dragging && self->layer) {
                    self->x = std::max(0, self->x + int(px - self->pointer_x));
                    self->y = std::max(0, self->y + int(py - self->pointer_y));
                    self->moved = true;
                    self->settings.placement = 0;
                    self->geometry();
                    self->notify();
                } else {
                    self->pointer_x = px;
                    self->pointer_y = py;
                }
            };
            value.button = [](void *data, wl_pointer *, uint32_t serial, uint32_t, uint32_t button, uint32_t state) {
                auto *self = static_cast<Impl *>(data);
                if (button == 0x110) {
                    self->dragging = state == WL_POINTER_BUTTON_STATE_PRESSED && !self->settings.locked;
                    if (self->dragging && self->toplevel)
                        xdg_toplevel_move(self->toplevel, self->seat, serial);
                } else if (button == 0x111 && state == WL_POINTER_BUTTON_STATE_PRESSED && !self->settings.locked) {
                    self->closed = true;
                    self->notify();
                }
            };
            value.axis = [](void *, wl_pointer *, uint32_t, uint32_t, wl_fixed_t) {};
            return value;
        }();
        wl_pointer_add_listener(pointer, &listener, this);
    }
    bool create() {
        if (display)
            return true;
        display = wl_display_connect(nullptr);
        if (!display) {
            owner->error = "无法连接 Wayland 歌词窗口";
            return false;
        }
        registry = wl_display_get_registry(display);
        static const wl_registry_listener listener{
            [](void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
                auto *self = static_cast<Impl *>(data);
                if (!std::strcmp(interface, "wl_compositor") && version >= 4)
                    self->compositor =
                        static_cast<wl_compositor *>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
                else if (!std::strcmp(interface, "wl_shm"))
                    self->shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
                else if (!std::strcmp(interface, "xdg_wm_base"))
                    self->shell =
                        static_cast<xdg_wm_base *>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
                else if (!std::strcmp(interface, "zwlr_layer_shell_v1"))
                    self->layer_shell = static_cast<zwlr_layer_shell_v1 *>(
                        wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, std::min(4u, version)));
                else if (!std::strcmp(interface, "wl_output"))
                    self->add_output(name, version);
                else if (!std::strcmp(interface, "wl_seat") && !self->seat) {
                    self->seat = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
                    static const wl_seat_listener seat_listener{[](void *data, wl_seat *, uint32_t capabilities) {
                                                                    auto *self = static_cast<Impl *>(data);
                                                                    if (capabilities & WL_SEAT_CAPABILITY_POINTER)
                                                                        self->attach_pointer();
                                                                    else if (self->pointer) {
                                                                        wl_pointer_destroy(self->pointer);
                                                                        self->pointer = nullptr;
                                                                        self->dragging = false;
                                                                    }
                                                                },
                                                                nullptr};
                    wl_seat_add_listener(self->seat, &seat_listener, self);
                }
            },
            [](void *data, wl_registry *, uint32_t name) {
                auto *self = static_cast<Impl *>(data);
                for (auto it = self->outputs.begin(); it != self->outputs.end(); ++it)
                    if (it->name == name) {
                        wl_output_destroy(it->object);
                        self->outputs.erase(it);
                        self->update_scale();
                        break;
                    }
            }};
        wl_registry_add_listener(registry, &listener, this);
        if (wl_display_roundtrip(display) < 0 || !compositor || !shm || (!layer_shell && !shell)) {
            owner->error = "桌面未提供歌词所需的 Wayland 协议";
            close();
            return false;
        }
        if (shell) {
            static const xdg_wm_base_listener ping{
                [](void *, xdg_wm_base *object, uint32_t serial) { xdg_wm_base_pong(object, serial); }};
            xdg_wm_base_add_listener(shell, &ping, this);
        }
        surface = wl_compositor_create_surface(compositor);
        if (!surface) {
            owner->error = "无法创建歌词表面";
            close();
            return false;
        }
        static const wl_surface_listener surface_listener = [] {
            wl_surface_listener value{};
            value.enter = [](void *data, wl_surface *, wl_output *output) {
                auto *self = static_cast<Impl *>(data);
                for (auto &item : self->outputs)
                    if (item.object == output)
                        item.entered = true;
                self->update_scale();
            };
            value.leave = [](void *data, wl_surface *, wl_output *output) {
                auto *self = static_cast<Impl *>(data);
                for (auto &item : self->outputs)
                    if (item.object == output)
                        item.entered = false;
                self->update_scale();
            };
            return value;
        }();
        wl_surface_add_listener(surface, &surface_listener, this);
        x = settings.x == -32000 ? 32 : std::max(0, settings.x);
        y = settings.y == -32000 ? 32 : std::max(0, settings.y);
        if (layer_shell) {
            layer = zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface, nullptr,
                                                          settings.topmost || settings.placement
                                                              ? ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY
                                                              : ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
                                                          "aino-player-lyrics");
            if (!layer) {
                owner->error = "无法创建歌词图层";
                close();
                return false;
            }
            static const zwlr_layer_surface_v1_listener layer_listener{
                [](void *data, zwlr_layer_surface_v1 *object, uint32_t serial, uint32_t w, uint32_t h) {
                    auto *self = static_cast<Impl *>(data);
                    zwlr_layer_surface_v1_ack_configure(object, serial);
                    self->width = int(w ? w : self->requested_w);
                    self->height = int(h ? h : self->requested_h);
                    self->configured = true;
                    self->notify();
                },
                [](void *data, zwlr_layer_surface_v1 *) {
                    auto *self = static_cast<Impl *>(data);
                    self->closed = true;
                    self->notify();
                }};
            zwlr_layer_surface_v1_add_listener(layer, &layer_listener, this);
            zwlr_layer_surface_v1_set_keyboard_interactivity(layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        } else {
            xdg = xdg_wm_base_get_xdg_surface(shell, surface);
            if (!xdg) {
                owner->error = "无法创建歌词窗口角色";
                close();
                return false;
            }
            static const xdg_surface_listener configure{[](void *data, xdg_surface *object, uint32_t serial) {
                auto *self = static_cast<Impl *>(data);
                xdg_surface_ack_configure(object, serial);
                self->configured = true;
                self->notify();
            }};
            xdg_surface_add_listener(xdg, &configure, this);
            toplevel = xdg_surface_get_toplevel(xdg);
            if (!toplevel) {
                owner->error = "无法创建歌词窗口";
                close();
                return false;
            }
            static const xdg_toplevel_listener toplevel_listener = [] {
                xdg_toplevel_listener value{};
                value.configure = [](void *data, xdg_toplevel *, int32_t w, int32_t h, wl_array *) {
                    auto *self = static_cast<Impl *>(data);
                    if (w > 0)
                        self->width = w;
                    if (h > 0)
                        self->height = h;
                    self->notify();
                };
                value.close = [](void *data, xdg_toplevel *) {
                    auto *self = static_cast<Impl *>(data);
                    self->closed = true;
                    self->notify();
                };
                return value;
            }();
            xdg_toplevel_add_listener(toplevel, &toplevel_listener, this);
            xdg_toplevel_set_title(toplevel, "Aino Player · 歌词（右键关闭）");
            xdg_toplevel_set_app_id(toplevel, "aino-player");
        }
        geometry();
        fd.store(wl_display_get_fd(display));
        wl_display_flush(display);
        return true;
    }
    bool dispatch() {
        if (!display)
            return true;
        while (wl_display_prepare_read(display) != 0)
            if (wl_display_dispatch_pending(display) < 0)
                return false;
        const int flushed = wl_display_flush(display);
        flush_blocked = flushed < 0 && errno == EAGAIN;
        if (flushed < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display);
            return false;
        }
        pollfd descriptor{wl_display_get_fd(display), POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, 0);
        if (ready > 0 && descriptor.revents & (POLLIN | POLLHUP | POLLERR)) {
            if (wl_display_read_events(display) < 0)
                return false;
        } else
            wl_display_cancel_read(display);
        return wl_display_dispatch_pending(display) >= 0;
    }
    std::unique_ptr<Buffer> allocate(int w, int h) {
        const size_t bytes = size_t(w) * h * 4;
        auto path = env("XDG_RUNTIME_DIR") + "/aino-lyrics-XXXXXX";
        const int file = mkstemp(path.data());
        if (file < 0)
            return {};
        unlink(path.c_str());
        if (fcntl(file, F_SETFD, FD_CLOEXEC) < 0 || ftruncate(file, off_t(bytes)) < 0) {
            ::close(file);
            return {};
        }
        auto buffer = std::make_unique<Buffer>();
        buffer->bytes = bytes;
        buffer->width = w;
        buffer->height = h;
        buffer->pixels = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
        if (buffer->pixels == MAP_FAILED) {
            ::close(file);
            return {};
        }
        auto *pool = wl_shm_create_pool(shm, file, int32_t(bytes));
        ::close(file);
        if (!pool)
            return {};
        buffer->object = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        if (!buffer->object)
            return {};
        static const wl_buffer_listener listener{
            [](void *data, wl_buffer *) { static_cast<Buffer *>(data)->busy = false; }};
        wl_buffer_add_listener(buffer->object, &listener, buffer.get());
        return buffer;
    }
    void draw(const LyricPainter &paint) {
        if (!configured || frame || !dirty)
            return;
        if (width <= 0 || height <= 0 || width > 4096 || height > 2048 ||
            size_t(width) * height * scale * scale > 4194304) {
            owner->error = "歌词窗口尺寸超过安全范围";
            return;
        }
        const int w = width * scale, h = height * scale;
        Buffer *target = nullptr;
        for (auto &buffer : buffers)
            if (!buffer->busy) {
                if (buffer->width != w || buffer->height != h) {
                    auto replacement = allocate(w, h);
                    if (!replacement) {
                        owner->error = "无法分配歌词共享内存";
                        return;
                    }
                    buffer = std::move(replacement);
                }
                target = buffer.get();
                break;
            }
        if (!target && buffers.size() < 3) {
            auto buffer = allocate(w, h);
            if (buffer) {
                target = buffer.get();
                buffers.push_back(std::move(buffer));
            } else {
                owner->error = "无法分配歌词共享内存";
                return;
            }
        }
        if (!target)
            return; // compositor 仍持有的缓冲不能覆盖。
        paint(static_cast<uint8_t *>(target->pixels), w, h, float(scale), nullptr);
        auto *channels = static_cast<uint8_t *>(target->pixels);
        for (size_t i = 0; i < target->bytes; ++i)
            channels[i] = uint8_t(unsigned(channels[i]) * std::clamp(settings.opacity, 10, 100) / 100);
        wl_surface_set_buffer_scale(surface, scale);
        if (xdg)
            xdg_surface_set_window_geometry(xdg, 0, 0, width, height);
        wl_surface_attach(surface, target->object, 0, 0);
        wl_surface_damage(surface, 0, 0, width, height);
        frame = wl_surface_frame(surface);
        if (!frame) {
            owner->error = "无法调度歌词绘制";
            return;
        }
        static const wl_callback_listener listener{[](void *data, wl_callback *object, uint32_t) {
            auto *self = static_cast<Impl *>(data);
            wl_callback_destroy(object);
            self->frame = nullptr;
        }};
        wl_callback_add_listener(frame, &listener, this);
        target->busy = true;
        dirty = false;
        wl_surface_commit(surface);
        const int result = wl_display_flush(display);
        flush_blocked = result < 0 && errno == EAGAIN;
    }
};

WaylandLyrics::WaylandLyrics() : impl_(std::make_unique<Impl>(this)) {}
WaylandLyrics::~WaylandLyrics() = default;
void WaylandLyrics::close(bool) {
    impl_->close();
}
bool WaylandLyrics::ready() const {
    return impl_->configured && !impl_->buffers.empty();
}
bool WaylandLyrics::uses_layer_shell() const { return impl_->layer != nullptr; }
bool WaylandLyrics::event_ready() const {
    const int fd = impl_->fd.load();
    if (fd < 0)
        return false;
    const short events = POLLIN | (impl_->flush_blocked.load() ? POLLOUT : 0);
    pollfd descriptor{fd, events, 0};
    return ::poll(&descriptor, 1, 0) > 0 && descriptor.revents & (events | POLLHUP | POLLERR);
}
bool WaylandLyrics::update(LyricOverlaySettings &settings, const std::string &first, const std::string &second,
                           const LyricOverlayProgress &progress, float, const LyricPainter &paint) {
    auto &self = *impl_;
    if (!self.dispatch()) {
        error = "Wayland 歌词连接已断开";
        self.close();
        self.retry_at = ticks() + 5000;
    }
    bool changed = false;
    if (self.closed) {
        settings.enabled = false;
        self.closed = false;
        changed = true;
    }
    if (self.moved) {
        settings.x = self.x;
        settings.y = self.y;
        settings.placement = self.settings.placement;
        self.moved = false;
        changed = true;
    }
    const auto key = [](const auto &value) {
        return std::tie(value.enabled, value.double_line, value.locked, value.translation, value.opacity,
                        value.background, value.font_size, value.width, value.placement, value.x, value.y,
                        value.panel_offset, value.topmost, value.karaoke, value.sung_color, value.unsung_color,
                        value.background_color);
    };
    const bool layout_changed = key(self.settings) != key(settings);
    const auto fraction = [](float value) { return value < 0 ? -1 : int(std::clamp(value, 0.f, 1.f) * 2048); };
    self.dirty |= layout_changed || self.first != first || self.second != second ||
                  fraction(self.progress.first) != fraction(progress.first) ||
                  fraction(self.progress.second) != fraction(progress.second);
    self.settings = settings;
    self.first = first;
    self.second = second;
    self.progress = progress;
    if (!settings.enabled) {
        self.close();
        error.clear();
        return changed;
    }
    if (ticks() < self.retry_at)
        return changed;
    if (!self.create()) {
        self.retry_at = ticks() + 5000;
        return changed;
    }
    if (layout_changed) {
        self.x = settings.x == -32000 ? 32 : std::max(0, settings.x);
        self.y = settings.y == -32000 ? 32 : std::max(0, settings.y);
        self.geometry();
    }
    if (!self.layer && (settings.topmost || settings.placement || settings.locked))
        error = "桌面未提供 layer-shell；歌词使用原生窗口，置顶和停靠由桌面管理，右键可关闭";
    else
        error.clear();
    self.draw(paint);
    return changed;
}
} // namespace platform
