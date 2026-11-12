#ifndef _WIN32
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_MAX_DIMENSIONS 8192
#include "../vendor/stb/stb_image.h"
#include "cover.hpp"
#include <algorithm>
#include <climits>
#ifdef __linux__
#include <dlfcn.h>
#endif
namespace cover { namespace detail {
#ifdef __linux__
// GdkPixbuf 在 size-prepared 阶段指定目标大小；JPEG 可在解码时降采样，
// 不再先用 stb 分配完整封面再缩小。只加载图像组件，不初始化 GTK。
struct LinuxCoverDecoder {
    void* lib = nullptr;
    void* (*create)() = nullptr;
    unsigned long (*connect)(void*, const char*, void(*)(), void*, void(*)(void*, void*), int) = nullptr;
    void (*set_size)(void*, int, int) = nullptr;
    int (*write)(void*, const unsigned char*, size_t, void**) = nullptr;
    int (*close)(void*, void**) = nullptr;
    void* (*image)(void*) = nullptr;
    int (*width)(const void*) = nullptr;
    int (*height)(const void*) = nullptr;
    int (*stride)(const void*) = nullptr;
    int (*channels)(const void*) = nullptr;
    int (*has_alpha)(const void*) = nullptr;
    unsigned char* (*pixels)(const void*) = nullptr;
    void (*unref)(void*) = nullptr;
    LinuxCoverDecoder() {
        lib = dlopen("libgdk_pixbuf-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
        if (!lib) return;
        create = reinterpret_cast<decltype(create)>(dlsym(lib, "gdk_pixbuf_loader_new"));
        connect = reinterpret_cast<decltype(connect)>(dlsym(lib, "g_signal_connect_data"));
        set_size = reinterpret_cast<decltype(set_size)>(dlsym(lib, "gdk_pixbuf_loader_set_size"));
        write = reinterpret_cast<decltype(write)>(dlsym(lib, "gdk_pixbuf_loader_write"));
        close = reinterpret_cast<decltype(close)>(dlsym(lib, "gdk_pixbuf_loader_close"));
        image = reinterpret_cast<decltype(image)>(dlsym(lib, "gdk_pixbuf_loader_get_pixbuf"));
        width = reinterpret_cast<decltype(width)>(dlsym(lib, "gdk_pixbuf_get_width"));
        height = reinterpret_cast<decltype(height)>(dlsym(lib, "gdk_pixbuf_get_height"));
        stride = reinterpret_cast<decltype(stride)>(dlsym(lib, "gdk_pixbuf_get_rowstride"));
        channels = reinterpret_cast<decltype(channels)>(dlsym(lib, "gdk_pixbuf_get_n_channels"));
        has_alpha = reinterpret_cast<decltype(has_alpha)>(dlsym(lib, "gdk_pixbuf_get_has_alpha"));
        pixels = reinterpret_cast<decltype(pixels)>(dlsym(lib, "gdk_pixbuf_get_pixels"));
        unref = reinterpret_cast<decltype(unref)>(dlsym(lib, "g_object_unref"));
        if (!create || !connect || !set_size || !write || !close || !image || !width ||
            !height || !stride || !channels || !has_alpha || !pixels || !unref) {
            dlclose(lib); lib = nullptr;
        }
    }
    struct SizeRequest { const LinuxCoverDecoder* api; int max_side; };
    static void prepared(void* loader, int w, int h, void* data) {
        const auto& request = *static_cast<SizeRequest*>(data);
        if (w <= 0 || h <= 0) return;
        const double scale = std::min(1.0, double(request.max_side) / std::max(w, h));
        request.api->set_size(loader, std::max(1, int(w * scale)), std::max(1, int(h * scale)));
    }
    bool decode(const std::vector<uint8_t>& bytes, int max_side, Image& out) const {
        void* loader = create();
        if (!loader) return false;
        SizeRequest request{this, max_side};
        connect(loader, "size-prepared", reinterpret_cast<void(*)()>(prepared), &request, nullptr, 0);
        const bool written = write(loader, bytes.data(), bytes.size(), nullptr) != 0;
        const bool closed = close(loader, nullptr) != 0;
        void* pixbuf = written && closed ? image(loader) : nullptr;
        bool ok = false;
        if (pixbuf) {
            const int w = width(pixbuf), h = height(pixbuf), row = stride(pixbuf), count = channels(pixbuf);
            const auto* source = pixels(pixbuf);
            if (source && w > 0 && h > 0 && w <= max_side && h <= max_side &&
                (count == 3 || count == 4) && row >= w * count) {
                const bool alpha = has_alpha(pixbuf) && count == 4;
                out.w = w; out.h = h; out.rgba.resize(size_t(w) * h * 4);
                for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                    const auto* pixel = source + size_t(y) * row + x * count;
                    auto* target = out.rgba.data() + (size_t(y) * w + x) * 4;
                    target[0] = pixel[0]; target[1] = pixel[1]; target[2] = pixel[2];
                    target[3] = alpha ? pixel[3] : 255;
                }
                ok = true;
            }
        }
        unref(loader);
        return ok;
    }
};
#endif
bool decode(const std::vector<uint8_t>& bytes, int max_side, Image& out) {
    int sw = 0, sh = 0, channels = 0;
    if (max_side <= 0 || bytes.empty() || bytes.size() > size_t(INT_MAX)) return false;
#ifdef __linux__
    // 保留原有 JPEG/PNG 和尺寸上限，原生解码器不扩大可接受的输入范围。
    if (!stbi_info_from_memory(bytes.data(), int(bytes.size()), &sw, &sh, &channels) ||
        sw <= 0 || sh <= 0 || sw > STBI_MAX_DIMENSIONS || sh > STBI_MAX_DIMENSIONS) return false;
    static const LinuxCoverDecoder native;
    if (native.lib && native.decode(bytes, max_side, out)) return true;
#endif
    auto* pixels = stbi_load_from_memory(bytes.data(), int(bytes.size()), &sw, &sh, &channels, 4);
    if (!pixels || sw <= 0 || sh <= 0) { stbi_image_free(pixels); return false; }
    const double scale = std::min(1.0, double(max_side) / std::max(sw, sh));
    out.w = std::max(1, int(sw * scale)); out.h = std::max(1, int(sh * scale));
    out.rgba.resize(size_t(out.w) * out.h * 4);
    for (int y = 0; y < out.h; ++y) for (int x = 0; x < out.w; ++x) {
        const double sx = (x + 0.5) * sw / out.w - 0.5, sy = (y + 0.5) * sh / out.h - 0.5;
        const int x0 = std::max(0, int(sx)), y0 = std::max(0, int(sy));
        const int x1 = std::min(sw - 1, x0 + 1), y1 = std::min(sh - 1, y0 + 1);
        const double fx = std::max(0.0, sx - x0), fy = std::max(0.0, sy - y0);
        for (int c = 0; c < 4; ++c) out.rgba[(size_t(y) * out.w + x) * 4 + c] = uint8_t(
            (1-fy) * ((1-fx) * pixels[(size_t(y0)*sw+x0)*4+c] + fx * pixels[(size_t(y0)*sw+x1)*4+c]) +
            fy * ((1-fx) * pixels[(size_t(y1)*sw+x0)*4+c] + fx * pixels[(size_t(y1)*sw+x1)*4+c]));
    }
    stbi_image_free(pixels); return true;
}
} }
#endif
