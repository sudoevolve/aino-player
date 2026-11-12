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
};
MediaInfo inspect_mp3(const std::string& path);
bool prepare_file(const std::string& path, std::string& playable, std::string& err);
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
