#include "platform.hpp"
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include "../vendor/miniaudio/miniaudio.h"
#include "audio.hpp"
#include <algorithm>
#include <cstring>

namespace audio {
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
bool prepare_file(const std::string& path, std::string& playable, std::string& err, const std::string& conversion_path) {
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
    if (!m4a && !adts && !webm && !ogg) return true;
    playable = conversion_path.empty() ? path + ".pcm.wav" : conversion_path;
    const auto partial = playable + ".part.wav";
    const int rc = platform::run_process({"ffmpeg", "-nostdin", "-v", "error", "-y", "-i", path,
        "-vn", "-acodec", "pcm_s16le", partial});
    if (rc != 0 || platform::file_size(partial) <= 44 || !platform::replace_file(partial, playable)) {
        platform::remove(partial);
        err = rc == -1 ? "此音频格式需要 FFmpeg，请安装并加入 PATH" : "FFmpeg 无法解码此音频";
        return false;
    }
    return true;
}
struct Player::Impl {
    ma_engine engine{};
    ma_sound sound{};
    bool engine_ready = false;
    bool sound_ready = false;
    int volume = 100;
    std::string error;
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
void Player::close() { if (impl_->sound_ready) { ma_sound_uninit(&impl_->sound); impl_->sound_ready = false; } }
bool Player::play() {
    if (!impl_->sound_ready) return false;
    const auto rc = ma_sound_start(&impl_->sound);
    if (rc != MA_SUCCESS) impl_->error = std::string("启动播放失败：") + ma_result_description(rc);
    return rc == MA_SUCCESS;
}
void Player::pause() { if (impl_->sound_ready) ma_sound_stop(&impl_->sound); }
void Player::resume() { play(); }
bool Player::is_playing() const { return impl_->sound_ready && ma_sound_is_playing(&impl_->sound); }
bool Player::is_ended() const { return impl_->sound_ready && ma_sound_at_end(&impl_->sound); }
long long Player::position_ms() const {
    float seconds = 0;
    if (impl_->sound_ready) ma_sound_get_cursor_in_seconds(&impl_->sound, &seconds);
    return static_cast<long long>(seconds * 1000.0f);
}
long long Player::duration_ms() const {
    float seconds = 0;
    if (impl_->sound_ready) ma_sound_get_length_in_seconds(&impl_->sound, &seconds);
    return static_cast<long long>(seconds * 1000.0f);
}
void Player::seek_ms(long long ms) {
    if (!impl_->sound_ready) return;
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
const std::string& Player::error() const { return impl_->error; }
} // namespace audio
