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
#include <random>
#include <cmath>
#include <stdexcept>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/eventfd.h>
#include <poll.h>
#endif
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
// FFmpeg 只连接随机路径的 loopback HTTP。TLS、账号头和远端 Range 校验仍归播放器，
// 每个连接最多缓存 512 KiB，不猜测时间/码率对应的字节位置，由容器索引准确定位。
class RangeBridge {
#ifdef _WIN32
    using Socket = SOCKET;
    static constexpr Socket invalid = INVALID_SOCKET;
    bool sockets_ready = false;
#else
    using Socket = int;
    static constexpr Socket invalid = -1;
#endif
    struct Client { Socket socket = invalid; std::thread thread; std::atomic<bool> done{false}; };
    Socket listener = invalid;
    std::thread acceptor;
    std::vector<std::unique_ptr<Client>> clients; // 只由 acceptor 修改，stop 在 join 后读取。
    std::atomic<bool> stopping{false}, bad{false};
    std::function<bool()> cancelled;
    StreamSpec spec;
    long long total = 0;
    std::string route, address;
    static void close_socket(Socket socket) {
#ifdef _WIN32
        closesocket(socket);
#else
        ::close(socket);
#endif
    }
    bool stopped() const { return stopping.load() || (cancelled && cancelled()); }
    static void timeout(Socket socket) {
#ifdef _WIN32
        DWORD ms = 1000; setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms); setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
#else
        timeval value{1, 0}; setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof value); setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof value);
#endif
    }
    static bool nonblocking(Socket socket) {
#ifdef _WIN32
        u_long enabled = 1; return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
        const auto flags = fcntl(socket, F_GETFL, 0);
        return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    }
    bool send_bytes(Socket socket, const char* data, size_t count) {
        // 解码器可能因 PCM 缓冲满或用户暂停而长期停止读取；这是背压，不是断流。
        // 非阻塞发送按实际已写字节推进，短轮询只用于取消，不给消费者设置一秒期限。
        while (count && !stopped()) {
            fd_set writable{}; FD_ZERO(&writable); FD_SET(socket, &writable);
            timeval wait{0, 100000};
            const auto ready = select(int(socket + 1), nullptr, &writable, nullptr, &wait);
            if (!ready) continue;
            if (ready < 0) {
#ifdef _WIN32
                if (WSAGetLastError() == WSAEINTR) continue;
#else
                if (errno == EINTR) continue;
#endif
                return false;
            }
#ifdef _WIN32
            const auto sent = ::send(socket, data, int(std::min(count, size_t(65536))), 0);
            if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
#else
            const auto sent = ::send(socket, data, std::min(count, size_t(65536)), MSG_NOSIGNAL);
            if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
#endif
            if (sent <= 0) return false;
            data += sent; count -= size_t(sent);
        }
        return count == 0;
    }
    void serve(Socket socket) {
        timeout(socket);
        std::string request;
        char buffer[2048];
        while (!stopped() && request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
            const auto got = recv(socket, buffer, sizeof buffer, 0); if (got <= 0) return; request.append(buffer, size_t(got));
        }
        if (!nonblocking(socket)) return;
        const bool head = request.rfind("HEAD " + route + " HTTP/1.", 0) == 0;
        if (!head && request.rfind("GET " + route + " HTTP/1.", 0) != 0) { const char* error = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; send_bytes(socket, error, std::strlen(error)); return; }
        std::string lower = request; std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        const auto range = lower.find("\r\nrange: bytes=");
        long long begin = 0, end = total - 1;
        if (range != std::string::npos) {
            const auto start = range + 15, dash = lower.find('-', start), finish = lower.find("\r\n", start);
            try {
                if (dash == std::string::npos || finish == std::string::npos || dash >= finish || dash == start || lower.find(',', start) < finish) throw std::invalid_argument("range");
                size_t used = 0; const auto first = lower.substr(start, dash - start); begin = std::stoll(first, &used); if (used != first.size()) throw std::invalid_argument("range");
                if (dash + 1 < finish) { const auto last = lower.substr(dash + 1, finish - dash - 1); end = std::stoll(last, &used); if (used != last.size()) throw std::invalid_argument("range"); }
                end = std::min(end, total - 1); if (begin < 0 || begin >= total || end < begin) throw std::invalid_argument("range");
            } catch (...) { const auto error = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\nConnection: close\r\nContent-Range: bytes */" + std::to_string(total) + "\r\n\r\n"; send_bytes(socket, error.data(), error.size()); return; }
        }
        std::string response = range == std::string::npos ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 206 Partial Content\r\n";
        response += "Content-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\nConnection: close\r\nContent-Length: " + std::to_string(end - begin + 1) + "\r\n";
        if (range != std::string::npos) response += "Content-Range: bytes " + std::to_string(begin) + "-" + std::to_string(end) + "/" + std::to_string(total) + "\r\n";
        response += "\r\n";
        if (!send_bytes(socket, response.data(), response.size()) || head) return;
        net::CancelScope signal([this] { return stopped(); });
        net::HttpSession session;
        size_t block_size = 256 * 1024;
        for (auto offset = begin; offset <= end && !stopped();) {
            const auto last = offset + std::min(end - offset, static_cast<long long>(block_size) - 1);
            auto headers = spec.headers;
            headers.erase(std::remove_if(headers.begin(), headers.end(), [](std::string h) { std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return char(std::tolower(c)); }); return h.rfind("range:", 0) == 0 || h.rfind("accept-encoding:", 0) == 0; }), headers.end());
            headers.push_back("Range: bytes=" + std::to_string(offset) + "-" + std::to_string(last));
            headers.push_back("Accept-Encoding: identity");
            net::Request query{spec.url, {}, headers, 10000, true}; query.max_body = block_size;
            const auto started = std::chrono::steady_clock::now();
            const auto chunk = session.execute(query);
            const auto elapsed = std::chrono::steady_clock::now() - started;
            auto content_range = chunk.content_range;
            const auto first = content_range.find_first_not_of(" \t"); if (first != std::string::npos) content_range.erase(0, first);
            while (!content_range.empty() && std::isspace(static_cast<unsigned char>(content_range.back()))) content_range.pop_back();
            const auto expected = "bytes " + std::to_string(offset) + "-" + std::to_string(last) + "/" + std::to_string(total);
            if (!chunk.ok() || chunk.status != 206 || content_range != expected || chunk.body.size() != size_t(last - offset + 1)) { if (!stopped()) bad.store(true); return; }
            if (!send_bytes(socket, chunk.body.data(), chunk.body.size())) return; // 解码器定位会主动抛弃旧连接。
            offset = last + 1;
            if (chunk.body.size() == block_size) {
                if (elapsed < std::chrono::milliseconds(250)) block_size = std::min(block_size * 2, size_t(512 * 1024));
                else if (elapsed > std::chrono::seconds(1)) block_size = std::max(block_size / 2, size_t(64 * 1024));
            }
        }
    }
