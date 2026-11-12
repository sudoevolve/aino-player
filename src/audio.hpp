// 播放由 miniaudio 单头库完成；网络与 AAC 转码在后台执行。
#pragma once
#include <memory>
#include <functional>
#include <string>
#include <vector>
namespace audio {
struct MediaInfo {
    bool valid = false;
    int bitrate_kbps = 0;
    int sample_rate = 0;
    int channels = 0;
    long long duration_ms = 0;
};
MediaInfo inspect_mp3(const std::string& path);
/** @brief 返回内置解码器的版本、体积和许可概况，未内置时明确说明。 */
std::string decoder_info();
/** @brief 返回随内置解码器提供的完整许可、来源校验值及构建信息。 */
std::string decoder_license();
bool prepare_file(const std::string& path, std::string& playable, std::string& err, const std::string& conversion_path = {}, const std::function<bool()>& cancelled = {});
struct StreamSpec {
    std::string url, local_path;
    std::vector<std::string> headers;
    long long duration_ms = 0, start_ms = 0;
};
/**
 * @brief 后台网络/解码管线，PCM 容量固定为三秒，音频回调只读取原子环形缓冲。
 *
 * open 在工作线程预缓冲；不能流式读取的容器返回失败，调用方可退回完整文件。
 * cancel 立即发出停止信号；析构回收所有线程，因此须在音频设备解绑后销毁。
 */
class Stream {
public:
    static std::shared_ptr<Stream> open(StreamSpec spec, std::function<bool()> cancelled, std::string& error);
    ~Stream();
    void cancel();
    bool failed() const;
    bool buffering() const;
    std::string error() const;
private:
    Stream();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class Player;
};
class Player {
public:
    Player();
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;
    bool open(const std::string& path);
    bool open_stream(std::shared_ptr<Stream> stream);
    void close();
    bool play();
    void pause();
    void resume();
    bool is_playing() const;
    bool is_ended() const;
    bool stream_failed() const;
    bool buffering() const;
    long long position_ms() const;
    long long duration_ms() const;
    void seek_ms(long long ms);
    void set_volume(int volume);
    const std::string& error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace audio
