// 仅清理播放器生成的音频文件；不递归删除目录，不跟随链接，不触碰其他运行实例。
#pragma once
#include "platform.hpp"
#include <algorithm>
#include <functional>
#include <limits>
namespace cache {
struct Cleanup {
    size_t files = 0, kept = 0, errors = 0;
    uintmax_t bytes = 0;
};
inline bool audio_file(const std::string& name) {
    if (name.rfind("track-", 0) != 0) return false;
    const auto dot = name.find('.', 6);
    if (dot == std::string::npos || dot == 6 || name.substr(6, dot-6).find_first_not_of("0123456789") != std::string::npos) return false;
    const auto suffix = name.substr(dot);
    if (suffix == ".pcm.wav" || suffix == ".pcm.wav.part.wav") return true;
    for (const char* ext : {".mp3", ".flac", ".wav", ".m4a", ".aac", ".ogg", ".opus", ".webm"}) {
        if (suffix == ext || suffix == std::string(ext) + ".pcm.wav" || suffix == std::string(ext) + ".pcm.wav.part.wav") return true;
    }
    return false;
}
inline bool codec_file(const std::string& name) {
    if (name.rfind("ffmpeg-", 0) != 0 || name.size() < 23 ||
        name.substr(7, 16).find_first_not_of("0123456789abcdef") != std::string::npos) return false;
    const auto suffix = name.substr(23);
    return suffix.empty() || suffix == ".exe" || suffix == ".part" || suffix == ".exe.part";
}
/** @brief 清理指定缓存根目录；调用方须锁住本进程的写入后才允许清理当前目录。 */
inline Cleanup clear_audio(const std::string& root, uint32_t current_pid, const std::string& keep_prefix,
                           bool clear_current, const std::function<bool(uint32_t)>& alive = platform::process_running) {
    Cleanup result;
    std::error_code ec;
    const auto base = platform::fs::u8path(root);
    const auto status = platform::fs::symlink_status(base, ec);
    if (ec == std::errc::no_such_file_or_directory) return result;
    if (ec || !platform::fs::is_directory(status) || platform::fs::is_symlink(status)) { ++result.errors; return result; }
    auto clean_file = [&](const platform::fs::directory_entry& entry, const std::string& prefix, bool stale_codec = false) {
        const auto name = entry.path().filename().u8string();
        if (!audio_file(name) && !(stale_codec && codec_file(name))) return;
        std::error_code error;
        const auto type = entry.symlink_status(error);
        if (error || !platform::fs::is_regular_file(type)) { if (error) ++result.errors; return; }
        if (!prefix.empty() && name.rfind(prefix, 0) == 0) { ++result.kept; return; }
        const auto bytes = entry.file_size(error);
        if (error) { ++result.errors; return; }
        if (platform::fs::remove(entry.path(), error)) { ++result.files; result.bytes += bytes; }
        else if (error) ++result.errors;
    };
    for (platform::fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code error;
        const auto type = it->symlink_status(error);
        if (error) { ++result.errors; continue; }
        if (platform::fs::is_symlink(type)) continue;
        if (platform::fs::is_regular_file(type)) { clean_file(*it, {}); continue; }
        if (!platform::fs::is_directory(type)) continue;
        const auto name = it->path().filename().u8string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos || name.size() > 10) continue;
        uint64_t value = 0;
        for (char c : name) value = value * 10 + uint64_t(c - '0');
        if (!value || value > uint64_t(std::numeric_limits<int32_t>::max())) continue;
        const auto pid = uint32_t(value);
        const bool current = pid == current_pid;
        if ((current && !clear_current) || (!current && alive(pid))) { ++result.kept; continue; }
        for (platform::fs::directory_iterator child(it->path(), error), child_end; !error && child != child_end; child.increment(error))
            clean_file(*child, current ? keep_prefix : std::string(), !current);
        if (error) ++result.errors;
        // 仅移除已空的旧进程目录；未知文件或嵌套目录一律保留。
        if (!current && platform::fs::is_empty(it->path(), error) && !error) platform::fs::remove(it->path(), error);
    }
    if (ec) ++result.errors;
    return result;
}
} // namespace cache