public:
    ~RangeBridge() { stop(); }
    bool start(const StreamSpec& value, std::function<bool()> signal) {
        spec = value; cancelled = std::move(signal);
        if (spec.probe) {
            if (!spec.probe->range_supported || spec.probe->total_bytes <= 0) return false;
            total = spec.probe->total_bytes;
        } else {
            const auto probe = net::probe_audio(spec.url, spec.headers, 6000);
            if (!probe.playable || probe.status != 206 || probe.total_bytes <= 0) return false;
            total = probe.total_bytes;
        }
        if (stopped()) return false;
#ifdef _WIN32
        WSADATA data{}; if (WSAStartup(MAKEWORD(2,2), &data) != 0) return false; sockets_ready = true;
#endif
        listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); if (listener == invalid) return false;
        sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK); local.sin_port = 0;
        if (::bind(listener, reinterpret_cast<const sockaddr*>(&local), sizeof local) != 0 || listen(listener, 4) != 0) return false;
#ifdef _WIN32
        int length = sizeof local;
#else
        socklen_t length = sizeof local;
#endif
        if (getsockname(listener, reinterpret_cast<sockaddr*>(&local), &length) != 0) return false;
        std::random_device random;
        route = "/" + net::sha1_hex(std::to_string(random()) + std::to_string(random()) + std::to_string(random()) + std::to_string(platform::ticks())) + "/audio";
        address = "http://127.0.0.1:" + std::to_string(ntohs(local.sin_port)) + route;
        clients.reserve(4);
        acceptor = std::thread([this] {
            while (!stopped()) {
                fd_set read{}; FD_ZERO(&read); FD_SET(listener, &read); timeval wait{0, 100000};
                if (select(int(listener + 1), &read, nullptr, nullptr, &wait) <= 0 || stopped()) continue;
                const auto socket = ::accept(listener, nullptr, nullptr); if (socket == invalid) continue;
                for (auto it = clients.begin(); it != clients.end();) { if ((*it)->done.load()) { (*it)->thread.join(); close_socket((*it)->socket); it = clients.erase(it); } else ++it; }
                if (clients.size() >= 4) { close_socket(socket); continue; }
                auto client = std::make_unique<Client>(); client->socket = socket; auto* pointer = client.get();
                clients.push_back(std::move(client));
                try { pointer->thread = std::thread([this, pointer] { try { serve(pointer->socket); } catch (...) { if (!stopped()) bad.store(true); } ::shutdown(pointer->socket, 2); pointer->done.store(true); });
                } catch (...) { close_socket(pointer->socket); clients.pop_back(); }
            }
        });
        return true;
    }
    const std::string& url() const { return address; }
    bool failed() const { return bad.load(); }
    void stop() {
        stopping.store(true);
        if (listener != invalid) ::shutdown(listener, 2);
        if (acceptor.joinable()) acceptor.join();
        for (auto& client : clients) ::shutdown(client->socket, 2);
        for (auto& client : clients) { if (client->thread.joinable()) client->thread.join(); close_socket(client->socket); }
        clients.clear();
        if (listener != invalid) { close_socket(listener); listener = invalid; }
#ifdef _WIN32
        if (sockets_ready) { WSACleanup(); sockets_ready = false; }
#endif
    }
};
namespace {
// 自动复位的原生事件会记住先于 wait 到达的通知；音频回调只发信号，不获取互斥锁。
class StreamSignal {
#ifdef _WIN32
    HANDLE event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
#else
    int event_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
#endif
public:
    StreamSignal() {
#ifdef _WIN32
        if (!event_)
#else
        if (event_ < 0)
#endif
            throw std::runtime_error("无法创建音频唤醒事件");
    }
    StreamSignal(const StreamSignal&) = delete;
    ~StreamSignal() {
#ifdef _WIN32
        CloseHandle(event_);
#else
        ::close(event_);
#endif
    }
    void notify() noexcept {
#ifdef _WIN32
        SetEvent(event_);
#else
        const uint64_t value = 1;
        while (::write(event_, &value, sizeof value) < 0 && errno == EINTR) {}
#endif
    }
    void wait(int milliseconds = -1) {
#ifdef _WIN32
        WaitForSingleObject(event_, milliseconds < 0 ? INFINITE : DWORD(milliseconds));
#else
        pollfd item{event_, POLLIN, 0};
        if (::poll(&item, 1, milliseconds) > 0) {
            uint64_t value;
            while (::read(event_, &value, sizeof value) < 0 && errno == EINTR) {}
        }
#endif
    }
};
}
struct Stream::Impl {
    static constexpr uint64_t rate = 48000, capacity = rate * 3, prebuffer = rate / 4;
    struct Source { ma_data_source_base base{}; Impl* owner = nullptr; } source;
    StreamSpec spec;
    std::function<bool()> cancelled;
    platform::PipeProcess process;
    std::unique_ptr<RangeBridge> bridge;
    std::vector<int16_t> pcm = std::vector<int16_t>(capacity * 2);
    std::atomic<uint64_t> written{0}, consumed{0};
    std::atomic<uint64_t> resume_buffer{prebuffer};
    std::atomic<bool> stop{false}, decoded{false}, broken{false}, waiting{true};
    std::atomic<bool> input_complete{false}, input_abort{false};
    std::atomic<bool> producer_waiting{false};
    StreamSignal space_ready, progress_ready;
    std::thread input, output, monitor;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::string failure;
    bool source_ready = false;
    bool stopped() const { return stop.load() || (cancelled && cancelled()); }
    void stop_pipeline() {
        stop.store(true); process.terminate(); cv.notify_all();
        space_ready.notify(); progress_ready.notify();
    }
    void fail(const std::string& message) {
        { std::lock_guard<std::mutex> lock(mutex); if (failure.empty()) failure = message; }
        broken.store(true); cv.notify_all(); progress_ready.notify();
    }
    ~Impl() {
        stop_pipeline();
        if (bridge) bridge->stop();
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
        if (!ended && (available < count || (self.waiting.load() && available < self.resume_buffer.load()))) {
            if (frames) std::memset(frames, 0, size_t(count) * 2 * sizeof(float));
            if (!self.waiting.exchange(true)) {
                self.resume_buffer.store(std::min<uint64_t>(rate, self.resume_buffer.load() + prebuffer));
                self.progress_ready.notify();
            }
            *read_count = count; return MA_SUCCESS;
        }
        const auto n = std::min<uint64_t>(count, available);
        const auto first = std::min<uint64_t>(n, capacity - begin % capacity);
        if (frames && n) {
            auto* output = static_cast<float*>(frames);
            for (uint64_t i = 0; i < first * 2; ++i) output[i] = self.pcm[(begin % capacity) * 2 + i] / 32768.0f;
            for (uint64_t i = 0; i < (n - first) * 2; ++i) output[first * 2 + i] = self.pcm[i] / 32768.0f;
        }
        self.consumed.store(begin + n, std::memory_order_seq_cst);
        if (self.producer_waiting.exchange(false, std::memory_order_seq_cst)) self.space_ready.notify();
        if (self.waiting.exchange(false)) self.progress_ready.notify();
        *read_count = n;
        return n ? MA_SUCCESS : MA_AT_END;
    }
    static ma_result seek(ma_data_source* data, ma_uint64 frame) {
        return frame == 0 && get(data).consumed.load() == 0 ? MA_SUCCESS : MA_NOT_IMPLEMENTED;
    }
    static ma_result format(ma_data_source*, ma_format* format, ma_uint32* channels, ma_uint32* sample_rate, ma_channel* map, size_t cap) {
        if (format) *format = ma_format_f32;
        if (channels) *channels = 2;
        if (sample_rate) *sample_rate = ma_uint32(rate);
        if (map) ma_channel_map_init_standard(ma_standard_channel_map_default, map, cap, 2);
        return MA_SUCCESS;
    }
    static ma_result cursor(ma_data_source* data, ma_uint64* frame) { *frame = get(data).consumed.load(); return MA_SUCCESS; }
    static ma_result length(ma_data_source*, ma_uint64*) { return MA_NOT_IMPLEMENTED; }
    bool start(std::string& error, const std::function<bool()>& startup_cancelled) {
        const auto helper = ffmpeg_path(error); if (helper.empty()) return false;
        static const ma_data_source_vtable vtable{read, seek, format, cursor, length, nullptr, 0};
        auto config = ma_data_source_config_init(); config.vtable = &vtable; source.owner = this;
        if (ma_data_source_init(&config, &source.base) != MA_SUCCESS) { error = "无法初始化流式音频源"; return false; }
        source_ready = true;
        if (spec.local_path.empty() && spec.range_allowed) {
            auto server = std::make_unique<RangeBridge>();
            if (server->start(spec, [this] { return stopped(); })) bridge = std::move(server);
        }
        std::vector<std::string> args{helper, "-nostdin", "-v", "error"};
        if (bridge) args.insert(args.end(), {"-protocol_whitelist", "http,tcp", "-http_proxy", "", "-rw_timeout", "10000000"});
        const auto seconds = std::to_string(double(std::max(0LL,spec.start_ms)) / 1000.0);
        if ((!spec.local_path.empty() || bridge) && spec.start_ms > 0) { args.push_back("-ss"); args.push_back(seconds); }
        args.insert(args.end(), {"-i", bridge ? bridge->url() : spec.local_path.empty() ? "pipe:0" : spec.local_path});
        // 管道不能随机访问；网络 seek 从头重建并解码丢弃到目标，不伪报定位完成。
        if (spec.local_path.empty() && !bridge && spec.start_ms > 0) { args.push_back("-ss"); args.push_back(seconds); }
        args.insert(args.end(), {"-vn", "-acodec", "pcm_s16le", "-f", "s16le", "-ar", "48000", "-ac", "2", "pipe:1"});
        if (!process.start(args)) { error = "无法启动流式解码器"; return false; }
        monitor = std::thread([this] {
            auto last_progress = std::chrono::steady_clock::now();
            uint64_t previous_written = 0;
            while (!decoded.load() && !stop.load()) {
                if (stopped()) { stop_pipeline(); return; }
                const auto produced = written.load();
                const auto read = consumed.load();
                const auto now = std::chrono::steady_clock::now();
                // 只检测缺少 PCM 时的停滞；暂停造成的正常背压不会触发。
                if (produced != previous_written || !waiting.load() || produced >= read + resume_buffer.load())
                    last_progress = now;
                previous_written = produced;
                if (waiting.load() && now - last_progress >= std::chrono::seconds(15)) {
                    fail("音频缓冲超过 15 秒没有进展");
                    input_abort.store(true); stop_pipeline(); return;
                }
                // PCM 生产/欠载通知负责唤醒；一秒兜底只检查外部取消，不再每 20 ms 轮询。
                progress_ready.wait(1000);
            }
        });
        if (spec.local_path.empty() && !bridge) input = std::thread([this] {
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
                    if (!n) {
                        producer_waiting.store(true, std::memory_order_seq_cst);
                        if (!stopped() && written.load() - consumed.load(std::memory_order_seq_cst) >= capacity) space_ready.wait();
                        producer_waiting.store(false, std::memory_order_seq_cst);
                        continue;
                    }
                    std::memcpy(pcm.data() + begin % capacity * 2, bytes.data() + done * 4, size_t(n) * 4);
                    written.store(begin + n, std::memory_order_release); done += size_t(n);
                    cv.notify_all(); progress_ready.notify();
                }
                tail = (got + tail) % 4;
                if (tail) std::memmove(bytes.data(), bytes.data() + frames * 4, tail);
            }
            if (stopped()) process.terminate();
            const int code = process.wait();
            if (!stopped() && (code != 0 || !written.load())) fail("此音频无法继续流式解码");
            if (!stopped() && bridge && bridge->failed()) fail("远端未正确返回定位数据，尝试完整文件播放");
            // 某些解复用器在 HTTP 提前 EOF 后仍返回 0，不能据此把短片段当成整曲结束。
            const auto decoded_ms = spec.start_ms + static_cast<long long>(written.load() * 1000 / rate);
            const auto tolerance_ms = std::clamp(spec.duration_ms / 100, 2000LL, 10000LL);
            if (!stopped() && spec.duration_ms > 0 && decoded_ms < spec.duration_ms - tolerance_ms)
                fail("流式音频提前结束，正在恢复剩余播放位置");
            if (!stopped() && !input_complete.load(std::memory_order_acquire)) fail("音频传输未完成，解码已提前结束");
            input_abort.store(true);
            decoded.store(true, std::memory_order_release); cv.notify_all(); progress_ready.notify();
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        std::unique_lock<std::mutex> lock(mutex);
        while (written.load() < prebuffer && !decoded.load() && !broken.load() && !stopped()) {
            if ((startup_cancelled && startup_cancelled()) || std::chrono::steady_clock::now() >= deadline) break;
            cv.wait_for(lock, std::chrono::milliseconds(40));
        }
        if ((startup_cancelled && startup_cancelled()) || stopped() || broken.load() || written.load() == 0 || (written.load() < prebuffer && !decoded.load())) {
            error = !failure.empty() ? failure : stopped() ? "流式播放已取消" : "流式预缓冲失败，尝试完整文件";
            lock.unlock(); stop_pipeline(); return false;
        }
        return true;
    }
};
Stream::Stream() : impl_(std::make_unique<Impl>()) {}
Stream::~Stream() = default;
namespace {
// 包含当前、准备中和回收中的流，总数最多四个；容量耗尽时由调用方回退文件播放。
// UI 只取消并移交指针，join 网络/解码线程和关闭 loopback 连接均在这个线程完成。
class StreamRetirement {
    static constexpr size_t limit = 4;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::array<Stream*, limit> pending_{};
    size_t head_ = 0, count_ = 0, outstanding_ = 0;
    bool stopping_ = false;
    std::thread worker_;
    void run() {
        for (;;) {
            Stream* stream = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait(lock, [&] { return stopping_ || count_ != 0; });
                if (!count_ && stopping_) return;
                stream = pending_[head_]; head_ = (head_ + 1) % limit; --count_;
            }
            delete stream;
            release_slot();
        }
    }
public:
    StreamRetirement() : worker_([this] { run(); }) {}
    ~StreamRetirement() {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
        changed_.notify_all();
        worker_.join();
    }
    bool acquire_slot() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (outstanding_ >= limit || stopping_) return false;
        ++outstanding_; return true;
    }
    void release_slot() {
        { std::lock_guard<std::mutex> lock(mutex_); --outstanding_; }
        changed_.notify_all();
    }
    void retire(Stream* stream) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_[(head_ + count_) % limit] = stream; ++count_;
        }
        changed_.notify_all();
    }
    void wait_idle() {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return outstanding_ == 0; });
    }
};
std::shared_ptr<StreamRetirement> stream_retirement() {
    static const auto instance = std::make_shared<StreamRetirement>();
    return instance;
}
}
void wait_for_stream_cleanup() { stream_retirement()->wait_idle(); }
std::shared_ptr<Stream> Stream::open(StreamSpec spec, std::function<bool()> cancelled, std::string& error,
                                     const std::function<bool()>& startup_cancelled) {
    error.clear();
    try {
        auto retirement = stream_retirement();
        if (!retirement->acquire_slot()) { error = "旧音频管线正在回收，暂时使用文件播放"; return {}; }
        Stream* raw = nullptr;
        try { raw = new Stream; }
        catch (...) { retirement->release_slot(); throw; }
        auto stream = std::shared_ptr<Stream>(raw, [retirement](Stream* retired) {
            retired->cancel();
            retirement->retire(retired);
        });
        stream->impl_->spec = std::move(spec); stream->impl_->cancelled = std::move(cancelled);
        if (!stream->impl_->start(error, startup_cancelled)) {
            if (stream->impl_->bridge && stream->impl_->spec.range_allowed &&
                !(stream->impl_->cancelled && stream->impl_->cancelled()) && !(startup_cancelled && startup_cancelled())) {
                auto fallback = stream->impl_->spec; fallback.range_allowed = false;
                auto signal = stream->impl_->cancelled;
                stream.reset(); // 先取消旧管线，异步回收；顺序管道不复用旧端口或进程。
                return open(std::move(fallback), std::move(signal), error, startup_cancelled);
            }
            return {};
        }
        return stream;
    }
    catch (...) { error = "无法创建流式音频任务"; return {}; }
}
void Stream::cancel() { impl_->stop_pipeline(); }
bool Stream::failed() const { return impl_->broken.load(); }
bool Stream::buffering() const { return impl_->waiting.load() && !impl_->decoded.load(); }
long long Stream::buffered_ms() const {
    const auto consumed = impl_->consumed.load(std::memory_order_acquire);
    const auto written = impl_->written.load(std::memory_order_acquire);
    return static_cast<long long>((written - std::min(written, consumed)) * 1000 / Impl::rate);
}
std::string Stream::error() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->failure; }

