// audio.hpp —— 播放引擎（Media Foundation / IMFMediaEngine）
//
// 为什么从 MCI 换过来：QQ 音乐取流返回的是 **m4a**（MP4 容器，文件头 ftypmp42），
// 而 MCI 对 m4a 一律报 277「初始化 MCI 时发生问题」——实测三种 open 方式（自动 /
// mpegvideo / 改扩展名）全部失败。MCI 只认 mp3/wav，QQ 歌单等于全都放不了。
//
// IMFMediaEngine 是 Windows 自带的媒体播放封装（Win8+），mp3 / m4a / flac / wav
// 都能播，而且自带 seek、音量、时长、结束判定，比 MCI 少一堆字符串拼接。
#pragma once

#include <windows.h>

#include <audiosessiontypes.h>   // AudioCategory_Media
#include <mfapi.h>
#include <mfmediaengine.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")   // SysAllocString

namespace audio {

using Microsoft::WRL::ComPtr;


inline std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

// MediaEngine 要求属性表里必须带事件回调（不给就 MF_E_ATTRIBUTENOTFOUND）。
// 我们只在结束时需要知道一下，所以回调本身什么都不做 —— 播放状态靠 IsPaused/IsEnded 轮询。
class EngineNotify : public IMFMediaEngineNotify {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IMFMediaEngineNotify) || riid == IID_IUnknown) {
            *ppv = static_cast<IMFMediaEngineNotify*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP EventNotify(DWORD, DWORD_PTR, DWORD) override { return S_OK; }
};

inline EngineNotify& engine_notify() {
    static EngineNotify n;   // 进程级单例，故意不释放
    return n;
}

struct MediaInfo {
    bool valid = false;
    int bitrate_kbps = 0;
    int sample_rate = 0;
    int channels = 0;
};

// 只解析 mp3 帧头（用于报"实测码率"）。m4a 走文件大小/时长估算，见 finish_play。
inline MediaInfo inspect_mp3(const std::string& path) {
    MediaInfo mi;
    std::FILE* f = _wfopen(utf8_to_wide(path).c_str(), L"rb");
    if (!f) return mi;

    unsigned char hdr[4];
    if (std::fread(hdr, 1, 4, f) != 4 || hdr[0] != 0xFF || (hdr[1] & 0xE0) != 0xE0) {
        std::fclose(f);
        return mi;
    }
    static const int kBitrateV1L3[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const int kBitrateV2L3[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const int kRateV1[] = {44100, 48000, 32000, 0};
    static const int kRateV2[] = {22050, 24000, 16000, 0};
    static const int kRateV25[] = {11025, 12000, 8000, 0};

    const int ver = (hdr[1] >> 3) & 0x3;   // 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5
    const int br_idx = (hdr[2] >> 4) & 0xF;
    const int sr_idx = (hdr[2] >> 2) & 0x3;
    const int mode = (hdr[3] >> 6) & 0x3;

    mi.bitrate_kbps = (ver == 3 ? kBitrateV1L3 : kBitrateV2L3)[br_idx];
    mi.sample_rate = (ver == 3 ? kRateV1 : (ver == 2 ? kRateV2 : kRateV25))[sr_idx];
    mi.channels = (mode == 3) ? 1 : 2;
    mi.valid = mi.bitrate_kbps > 0 && mi.sample_rate > 0;
    std::fclose(f);
    return mi;
}

// 绝对路径 → file:/// URL（MediaEngine 只认 URL）。
// 必须做百分号编码：中文用户名（C:\Users\张三\...）直接塞进 URL 会加载失败。
inline std::wstring to_file_url(const std::string& path) {
    std::wstring out = L"file:///";
    for (unsigned char c : path) {
        if (c == '\\') {
            out += L'/';
        } else if (c == ' ' || c == '#' || c == '?' || c == '%' || c == '&' || c >= 0x80) {
            // UTF-8 字节逐个编码，中文会变成三个 %XX
            wchar_t buf[8];
            std::swprintf(buf, 8, L"%%%02X", c);
            out += buf;
        } else {
            out += wchar_t(c);
        }
    }
    return out;
}

class Player {
public:
    ~Player() { close(); }

    bool open(const std::string& path) {
        close();
        if (path.empty()) return false;

        if (!mf_started_) {
            if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET))) return false;
            mf_started_ = true;
        }

        ComPtr<IMFMediaEngineClassFactory> factory;
        const HRESULT fhr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER,
                                             IID_PPV_ARGS(&factory));
        if (FAILED(fhr)) return false;

        ComPtr<IMFAttributes> attr;
        if (FAILED(MFCreateAttributes(&attr, 2))) return false;
        attr->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, &engine_notify());
        attr->SetUINT32(MF_MEDIA_ENGINE_AUDIO_CATEGORY, AudioCategory_Media);

        const HRESULT chr = factory->CreateInstance(0, attr.Get(), &engine_);
        if (FAILED(chr) || !engine_) return false;

        const std::wstring url = to_file_url(path);
        BSTR b = SysAllocString(url.c_str());
        const HRESULT hr = engine_->SetSource(b);
        SysFreeString(b);
        if (FAILED(hr)) {
            close();
            return false;
        }

        engine_->SetVolume(volume_);
        // 不在这里等元数据：SetSource 之后 readyState 仍是 HAVE_NOTHING，
        // 要等 Play() 才真正开始加载。在这里 Sleep 等只会把主线程卡住 3 秒，
        // 而且一定等不到，最后还误判成"打不开"。总时长先用调用方给的值兜着，
        // 播起来之后每帧会拿到真实值。
        return true;
    }

    void close() {
        if (engine_) {
            engine_->Pause();
            engine_->SetSource(nullptr);
            engine_.Reset();
        }
    }

    bool play() {
        if (!engine_) return false;
        const HRESULT hr = engine_->Play();
        return SUCCEEDED(hr);
    }

    void pause() {
        if (engine_) engine_->Pause();
    }

    void resume() {
        if (engine_) engine_->Play();
    }

    bool is_playing() const {
        if (!engine_) return false;
        return !engine_->IsPaused() && !engine_->IsEnded();
    }

    bool is_ended() const {
        if (!engine_) return false;
        return engine_->IsEnded() != FALSE;
    }

    long long position_ms() const {
        if (!engine_) return 0;
        return static_cast<long long>(engine_->GetCurrentTime() * 1000.0 + 0.5);
    }

    long long duration_ms() const {
        if (!engine_) return 0;
        return static_cast<long long>(engine_->GetDuration() * 1000.0 + 0.5);
    }

    void seek_ms(long long ms) {
        if (!engine_) return;
        engine_->SetCurrentTime(double(ms) / 1000.0);
    }

    // 0..100
    void set_volume(int v) {
        volume_ = float(std::min(100, std::max(0, v))) / 100.0f;
        if (engine_) engine_->SetVolume(volume_);
    }

private:
    ComPtr<IMFMediaEngine> engine_;
    float volume_ = 1.0f;
    bool mf_started_ = false;
};

}  // namespace audio
