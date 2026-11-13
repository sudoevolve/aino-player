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
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
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
// 每个连接最多缓存 256 KiB，不猜测时间/码率对应的字节位置，由容器索引准确定位。
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
        for (auto offset = begin; offset <= end && !stopped();) {
            const auto last = offset + std::min(end - offset, 256LL * 1024 - 1);
            auto headers = spec.headers;
            headers.erase(std::remove_if(headers.begin(), headers.end(), [](std::string h) { std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return char(std::tolower(c)); }); return h.rfind("range:", 0) == 0 || h.rfind("accept-encoding:", 0) == 0; }), headers.end());
            headers.push_back("Range: bytes=" + std::to_string(offset) + "-" + std::to_string(last));
            headers.push_back("Accept-Encoding: identity");
            net::Request query{spec.url, {}, headers, 10000, true}; query.max_body = 256 * 1024;
            const auto chunk = net::execute(query);
            auto content_range = chunk.content_range;
            const auto first = content_range.find_first_not_of(" \t"); if (first != std::string::npos) content_range.erase(0, first);
            while (!content_range.empty() && std::isspace(static_cast<unsigned char>(content_range.back()))) content_range.pop_back();
            const auto expected = "bytes " + std::to_string(offset) + "-" + std::to_string(last) + "/" + std::to_string(total);
            if (!chunk.ok() || chunk.status != 206 || content_range != expected || chunk.body.size() != size_t(last - offset + 1)) { if (!stopped()) bad.store(true); return; }
            if (!send_bytes(socket, chunk.body.data(), chunk.body.size())) return; // 解码器定位会主动抛弃旧连接。
            offset = last + 1;
        }
    }