static std::string device_key(ma_backend backend, const ma_device_id& id) {
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&id); size_t size = 0;
    // 仅序列化后端的有效成员，避免 union 尾部未定义字节影响跨启动匹配。
    const auto text_size = [](const char* text, size_t cap) { size_t n = 0; while (n < cap && text[n]) ++n; return n; };
    switch (backend) {
        case ma_backend_wasapi: while (size < 64 && id.wasapi[size]) ++size; size *= sizeof(id.wasapi[0]); break;
        case ma_backend_dsound: size = sizeof id.dsound; break;
        case ma_backend_winmm: size = sizeof id.winmm; break;
        case ma_backend_alsa: size = text_size(id.alsa, sizeof id.alsa); break;
        case ma_backend_pulseaudio: size = text_size(id.pulse, sizeof id.pulse); break;
        case ma_backend_coreaudio: size = text_size(id.coreaudio, sizeof id.coreaudio); break;
        case ma_backend_sndio: size = text_size(id.sndio, sizeof id.sndio); break;
        case ma_backend_audio4: size = text_size(id.audio4, sizeof id.audio4); break;
        case ma_backend_oss: size = text_size(id.oss, sizeof id.oss); break;
        case ma_backend_jack: size = sizeof id.jack; break;
        case ma_backend_null: size = sizeof id.nullbackend; break;
        default: return {}; // 自定义/未知后端不持久化指针或未定义标识。
    }
    const char* hex = "0123456789abcdef"; std::string key = std::string(ma_get_backend_name(backend)) + ":";
    for (size_t i = 0; i < size; ++i) { key += hex[bytes[i] >> 4]; key += hex[bytes[i] & 15]; }
    return key;
}
std::vector<OutputDevice> output_devices(std::string& error) {
    std::vector<OutputDevice> out; ma_context context{};
    const auto rc = ma_context_init(nullptr, 0, nullptr, &context);
    if (rc != MA_SUCCESS) { error = ma_result_description(rc); return out; }
    ma_device_info* devices = nullptr; ma_uint32 count = 0;
    if (ma_context_get_devices(&context, &devices, &count, nullptr, nullptr) == MA_SUCCESS)
        for (ma_uint32 i = 0; i < count; ++i) out.push_back({device_key(context.backend, devices[i].id), devices[i].name, devices[i].isDefault != 0});
    else error = "无法读取系统音频设备";
    ma_context_uninit(&context); return out;
}
struct Player::Impl {
    // 所有权由 UI 线程持有；回调只读已发布的 Segment，退出读取后才回收旧段。
    struct Segment {
        ma_resource_manager_data_source file{};
        bool file_ready = false;
        std::shared_ptr<Stream> stream;
        ma_data_source* source() { return stream ? reinterpret_cast<ma_data_source*>(&stream->impl_->source.base) : reinterpret_cast<ma_data_source*>(&file); }
        ~Segment() { if (file_ready) ma_resource_manager_data_source_uninit(&file); if (stream) stream->cancel(); }
    };
    struct Sequence { ma_data_source_base base{}; Impl* owner = nullptr; } sequence;
    ma_context context{};
    ma_resource_manager resources{};
    ma_engine engine{};
    ma_sound sound{};
    bool context_ready = false, resources_ready = false, engine_ready = false, sound_ready = false, sequence_ready = false;
    bool device_fallback = false, offline = false;
    std::string selected_device, error;
    std::vector<std::unique_ptr<Segment>> segments;
    std::atomic<Segment*> current{nullptr}, next{nullptr};
    std::atomic<unsigned> readers{0};
    std::atomic<bool> transitioned{false}, normalization{false};
    std::atomic<int> read_failure{MA_SUCCESS};
    std::atomic<int> volume{100};
    std::atomic<bool> smooth{false}, pause_on_device{true}, safety_mute{false}, device_changed{false};
    std::atomic<float> sleep_gain{1.0f};
    std::atomic<unsigned> eq_revision{0};
    std::array<std::atomic<int>, 10> eq_values{};
    std::atomic<int> eq_preamp{0};
    std::atomic<bool> eq_enabled{false};
    struct Band {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1[2]{}, z2[2]{};
        float process(float sample, int channel) {
            const double result = b0 * sample + z1[channel];
            z1[channel] = b1 * sample - a1 * result + z2[channel];
            z2[channel] = b2 * sample - a2 * result;
            // 防止静音尾部进入极小浮点数运算。
            if (std::abs(z1[channel]) < 1e-24) z1[channel] = 0;
            if (std::abs(z2[channel]) < 1e-24) z2[channel] = 0;
            return float(result);
        }
    };
    std::array<Band, 10> bands{};
    std::array<bool, 10> band_active{};
    unsigned applied_eq_revision = ~0u;
    bool apply_eq = false;
    float eq_mix = 0, eq_level = 1;
    // 下面仅由音频线程访问，避免回调分配内存、取锁或执行网络任务。
    double mean_power = 0.0;
    float gain = 1.0f, output_volume = 1.0f;
    Segment* normalized_segment = nullptr;

