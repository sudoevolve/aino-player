#include "platform.hpp"
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include "../vendor/miniaudio/miniaudio.h"
#include "audio.hpp"
#include "net.hpp"
#include <condition_variable>
#include <algorithm>
#include <cstring>
#include <mutex>
#ifdef AINO_BUNDLED_FFMPEG
#include "aino_ffmpeg_data.hpp"
#endif

namespace audio {
#if defined(AINO_BUNDLED_FFMPEG) && !defined(_WIN32)
extern "C" const unsigned char aino_ffmpeg_start[], aino_ffmpeg_end[];
#endif
std::string decoder_info() {
#ifdef AINO_BUNDLED_FFMPEG
    return std::string("FFmpeg ") + embedded::version + " (embedded, " + std::to_string(embedded::size / 1024) + " KiB, LGPL 2.1+)";
#else
    return "开发构建未内置 FFmpeg；AAC/M4A 使用系统 FFmpeg";
#endif
}
static std::string ffmpeg_path(std::string& error) {
    const auto override = platform::env("AINO_FFMPEG");
    if (!override.empty()) return override;
#ifdef AINO_BUNDLED_FFMPEG
    struct EmbeddedDecoderFile {
        std::mutex mutex;
        std::string path;
        ~EmbeddedDecoderFile() { if (!path.empty()) platform::remove(path); }
    };
    static EmbeddedDecoderFile helper;
    std::lock_guard<std::mutex> lock(helper.mutex);
    if (!helper.path.empty() && platform::file_size(helper.path) == embedded::size) return helper.path;
#ifdef _WIN32
    const auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(32761), MAKEINTRESOURCEW(10));
    const auto loaded = resource ? LoadResource(nullptr, resource) : nullptr;
    const auto* data = loaded ? static_cast<const unsigned char*>(LockResource(loaded)) : nullptr;
    const auto bytes = resource ? size_t(SizeofResource(nullptr, resource)) : 0;
#else
    const auto* data = aino_ffmpeg_start;
    const auto bytes = size_t(aino_ffmpeg_end - aino_ffmpeg_start);
#endif
    if (!data || bytes != embedded::size) { error = "内置解码器资源缺失或大小不符"; return {}; }
    const auto temporary = platform::temp_dir();
    if (temporary.empty()) { error = "无法创建内置解码器临时目录"; return {}; }
    const auto path = platform::join(temporary, std::string("ffmpeg-") + std::string(embedded::digest).substr(0, 16) +
#ifdef _WIN32
        ".exe"
#else
        ""
#endif
    );
    const auto partial = path + ".part";
    auto* file = platform::fopen(partial, "wb");
    if (!file) { error = "无法释放内置解码器"; return {}; }
    const bool written = std::fwrite(data, 1, bytes, file) == bytes;
    const bool closed = std::fclose(file) == 0;
#ifndef _WIN32
    if (::chmod(partial.c_str(), 0700)) { platform::remove(partial); error = "无法设置内置解码器执行权限"; return {}; }
#endif
    if (!written || !closed || !platform::replace_file(partial, path)) { platform::remove(partial); error = "无法写入内置解码器"; return {}; }
    helper.path = path; return path;
#else
    return "ffmpeg";
#endif
}
std::string decoder_license() {
#ifdef AINO_BUNDLED_FFMPEG
    return embedded::notice;
#else
    return "No embedded FFmpeg. See the license of your external decoder.\n";
#endif
}
MediaInfo inspect_mp3(const std::string& path) {
    MediaInfo mi;
    ma_decoder decoder;
#ifdef _WIN32
    const auto rc = ma_decoder_init_file_w(platform::widen(path).c_str(), nullptr, &decoder);
#else
    const auto rc = ma_decoder_init_file(path.c_str(), nullptr, &decoder);
#endif
    if (rc != MA_SUCCESS) return mi;
    mi.sample_rate = int(decoder.outputSampleRate);
    mi.channels = int(decoder.outputChannels);
    ma_uint64 frames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &frames) == MA_SUCCESS && frames && mi.sample_rate) {
        mi.duration_ms = static_cast<long long>(frames * 1000 / mi.sample_rate);
        const auto bytes = platform::file_size(path);
        if (bytes > 0) mi.bitrate_kbps = int(double(bytes) * 8 * mi.sample_rate / double(frames) / 1000);
    }
    mi.valid = mi.sample_rate > 0;
    ma_decoder_uninit(&decoder);
    return mi;
}
bool prepare_file(const std::string& path, std::string& playable, std::string& err, const std::string& conversion_path, const std::function<bool()>& cancelled) {
    err.clear();
    if (cancelled && cancelled()) { err = "已取消"; return false; }
    playable = path;
    auto* f = platform::fopen(path, "rb");
    if (!f) { err = "无法读取音频文件"; return false; }
    unsigned char head[12]{};
    const auto n = std::fread(head, 1, sizeof(head), f);
    std::fclose(f);
    const bool m4a = n >= 8 && !std::memcmp(head + 4, "ftyp", 4);
    const bool adts = n >= 2 && head[0] == 0xff && (head[1] & 0xf6) == 0xf0;
    const bool webm = n >= 4 && head[0] == 0x1a && head[1] == 0x45 && head[2] == 0xdf && head[3] == 0xa3;
    const bool ogg = n >= 4 && !std::memcmp(head, "OggS", 4);
    auto extension = platform::fs::u8path(path).extension().u8string();
    for (auto& c : extension) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    const bool named_aac = extension == ".aac" || extension == ".m4a" || extension == ".mp4";
    if (!m4a && !adts && !webm && !ogg && (!named_aac || inspect_mp3(path).valid)) return true;
    playable = conversion_path.empty() ? path + ".pcm.wav" : conversion_path;
    const auto partial = playable + ".part.wav";
    for (const auto& output : {playable, partial}) {
        std::error_code ec;
        if (platform::fs::equivalent(platform::fs::u8path(path), platform::fs::u8path(output), ec)) {
            err = "解码输出不能覆盖原始音乐文件"; return false;
        }
    }
    const auto helper = ffmpeg_path(err);
    if (helper.empty()) return false;
    static std::timed_mutex conversion;
    std::unique_lock<std::timed_mutex> slot(conversion, std::defer_lock);
    while (!slot.try_lock_for(std::chrono::milliseconds(20))) {
        if (cancelled && cancelled()) { err = "解码已取消"; return false; }
    }
    if (cancelled && cancelled()) { err = "解码已取消"; return false; }
    const int rc = platform::run_process({helper, "-nostdin", "-v", "error", "-y", "-i", path,
        "-vn", "-acodec", "pcm_s16le", partial}, 120000, cancelled);
    if (rc != 0 || platform::file_size(partial) <= 44) {
        platform::remove(partial);
        err = rc == 125 ? "解码已取消" : rc == -1 ? "无法启动音频解码器，请检查临时目录权限或 AINO_FFMPEG 配置" :
              rc == 124 ? "音频解码超时，请检查文件是否损坏" : "FFmpeg 无法解码此音频";
        return false;
    }
    if (!platform::replace_file(partial, playable)) {
        platform::remove(partial); err = "无法保存解码音频，请检查缓存目录权限";
        return false;
    }
    return true;
}
struct Stream::Impl {
    static constexpr uint64_t rate = 48000, capacity = rate * 3, prebuffer = rate / 4;
    struct Source { ma_data_source_base base{}; Impl* owner = nullptr; } source;
    StreamSpec spec;
    std::function<bool()> cancelled;
    platform::PipeProcess process;
    std::vector<int16_t> pcm = std::vector<int16_t>(capacity * 2);
    std::atomic<uint64_t> written{0}, consumed{0};
    std::atomic<bool> stop{false}, decoded{false}, broken{false}, waiting{true};
    std::atomic<bool> input_complete{false}, input_abort{false};
    std::thread input, output, monitor;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::string failure;
    bool source_ready = false;
    bool stopped() const { return stop.load() || (cancelled && cancelled()); }
    void fail(const std::string& message) {
        { std::lock_guard<std::mutex> lock(mutex); if (failure.empty()) failure = message; }
        broken.store(true); cv.notify_all();
    }
    ~Impl() {
        stop.store(true); process.terminate(); cv.notify_all();
        if (input.joinable()) input.join();
        if (output.joinable()) output.join();
        if (monitor.joinable()) monitor.join();
        if (source_ready) ma_data_source_uninit(&source.base);
    }
    static Impl& get(ma_data_source* data) { return *reinterpret_cast<Source*>(data)->owner; }
    static ma_result read(ma_data_source* data, void* frames, ma_uint64 count, ma_uint64* read_count) {
        auto& self = get(data);
        const auto begin = self.consumed.load(std::memory_order_relaxed);
        const bool ended = self.decoded.load(std::memory_order_acquire);
        const auto available = self.written.load(std::memory_order_acquire) - begin;
        // 暂时断流输出静音且不推进曲目位置，不能把网络空隙误报为自然结束。
        if (!ended && (available < count || (self.waiting.load() && available < prebuffer))) {
            if (frames) std::memset(frames, 0, size_t(count) * 4);
            self.waiting.store(true); *read_count = count; return MA_SUCCESS;
        }
        const auto n = std::min<uint64_t>(count, available);
        const auto first = std::min<uint64_t>(n, capacity - begin % capacity);
        if (frames && n) {
            std::memcpy(frames, self.pcm.data() + (begin % capacity) * 2, size_t(first) * 4);
            if (n > first) std::memcpy(static_cast<char*>(frames) + first * 4, self.pcm.data(), size_t(n - first) * 4);
        }
        self.consumed.store(begin + n, std::memory_order_release);
        self.waiting.store(false); *read_count = n;
        return n ? MA_SUCCESS : MA_AT_END;
    }
    static ma_result seek(ma_data_source* data, ma_uint64 frame) {
        return frame == 0 && get(data).consumed.load() == 0 ? MA_SUCCESS : MA_NOT_IMPLEMENTED;
    }
    static ma_result format(ma_data_source*, ma_format* format, ma_uint32* channels, ma_uint32* sample_rate, ma_channel* map, size_t cap) {
        if (format) *format = ma_format_s16;
        if (channels) *channels = 2;
        if (sample_rate) *sample_rate = ma_uint32(rate);
        if (map) ma_channel_map_init_standard(ma_standard_channel_map_default, map, cap, 2);
        return MA_SUCCESS;
    }
    static ma_result cursor(ma_data_source* data, ma_uint64* frame) { *frame = get(data).consumed.load(); return MA_SUCCESS; }
    static ma_result length(ma_data_source*, ma_uint64*) { return MA_NOT_IMPLEMENTED; }
    bool start(std::string& error) {
        const auto helper = ffmpeg_path(error); if (helper.empty()) return false;
        static const ma_data_source_vtable vtable{read, seek, format, cursor, length, nullptr, 0};
        auto config = ma_data_source_config_init(); config.vtable = &vtable; source.owner = this;
        if (ma_data_source_init(&config, &source.base) != MA_SUCCESS) { error = "无法初始化流式音频源"; return false; }
        source_ready = true;
        std::vector<std::string> args{helper, "-nostdin", "-v", "error"};
        const auto seconds = std::to_string(double(std::max(0LL,spec.start_ms)) / 1000.0);
        if (!spec.local_path.empty() && spec.start_ms > 0) { args.push_back("-ss"); args.push_back(seconds); }
        args.insert(args.end(), {"-i", spec.local_path.empty() ? "pipe:0" : spec.local_path});
        // 管道不能随机访问；网络 seek 从头重建并解码丢弃到目标，不伪报定位完成。
        if (spec.local_path.empty() && spec.start_ms > 0) { args.push_back("-ss"); args.push_back(seconds); }
        args.insert(args.end(), {"-vn", "-acodec", "pcm_s16le", "-f", "s16le", "-ar", "48000", "-ac", "2", "pipe:1"});
        if (!process.start(args)) { error = "无法启动流式解码器"; return false; }
        monitor = std::thread([this] {
            while (!decoded.load() && !stop.load()) {
                if (stopped()) { stop.store(true); process.terminate(); cv.notify_all(); return; }
                platform::sleep_ms(20);
            }
        });
        if (spec.local_path.empty()) input = std::thread([this] {
            net::CancelScope scope([this] { return stopped() || input_abort.load(); });
            std::string error;
            bool ok = false;
            try {
                ok = net::download(spec.url, {}, spec.headers, {}, stop, error, 20000,
                    [this](const char* bytes, size_t n) { return !stopped() && !input_abort.load() && process.write(bytes, n); });
            } catch (...) { error = "音频网络传输失败"; }
            input_complete.store(ok, std::memory_order_release);
            process.finish_input();
            if (!ok && !stopped()) fail(error.empty() ? "音频网络连接中断" : error);
        });
        else { input_complete.store(true); process.finish_input(); }
        output = std::thread([this] {
            std::array<char,16384> bytes{}; size_t tail = 0;
            while (!stopped()) {
                const auto got = process.read(bytes.data() + tail, bytes.size() - tail);
                if (!got) break;
                const auto frames = (got + tail) / 4;
                size_t done = 0;
                while (done < frames && !stopped()) {
                    const auto begin = written.load(std::memory_order_relaxed);
                    const auto free = capacity - (begin - consumed.load(std::memory_order_acquire));
                    const auto n = std::min<uint64_t>(frames - done, std::min<uint64_t>(free, capacity - begin % capacity));
                    if (!n) { std::unique_lock<std::mutex> lock(mutex); cv.wait_for(lock,std::chrono::milliseconds(5)); continue; }
                    std::memcpy(pcm.data() + begin % capacity * 2, bytes.data() + done * 4, size_t(n) * 4);
                    written.store(begin + n, std::memory_order_release); done += size_t(n); cv.notify_all();
                }
                tail = (got + tail) % 4;
                if (tail) std::memmove(bytes.data(), bytes.data() + frames * 4, tail);
            }
            if (stopped()) process.terminate();
            const int code = process.wait();
            if (!stopped() && (code != 0 || !written.load())) fail("此音频无法继续流式解码");
            if (!stopped() && !input_complete.load(std::memory_order_acquire)) fail("音频传输未完成，解码已提前结束");
            input_abort.store(true);
            decoded.store(true, std::memory_order_release); cv.notify_all();
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        std::unique_lock<std::mutex> lock(mutex);
        while (written.load() < prebuffer && !decoded.load() && !broken.load() && !stopped()) {
            if (cv.wait_until(lock,deadline) == std::cv_status::timeout) break;
        }
        if (stopped() || broken.load() || written.load() == 0 || (written.load() < prebuffer && !decoded.load())) {
            error = stopped() ? "流式播放已取消" : failure.empty() ? "流式预缓冲失败，尝试完整文件" : failure;
            lock.unlock(); stop.store(true); process.terminate(); cv.notify_all(); return false;
        }
        return true;
    }
};
Stream::Stream() : impl_(std::make_unique<Impl>()) {}
Stream::~Stream() = default;
std::shared_ptr<Stream> Stream::open(StreamSpec spec, std::function<bool()> cancelled, std::string& error) {
    error.clear();
    auto stream = std::shared_ptr<Stream>(new Stream);
    stream->impl_->spec = std::move(spec); stream->impl_->cancelled = std::move(cancelled);
    try { if (!stream->impl_->start(error)) return {}; }
    catch (...) { error = "无法创建流式音频任务"; return {}; }
    return stream;
}
void Stream::cancel() { impl_->stop.store(true); impl_->process.terminate(); impl_->cv.notify_all(); }
bool Stream::failed() const { return impl_->broken.load(); }
bool Stream::buffering() const { return impl_->waiting.load() && !impl_->decoded.load(); }
std::string Stream::error() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->failure; }

struct Player::Impl {
    ma_engine engine{};
    ma_sound sound{};
    bool engine_ready = false;
    bool sound_ready = false;
    int volume = 100;
    std::string error;
    std::shared_ptr<Stream> stream;
    ~Impl() { if (sound_ready) ma_sound_uninit(&sound); if (engine_ready) ma_engine_uninit(&engine); }
};
Player::Player() : impl_(std::make_unique<Impl>()) {}
Player::~Player() = default;
bool Player::open(const std::string& path) {
    close();
    auto& p = *impl_;
    p.error.clear();
    if (!p.engine_ready) {
        auto config = ma_engine_config_init();
        const auto rc = ma_engine_init(&config, &p.engine);
        if (rc != MA_SUCCESS) { p.error = std::string("音频设备初始化失败：") + ma_result_description(rc); return false; }
        p.engine_ready = true;
    }
#ifdef _WIN32
    const auto rc = ma_sound_init_from_file_w(&p.engine, platform::widen(path).c_str(), MA_SOUND_FLAG_STREAM, nullptr, nullptr, &p.sound);
#else
    const auto rc = ma_sound_init_from_file(&p.engine, path.c_str(), MA_SOUND_FLAG_STREAM, nullptr, nullptr, &p.sound);
#endif
    if (rc != MA_SUCCESS) { p.error = std::string("音频解码失败：") + ma_result_description(rc); return false; }
    p.sound_ready = true;
    ma_sound_set_volume(&p.sound, float(p.volume) / 100);
    return true;
}
bool Player::open_stream(std::shared_ptr<Stream> stream) {
    close(); auto& p = *impl_; p.error.clear();
    if (!stream) { p.error = "流式音频为空"; return false; }
    if (!p.engine_ready) {
        auto config = ma_engine_config_init();
        const auto rc = ma_engine_init(&config, &p.engine);
        if (rc != MA_SUCCESS) { p.error = std::string("音频设备初始化失败：") + ma_result_description(rc); return false; }
        p.engine_ready = true;
    }
    const auto rc = ma_sound_init_from_data_source(&p.engine, &stream->impl_->source.base, 0, nullptr, &p.sound);
    if (rc != MA_SUCCESS) { p.error = std::string("流式播放初始化失败：") + ma_result_description(rc); return false; }
    p.stream = std::move(stream); p.sound_ready = true;
    ma_sound_set_volume(&p.sound,float(p.volume)/100); return true;
}
void Player::close() {
    if (impl_->sound_ready) { ma_sound_uninit(&impl_->sound); impl_->sound_ready = false; }
    if (impl_->stream) { impl_->stream->cancel(); impl_->stream.reset(); }
}
bool Player::play() {
    if (!impl_->sound_ready) return false;
    const auto rc = ma_sound_start(&impl_->sound);
    if (rc != MA_SUCCESS) impl_->error = std::string("启动播放失败：") + ma_result_description(rc);
    return rc == MA_SUCCESS;
}
void Player::pause() { if (impl_->sound_ready) ma_sound_stop(&impl_->sound); }
void Player::resume() { play(); }
bool Player::is_playing() const { return impl_->sound_ready && ma_sound_is_playing(&impl_->sound); }
bool Player::is_ended() const { return impl_->sound_ready && !stream_failed() && ma_sound_at_end(&impl_->sound); }
bool Player::stream_failed() const { return impl_->stream && impl_->stream->failed(); }
bool Player::buffering() const { return impl_->stream && impl_->stream->buffering(); }
long long Player::position_ms() const {
    if (impl_->stream) { const auto& stream = *impl_->stream->impl_; return stream.spec.start_ms + static_cast<long long>(stream.consumed.load() * 1000 / Stream::Impl::rate); }
    float seconds = 0;
    if (impl_->sound_ready) ma_sound_get_cursor_in_seconds(&impl_->sound, &seconds);
    return static_cast<long long>(seconds * 1000.0f);
}
long long Player::duration_ms() const {
    if (impl_->stream) return impl_->stream->impl_->spec.duration_ms;
    float seconds = 0;
    if (impl_->sound_ready) ma_sound_get_length_in_seconds(&impl_->sound, &seconds);
    return static_cast<long long>(seconds * 1000.0f);
}
void Player::seek_ms(long long ms) {
    if (!impl_->sound_ready || impl_->stream) return;
    ma_uint32 rate = 0;
    if (ma_sound_get_data_format(&impl_->sound, nullptr, nullptr, &rate, nullptr, 0) != MA_SUCCESS) return;
    const auto end = duration_ms();
    ms = std::max(0LL, end > 0 ? std::min(ms, end) : ms);
    ma_sound_seek_to_pcm_frame(&impl_->sound, ma_uint64(ms) * rate / 1000);
}
void Player::set_volume(int volume) {
    impl_->volume = std::clamp(volume, 0, 100);
    if (impl_->sound_ready) ma_sound_set_volume(&impl_->sound, float(impl_->volume) / 100);
}
const std::string& Player::error() const {
    if (impl_->stream && impl_->stream->failed()) impl_->error = impl_->stream->error();
    return impl_->error;
}
} // namespace audio
