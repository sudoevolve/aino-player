// cover.hpp —— 封面图：下载 + WIC 解码 + 内存缓存
//
// aino 本身不加载任何图片（它连解码器都没有），所以这里补上播放器需要的那一小块：
//   * 用 Windows 自带的 WIC 解 JPEG/PNG（不加第三方库，windowscodecs.lib 是系统组件）
//   * 一次性缩放到 192px 长边，之后由 aino::Ui::draw_bitmap 双线性缩放显示
//   * 下载与解码都在后台线程，UI 线程只做 "就绪了吗" 的查询
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <wincodec.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net.hpp"

namespace cover {

struct Image {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> rgba;  // 4 字节/像素，RGBA 顺序（WIC 直接给这个格式）
};

namespace detail {

// COM 按线程初始化：解码发生在哪条线程，就得在那条线程上 CoInitializeEx
inline void com_init_once() {
    static thread_local bool inited = false;
    if (inited) return;
    inited = true;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
}

// 解码并在解码阶段就缩放到长边 ≤ max_side：一次到位，避免每帧缩放开销
inline bool decode(const std::vector<uint8_t>& bytes, int max_side, Image& out) {
    if (bytes.size() < 16) return false;
    com_init_once();

    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                               __uuidof(IWICImagingFactory), reinterpret_cast<void**>(&factory))))
        return false;

    bool ok = false;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* conv = nullptr;

    do {
        if (FAILED(factory->CreateStream(&stream))) break;
        if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),
                                               static_cast<DWORD>(bytes.size()))))
            break;
        if (FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand,
                                                    &decoder)))
            break;
        if (FAILED(decoder->GetFrame(0, &frame))) break;

        UINT sw = 0, sh = 0;
        if (FAILED(frame->GetSize(&sw, &sh)) || sw == 0 || sh == 0) break;
        const double k = double(max_side) / double(sw > sh ? sw : sh);
        const UINT dw = UINT(sw * k + 0.5), dh = UINT(sh * k + 0.5);

        if (FAILED(factory->CreateBitmapScaler(&scaler))) break;
        if (FAILED(scaler->Initialize(frame, dw, dh, WICBitmapInterpolationModeFant))) break;
        if (FAILED(factory->CreateFormatConverter(&conv))) break;
        if (FAILED(conv->Initialize(scaler, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                    nullptr, 0.0, WICBitmapPaletteTypeCustom)))
            break;

        out.w = int(dw);
        out.h = int(dh);
        out.rgba.resize(size_t(dw) * size_t(dh) * 4u);
        if (FAILED(conv->CopyPixels(nullptr, dw * 4u, static_cast<UINT>(out.rgba.size()),
                                    out.rgba.data())))
            break;
        ok = true;
    } while (false);

    if (conv) conv->Release();
    if (scaler) scaler->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    factory->Release();
    return ok;
}

}  // namespace detail

// 一张封面对应一条 http 地址；同一个地址只下载一次
class Store {
public:
    void set_notifier(std::function<void()> fn) { notify_ = std::move(fn); }

    // 就绪返回图；没就绪返回空（并确保后台已经在取）。UI 线程每帧调它。
    std::shared_ptr<const Image> get(const std::string& url) {
        if (url.empty()) return nullptr;
        {
            std::lock_guard<std::mutex> lk(m_);
            auto it = map_.find(url);
            if (it != map_.end()) {
                if (it->second.state == 2) return it->second.img;
                return nullptr;
            }
            map_[url].state = 1;  // 下载中
        }
        std::thread([this, url] { load(url); }).detach();
        return nullptr;
    }

private:
    struct Entry {
        int state = 0;  // 0 未知 / 1 下载中 / 2 就绪 / 3 失败
        std::shared_ptr<const Image> img;
    };

    void load(const std::string& url) {
        auto img = std::make_shared<Image>();
        bool ok = false;
        const net::Response r = net::get(url, {"User-Agent: Mozilla/5.0", "Referer: https://music.163.com/"},
                                         12000);
        if (r.status == 200 && r.body.size() > 256) {
            std::vector<uint8_t> bytes(r.body.begin(), r.body.end());
            ok = detail::decode(bytes, 192, *img);
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            Entry& e = map_[url];
            e.state = ok ? 2 : 3;
            if (ok) e.img = img;
            // 简单的容量控制：超过 160 张就把已经就绪的前一半丢掉
            if (map_.size() > 160) {
                int budget = int(map_.size() / 2);
                for (auto it = map_.begin(); it != map_.end() && budget > 0;) {
                    if (it->second.state == 2) {
                        it = map_.erase(it);
                        --budget;
                    } else {
                        ++it;
                    }
                }
            }
        }
        if (notify_) notify_();
    }

    std::mutex m_;
    std::map<std::string, Entry> map_;
    std::function<void()> notify_;
};

}  // namespace cover