public:
    ~RangeBridge() { stop(); }
    bool start(const StreamSpec& value, std::function<bool()> signal) {
        spec = value; cancelled = std::move(signal);
        const auto probe = net::probe_audio(spec.url, spec.headers, 6000);
        if (stopped() || probe.status != 206 || probe.total_bytes <= 0 || !probe.playable) return false;
        total = probe.total_bytes;
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
struct Stream::Impl {
    static constexpr uint64_t rate = 48000, capacity = rate * 3, prebuffer = rate / 4;
    struct Source { ma_data_source_base base{}; Impl* owner = nullptr; } source;
    StreamSpec spec;
    std::function<bool()> cancelled;
    platform::PipeProcess process;
    std::unique_ptr<RangeBridge> bridge;
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
            while (!decoded.load() && !stop.load()) {
                if (stopped()) { stop.store(true); process.terminate(); cv.notify_all(); return; }
                platform::sleep_ms(20);
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
            if (!stopped() && bridge && bridge->failed()) fail("远端未正确返回定位数据，尝试完整文件播放");
            // 某些解复用器在 HTTP 提前 EOF 后仍返回 0，不能据此把短片段当成整曲结束。
            const auto decoded_ms = spec.start_ms + static_cast<long long>(written.load() * 1000 / rate);
            const auto tolerance_ms = std::clamp(spec.duration_ms / 100, 2000LL, 10000LL);
            if (!stopped() && spec.duration_ms > 0 && decoded_ms < spec.duration_ms - tolerance_ms)
                fail("流式音频提前结束，正在恢复剩余播放位置");
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
    try {
        if (!stream->impl_->start(error)) {
            if (stream->impl_->bridge && stream->impl_->spec.range_allowed &&
                !(stream->impl_->cancelled && stream->impl_->cancelled())) {
                auto fallback = stream->impl_->spec; fallback.range_allowed = false;
                auto signal = stream->impl_->cancelled;
                stream.reset(); // 先回收旧进程、网络请求和端口，再走顺序管道。
                return open(std::move(fallback), std::move(signal), error);
            }
            return {};
        }
    }
    catch (...) { error = "无法创建流式音频任务"; return {}; }
    return stream;
}
void Stream::cancel() { impl_->stop.store(true); impl_->process.terminate(); impl_->cv.notify_all(); }
bool Stream::failed() const { return impl_->broken.load(); }
bool Stream::buffering() const { return impl_->waiting.load() && !impl_->decoded.load(); }
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
    ma_context context{};
    bool context_ready = false, device_fallback = false;
    ma_engine engine{};
    ma_sound sound{};
    bool engine_ready = false;
    std::string selected_device;
    bool sound_ready = false;
    int volume = 100;
    std::string error;
    std::shared_ptr<Stream> stream;
    bool ensure_engine() {
        if (engine_ready) return true;
        auto& p = *this;
        auto config = ma_engine_config_init();
        if (!context_ready) {
            const auto rc = ma_context_init(nullptr, 0, nullptr, &context);
            if (rc != MA_SUCCESS) { error = std::string("音频后端初始化失败：") + ma_result_description(rc); return false; }
            context_ready = true;
        }
        config.pContext = &context;
        ma_device_id selected{};
        if (!selected_device.empty()) {
            ma_device_info* devices = nullptr; ma_uint32 count = 0;
            if (ma_context_get_devices(&context, &devices, &count, nullptr, nullptr) == MA_SUCCESS)
                for (ma_uint32 i = 0; i < count; ++i)
                    if (device_key(context.backend, devices[i].id) == selected_device) { selected = devices[i].id; config.pPlaybackDeviceID = &selected; break; }
            if (!config.pPlaybackDeviceID) device_fallback = true;
        }
        auto rc = ma_engine_init(&config, &p.engine);
        if (rc != MA_SUCCESS && config.pPlaybackDeviceID) { device_fallback = true; config.pPlaybackDeviceID = nullptr; rc = ma_engine_init(&config, &p.engine); }
        if (rc != MA_SUCCESS) { p.error = std::string("音频设备初始化失败：") + ma_result_description(rc); return false; }
        p.engine_ready = true;
        return true;
    }
    ~Impl() { if (sound_ready) ma_sound_uninit(&sound); if (engine_ready) ma_engine_uninit(&engine); if (context_ready) ma_context_uninit(&context); }
};
Player::Player() : impl_(std::make_unique<Impl>()) {}
Player::~Player() = default;
bool Player::open(const std::string& path) {
    close();
    auto& p = *impl_;
    p.error.clear();
    if (!p.ensure_engine()) return false;
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
    if (!p.ensure_engine()) return false;
    const auto rc = ma_sound_init_from_data_source(&p.engine, &stream->impl_->source.base, 0, nullptr, &p.sound);
    if (rc != MA_SUCCESS) { p.error = std::string("流式播放初始化失败：") + ma_result_description(rc); return false; }
    p.stream = std::move(stream); p.sound_ready = true;
    ma_sound_set_volume(&p.sound,float(p.volume)/100); return true;
}
void Player::close() {
    if (impl_->sound_ready) { ma_sound_uninit(&impl_->sound); impl_->sound_ready = false; }
    if (impl_->stream) { impl_->stream->cancel(); impl_->stream.reset(); }
}
void Player::shutdown() {
    close();
    if (impl_->engine_ready) { ma_engine_uninit(&impl_->engine); impl_->engine_ready = false; }
    if (impl_->context_ready) { ma_context_uninit(&impl_->context); impl_->context_ready = false; }
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
void Player::set_device(const std::string& id) {
    shutdown(); impl_->selected_device = id; impl_->device_fallback = false;
}
bool Player::take_device_change() {
    // 默认设备切换由 miniaudio 后端处理；失效设备交给 UI 线程重建，回调不执行销毁。
    return impl_->engine_ready && ma_device_get_state(ma_engine_get_device(&impl_->engine)) == ma_device_state_stopped && impl_->sound_ready;
}
bool Player::take_device_fallback() { const bool value = impl_->device_fallback; impl_->device_fallback = false; return value; }
const std::string& Player::error() const {
    if (impl_->stream && impl_->stream->failed()) impl_->error = impl_->stream->error();
    return impl_->error;
}
} // namespace audio
