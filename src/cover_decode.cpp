#ifndef _WIN32
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_MAX_DIMENSIONS 8192
#include "../vendor/stb/stb_image.h"
#include "cover.hpp"
#include <algorithm>
#include <climits>
namespace cover { namespace detail {
bool decode(const std::vector<uint8_t>& bytes, int max_side, Image& out) {
    int sw = 0, sh = 0, channels = 0;
    if (max_side <= 0 || bytes.empty() || bytes.size() > size_t(INT_MAX)) return false;
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