    static Impl& get(ma_data_source* source) { return *reinterpret_cast<Sequence*>(source)->owner; }
    static ma_result read(ma_data_source* source, void* output, ma_uint64 count, ma_uint64* read_count) {
        auto& self = get(source);
        struct Reading { std::atomic<unsigned>& count; Reading(std::atomic<unsigned>& c) : count(c) { count.fetch_add(1, std::memory_order_seq_cst); } ~Reading() { count.fetch_sub(1, std::memory_order_seq_cst); } } reading(self.readers);
        *read_count = 0;
        while (*read_count < count) {
            auto* segment = self.current.load(std::memory_order_seq_cst);
            if (!segment) return *read_count ? MA_SUCCESS : MA_AT_END;
            ma_uint64 got = 0;
            const auto result = ma_data_source_read_pcm_frames(segment->source(), output ? static_cast<float*>(output) + *read_count * 2 : nullptr, count - *read_count, &got);
            *read_count += got;
            if (segment->stream && segment->stream->failed()) {
                self.read_failure.store(MA_ERROR); return MA_ERROR;
            }
            if (*read_count == count) return MA_SUCCESS;
            if (result == MA_AT_END || (result == MA_SUCCESS && !got)) {
                // 在本次读取内部换段，剩余帧直接接下一曲，无 UI 调度间隙。
                if (auto* queued = self.next.exchange(nullptr, std::memory_order_seq_cst)) {
                    self.current.store(queued, std::memory_order_seq_cst);
                    self.transitioned.store(true, std::memory_order_release);
                    continue;
                }
                return *read_count ? MA_SUCCESS : MA_AT_END;
            }
            if (result == MA_BUSY) {
                if (output) std::memset(static_cast<float*>(output) + *read_count * 2, 0, size_t(count - *read_count) * 2 * sizeof(float));
                *read_count = count; return MA_SUCCESS;
            }
            if (result != MA_SUCCESS) { self.read_failure.store(result); return result; }
        }
        return MA_SUCCESS;
    }
    static ma_result seek(ma_data_source* source, ma_uint64 frame) {
        auto* current = get(source).current.load();
        return current ? ma_data_source_seek_to_pcm_frame(current->source(), frame) : MA_INVALID_OPERATION;
    }
    static ma_result format(ma_data_source*, ma_format* f, ma_uint32* channels, ma_uint32* rate, ma_channel* map, size_t cap) {
        if (f) *f = ma_format_f32;
        if (channels) *channels = 2;
        if (rate) *rate = 48000;
        if (map) ma_channel_map_init_standard(ma_standard_channel_map_default, map, cap, 2);
        return MA_SUCCESS;
    }
    static ma_result cursor(ma_data_source* source, ma_uint64* frame) {
        auto* current = get(source).current.load();
        return current ? ma_data_source_get_cursor_in_pcm_frames(current->source(), frame) : MA_INVALID_OPERATION;
    }
    static ma_result length(ma_data_source* source, ma_uint64* frames) {
        auto* current = get(source).current.load();
        return current ? ma_data_source_get_length_in_pcm_frames(current->source(), frames) : MA_INVALID_OPERATION;
    }
    static void process(void* owner, float* samples, ma_uint64 frames) {
        auto& self = *static_cast<Impl*>(owner);
        if (!frames) return;
        const auto revision = self.eq_revision.load(std::memory_order_acquire);
        if (!(revision & 1) && revision != self.applied_eq_revision) {
            std::array<int, 10> values{};
            for (size_t i = 0; i < values.size(); ++i) values[i] = self.eq_values[i].load(std::memory_order_relaxed);
            const int preamp = self.eq_preamp.load(std::memory_order_relaxed);
            const bool enabled = self.eq_enabled.load(std::memory_order_relaxed);
            if (revision == self.eq_revision.load(std::memory_order_acquire)) {
                constexpr double frequencies[]{31, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
                for (size_t i = 0; i < values.size(); ++i) {
                    auto& band = self.bands[i];
                    const double a = std::pow(10.0, values[i] / 40.0), w = 6.283185307179586 * frequencies[i] / 48000.0;
                    const double alpha = std::sin(w) / (2 * 1.4), a0 = 1 + alpha / a;
                    band.b0 = (1 + alpha * a) / a0; band.b1 = -2 * std::cos(w) / a0; band.b2 = (1 - alpha * a) / a0;
                    band.a1 = band.b1; band.a2 = (1 - alpha / a) / a0;
                    if (values[i] == 0 || !self.apply_eq) { band.z1[0] = band.z1[1] = band.z2[0] = band.z2[1] = 0; }
                    self.band_active[i] = values[i] != 0;
                }
                const auto headroom = std::max(0, *std::max_element(values.begin(), values.end()));
                self.eq_level = float(std::pow(10.0, (preamp - headroom) / 20.0));
                self.apply_eq = enabled; self.applied_eq_revision = revision;
            }
        }
        if (self.apply_eq || self.eq_mix > 0) for (ma_uint64 f = 0; f < frames; ++f) {
            self.eq_mix = std::clamp(self.eq_mix + (self.apply_eq ? 1.0f : -1.0f) / 480.0f, 0.0f, 1.0f);
            for (int channel = 0; channel < 2; ++channel) {
                auto& sample = samples[size_t(f) * 2 + channel];
                float filtered = sample;
                for (size_t i = 0; i < self.bands.size(); ++i) if (self.band_active[i]) filtered = self.bands[i].process(filtered, channel);
                sample += self.eq_mix * (filtered * self.eq_level - sample);
            }
        }
        const auto* segment = self.current.load(std::memory_order_seq_cst);
        if (self.normalized_segment != segment) { self.normalized_segment = const_cast<Segment*>(segment); self.mean_power = 0; self.gain = 1; }
        const bool enabled = self.normalization.load(std::memory_order_relaxed);
        float target = 1.0f;
        if (enabled) {
            double power = 0; float peak = 0;
            for (size_t i = 0; i < size_t(frames) * 2; ++i) { power += double(samples[i]) * samples[i]; peak = std::max(peak, std::abs(samples[i])); }
            power /= double(frames * 2);
            const double coefficient = 1.0 - std::exp(-double(frames) / (48000.0 * 0.4));
            if (power > 1e-8) self.mean_power = self.mean_power > 0 ? self.mean_power + coefficient * (power - self.mean_power) : power;
            if (self.mean_power > 1e-8 && power > 1e-8) target = float(std::clamp(0.126 / std::sqrt(self.mean_power), 0.25, 2.0));
            else target = self.gain; // 静音不抬增益，避免噪声和重新入声时的抽吸。
            if (peak > 0) target = std::min(target, 0.98f / peak);
        }
        const auto smoothing = float(1.0 - std::exp(-double(frames) / (48000.0 * (target < self.gain ? 0.08 : 2.0))));
        const float final_gain = enabled ? self.gain + smoothing * (target - self.gain) : 1.0f;
        const float volume_target = self.safety_mute.load(std::memory_order_relaxed) ? 0.0f : self.volume.load(std::memory_order_relaxed) / 100.0f * self.sleep_gain.load(std::memory_order_relaxed);
        for (ma_uint64 f = 0; f < frames; ++f) {
            const float progress = float(f + 1) / float(frames);
            const float level = (self.gain + (final_gain - self.gain) * progress) * (self.output_volume + (volume_target - self.output_volume) * progress);
            for (int channel = 0; channel < 2; ++channel) {
                auto& sample = samples[size_t(f) * 2 + channel];
                sample *= level;
                if (enabled || self.apply_eq || self.eq_mix > 0) sample = std::clamp(sample, -0.98f, 0.98f);
            }
        }
        self.gain = final_gain; self.output_volume = volume_target;
    }
    bool ensure_engine() {
        if (engine_ready) return true;
        auto config = ma_engine_config_init();
        config.channels = 2; config.sampleRate = 48000; config.noDevice = offline;
        config.onProcess = process; config.pProcessUserData = this;
        config.notificationCallback = [](const ma_device_notification* event) {
            if (event->type != ma_device_notification_type_stopped && event->type != ma_device_notification_type_rerouted && event->type != ma_device_notification_type_interruption_began) return;
            auto* engine = static_cast<ma_engine*>(event->pDevice->pUserData);
            auto* self = engine ? static_cast<Impl*>(engine->pProcessUserData) : nullptr;
            if (self) { if (self->pause_on_device.load()) self->safety_mute.store(true); self->device_changed.store(true); }
        };
        if (!offline && !context_ready) {
            const auto rc = ma_context_init(nullptr, 0, nullptr, &context);
            if (rc != MA_SUCCESS) { error = std::string("音频后端初始化失败：") + ma_result_description(rc); return false; }
            context_ready = true;
        }
        if (!resources_ready) {
            auto resource_config = ma_resource_manager_config_init();
            resource_config.decodedFormat = ma_format_f32; resource_config.decodedChannels = 2; resource_config.decodedSampleRate = 48000;
            if (ma_resource_manager_init(&resource_config, &resources) != MA_SUCCESS) { error = "无法初始化音频预读器"; return false; }
            resources_ready = true;
        }
        config.pResourceManager = &resources;
        if (!offline) config.pContext = &context;
        ma_device_id selected{};
        if (!offline && !selected_device.empty()) {
            ma_device_info* devices = nullptr; ma_uint32 count = 0;
            if (ma_context_get_devices(&context, &devices, &count, nullptr, nullptr) == MA_SUCCESS)
                for (ma_uint32 i = 0; i < count; ++i)
                    if (device_key(context.backend, devices[i].id) == selected_device) { selected = devices[i].id; config.pPlaybackDeviceID = &selected; break; }
            if (!config.pPlaybackDeviceID) device_fallback = true;
        }
        auto rc = ma_engine_init(&config, &engine);
        if (rc != MA_SUCCESS && config.pPlaybackDeviceID) { device_fallback = true; config.pPlaybackDeviceID = nullptr; rc = ma_engine_init(&config, &engine); }
        if (rc != MA_SUCCESS) { error = std::string("音频后端初始化失败：") + ma_result_description(rc); return false; }
        engine_ready = true; device_changed.store(false); safety_mute.store(false);
        static const ma_data_source_vtable vtable{read, seek, format, cursor, length, nullptr, 0};
        auto source_config = ma_data_source_config_init(); source_config.vtable = &vtable; sequence.owner = this;
        if (ma_data_source_init(&source_config, &sequence.base) != MA_SUCCESS) { error = "无法创建连续播放数据源"; return false; }
        sequence_ready = true;
        return true;
    }
    std::unique_ptr<Segment> file_segment(const std::string& path) {
        auto segment = std::make_unique<Segment>();
        auto config = ma_resource_manager_data_source_config_init();
        config.flags = MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_STREAM;
#ifdef _WIN32
        const auto native = platform::widen(path); config.pFilePathW = native.c_str();
#else
        config.pFilePath = path.c_str();
#endif
        const auto rc = ma_resource_manager_data_source_init_ex(&resources, &config, &segment->file);
        if (rc != MA_SUCCESS) { error = std::string("音频解码失败：") + ma_result_description(rc); return {}; }
        segment->file_ready = true;
        return segment;
    }
    bool attach(std::unique_ptr<Segment> segment) {
        current.store(segment.get()); segments.push_back(std::move(segment));
        // 所有段已统一到引擎格式；关闭多余的变速重采样，避免引入一帧前导延迟。
        const auto rc = ma_sound_init_from_data_source(&engine, &sequence.base, MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH, nullptr, &sound);
        if (rc != MA_SUCCESS) { error = std::string("音频解码失败：") + ma_result_description(rc); current.store(nullptr); segments.clear(); return false; }
        sound_ready = true; return true;
    }
    void collect_segments() {
        // next 为 null 后，新回调无法开始换段；已有换段由 readers 保护。
        if (next.load(std::memory_order_seq_cst)) return;
        if (readers.load(std::memory_order_seq_cst)) return;
        const auto* active = current.load(std::memory_order_seq_cst);
        const auto* queued = next.load(std::memory_order_seq_cst);
        segments.erase(std::remove_if(segments.begin(), segments.end(), [&](const auto& segment) { return segment.get() != active && segment.get() != queued; }), segments.end());
    }
    ~Impl() {
        if (sound_ready) ma_sound_uninit(&sound);
        if (engine_ready) ma_engine_uninit(&engine);
        segments.clear();
        if (sequence_ready) ma_data_source_uninit(&sequence.base);
        if (resources_ready) ma_resource_manager_uninit(&resources);
        if (context_ready) ma_context_uninit(&context);
    }
};
Player::Player(bool offline) : impl_(std::make_unique<Impl>()) { impl_->offline = offline; }
Player::~Player() = default;
bool Player::open(const std::string& path) {
    close(); auto& p = *impl_; p.error.clear();
    if (!p.ensure_engine()) return false;
    auto segment = p.file_segment(path);
    return segment && p.attach(std::move(segment));
}
bool Player::open_stream(std::shared_ptr<Stream> stream) {
    close(); auto& p = *impl_; p.error.clear();
    if (!stream) { p.error = "流式音频为空"; return false; }
    if (!p.ensure_engine()) return false;
    auto segment = std::make_unique<Impl::Segment>(); segment->stream = std::move(stream);
    return p.attach(std::move(segment));
}
bool Player::queue_next(const std::string& path) {
    auto& p = *impl_;
    if (!p.sound_ready || p.transitioned.load()) return false;
    cancel_next();
    auto segment = p.file_segment(path);
    if (!segment) return false;
    auto* pointer = segment.get(); p.segments.push_back(std::move(segment));
    p.next.store(pointer, std::memory_order_seq_cst); return true;
}
bool Player::queue_next_stream(std::shared_ptr<Stream> stream) {
    auto& p = *impl_;
    if (!p.sound_ready || p.transitioned.load() || !stream || stream->failed() || stream->buffered_ms() < 200) return false;
    cancel_next(); auto segment = std::make_unique<Impl::Segment>(); segment->stream = std::move(stream);
    auto* pointer = segment.get(); p.segments.push_back(std::move(segment));
    p.next.store(pointer, std::memory_order_seq_cst); return true;
}
void Player::cancel_next() { impl_->next.store(nullptr, std::memory_order_seq_cst); impl_->collect_segments(); }
bool Player::take_transition() { const bool changed = impl_->transitioned.exchange(false, std::memory_order_acq_rel); impl_->collect_segments(); return changed; }
void Player::close() {
    auto& p = *impl_;
    if (p.sound_ready) { ma_sound_uninit(&p.sound); p.sound_ready = false; }
    p.next.store(nullptr); p.current.store(nullptr); p.transitioned.store(false); p.segments.clear();
    p.read_failure.store(MA_SUCCESS);
}
void Player::shutdown() {
    close(); auto& p = *impl_;
    if (p.engine_ready) { ma_engine_uninit(&p.engine); p.engine_ready = false; }
    if (p.sequence_ready) { ma_data_source_uninit(&p.sequence.base); p.sequence_ready = false; }
    if (p.resources_ready) { ma_resource_manager_uninit(&p.resources); p.resources_ready = false; }
    if (p.context_ready) { ma_context_uninit(&p.context); p.context_ready = false; }
}
bool Player::play() {
    if (!impl_->sound_ready) return false;
    impl_->safety_mute.store(false);
    ma_sound_set_stop_time_in_pcm_frames(&impl_->sound, ~(ma_uint64)0);
    ma_sound_set_fade_in_milliseconds(&impl_->sound, impl_->smooth.load() ? 0.0f : 1.0f, 1.0f, impl_->smooth.load() ? 100 : 0);
    const auto rc = ma_sound_start(&impl_->sound);
    if (rc != MA_SUCCESS) impl_->error = std::string("启动播放失败：") + ma_result_description(rc);
    return rc == MA_SUCCESS;
}
void Player::pause(bool fade) {
    if (!impl_->sound_ready) return;
    if (fade && impl_->smooth.load() && !impl_->safety_mute.load()) ma_sound_stop_with_fade_in_milliseconds(&impl_->sound, 100);
    else ma_sound_stop(&impl_->sound);
}
void Player::resume() { play(); }
bool Player::is_playing() const { return impl_->sound_ready && ma_sound_is_playing(&impl_->sound); }
bool Player::is_ended() const { return impl_->sound_ready && !stream_failed() && ma_sound_at_end(&impl_->sound); }
bool Player::stream_failed() const { auto* current = impl_->current.load(); return impl_->read_failure.load() != MA_SUCCESS || (current && current->stream && current->stream->failed()); }
bool Player::buffering() const { auto* current = impl_->current.load(); return current && current->stream && current->stream->buffering(); }
long long Player::buffered_ms() const { auto* current = impl_->current.load(); return current && current->stream ? current->stream->buffered_ms() : 0; }
long long Player::position_ms() const {
    auto* current = impl_->current.load();
    if (!current) return 0;
    if (current->stream) { const auto& stream = *current->stream->impl_; return stream.spec.start_ms + static_cast<long long>(stream.consumed.load() * 1000 / Stream::Impl::rate); }
    ma_uint64 frames = 0; ma_data_source_get_cursor_in_pcm_frames(current->source(), &frames);
    return static_cast<long long>(frames * 1000 / 48000);
}
long long Player::duration_ms() const {
    auto* current = impl_->current.load();
    if (!current) return 0;
    if (current->stream) return current->stream->impl_->spec.duration_ms;
    ma_uint64 frames = 0; ma_data_source_get_length_in_pcm_frames(current->source(), &frames);
    return static_cast<long long>(frames * 1000 / 48000);
}
void Player::seek_ms(long long ms) {
    if (!impl_->sound_ready || (impl_->current.load() && impl_->current.load()->stream)) return;
    cancel_next(); const auto end = duration_ms();
    ms = std::max(0LL, end > 0 ? std::min(ms, end) : ms);
    ma_sound_seek_to_pcm_frame(&impl_->sound, ma_uint64(ms) * 48000 / 1000);
}
void Player::set_volume(int volume) { impl_->volume.store(std::clamp(volume, 0, 100)); }
void Player::set_smoothing(bool enabled) { impl_->smooth.store(enabled); }
void Player::set_sleep_gain(float gain) { impl_->sleep_gain.store(std::clamp(gain, 0.0f, 1.0f)); }
void Player::set_device_pause(bool enabled) { impl_->pause_on_device.store(enabled); }
void Player::set_normalization(bool enabled) { impl_->normalization.store(enabled); }
void Player::set_equalizer(const EqualizerSettings& settings) {
    auto& p = *impl_; p.eq_revision.fetch_add(1, std::memory_order_acq_rel);
    for (size_t i = 0; i < settings.gains_db.size(); ++i) p.eq_values[i].store(std::clamp(settings.gains_db[i], -12, 12), std::memory_order_relaxed);
    p.eq_preamp.store(std::clamp(settings.preamp_db, -12, 12), std::memory_order_relaxed); p.eq_enabled.store(settings.enabled, std::memory_order_relaxed);
    p.eq_revision.fetch_add(1, std::memory_order_release);
}
bool Player::render(float* samples, size_t frames) { return impl_->offline && impl_->engine_ready && ma_engine_read_pcm_frames(&impl_->engine, samples, frames, nullptr) == MA_SUCCESS; }
void Player::set_device(const std::string& id) { shutdown(); impl_->selected_device = id; impl_->device_fallback = false; }
bool Player::take_device_change() {
    const auto* device = impl_->engine_ready && !impl_->offline ? ma_engine_get_device(&impl_->engine) : nullptr;
    const bool notified = impl_->device_changed.exchange(false);
    return impl_->sound_ready && (notified || (device && ma_device_get_state(device) == ma_device_state_stopped));
}
bool Player::take_device_fallback() { const bool value = impl_->device_fallback; impl_->device_fallback = false; return value; }
const std::string& Player::error() const {
    auto* current = impl_->current.load();
    if (current && current->stream && current->stream->failed()) impl_->error = current->stream->error();
    else if (impl_->read_failure.load() != MA_SUCCESS) impl_->error = std::string("音频读取失败：") + ma_result_description(ma_result(impl_->read_failure.load()));
    return impl_->error;
}
} // namespace audio
