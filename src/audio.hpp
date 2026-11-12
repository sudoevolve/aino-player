// 播放由 miniaudio 单头库完成；网络与 AAC 转码在后台执行。
#pragma once
#include <memory>
#include <string>
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
bool prepare_file(const std::string& path, std::string& playable, std::string& err, const std::string& conversion_path = {});
class Player {
public:
    Player();
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;
    bool open(const std::string& path);
    void close();
    bool play();
    void pause();
    void resume();
    bool is_playing() const;
    bool is_ended() const;
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
