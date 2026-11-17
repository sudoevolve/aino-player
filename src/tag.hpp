// tag.hpp —— 把封面 / 歌词写进下载下来的音频文件
//
// mp3：ID3v2.3（APIC 内嵌封面 + USLT 内嵌歌词）。选 2.3 不是因为新，是因为最兼容：
//      Windows 资源管理器、WMP、Foobar2000、手机自带播放器都认；2.4 的 UTF-8 帧
//      有些老播放器读成乱码。文本统一用 UTF-16LE + BOM（encoding=1）。
//
// m4a：MP4 的音符，udta/meta/ilst 里塞 covr（data type 13=jpeg / 14=png）
//      和 ©lyr（data type 1=utf8）。坑在于 moov 变大后，后面的 mdat 整体位移，
//      所有 stco/co64 的 chunk 偏移必须同步加上 delta —— 不然播放器找不到音频数据，
//      表现是"能打开、没声音"或者干脆打不开。
#pragma once

#include "platform.hpp"
#include <codecvt>
#include <locale>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>

namespace tag {

// ---------------------------------------------------------------- 小工具

inline void be32(std::string& s, uint32_t v) {
    s += char((v >> 24) & 0xFF);
    s += char((v >> 16) & 0xFF);
    s += char((v >> 8) & 0xFF);
    s += char(v & 0xFF);
}

inline uint32_t rd32(const std::string& s, size_t at) {
    if (at > s.size() || s.size() - at < 4) return 0;
    return (uint32_t(uint8_t(s[at])) << 24) | (uint32_t(uint8_t(s[at + 1])) << 16) |
           (uint32_t(uint8_t(s[at + 2])) << 8) | uint32_t(uint8_t(s[at + 3]));
}

inline void wr32(std::string& s, size_t at, uint32_t v) {
    if (at > s.size() || s.size() - at < 4) return;
    s[at] = char((v >> 24) & 0xFF);
    s[at + 1] = char((v >> 16) & 0xFF);
    s[at + 2] = char((v >> 8) & 0xFF);
    s[at + 3] = char(v & 0xFF);
}

inline void wr_be32(char* p, uint32_t v) {
    p[0] = char((v >> 24) & 0xFF);
    p[1] = char((v >> 16) & 0xFF);
    p[2] = char((v >> 8) & 0xFF);
    p[3] = char(v & 0xFF);
}

inline std::string utf16le_bom(const std::string& utf8) {
    std::string out;
    if (utf8.empty()) return out;
    std::u16string w;
    try { w = std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.from_bytes(utf8); }
    catch (...) { return out; }
    out += char(0xFF);
    out += char(0xFE);
    for (char16_t c : w) {
        out += char(c & 0xFF);
        out += char((c >> 8) & 0xFF);
    }
    return out;
}

inline std::wstring widen(const std::string& s) { return platform::widen(s); }

inline bool read_file(const std::string& path, std::string& out) {
    std::FILE* f = platform::fopen(path, "rb");
    if (!f) return false;
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return false; }
    const long n = std::ftell(f);
    if (std::fseek(f, 0, SEEK_SET) != 0) { std::fclose(f); return false; }
    if (n <= 0) {
        std::fclose(f);
        return false;
    }
    out.resize(size_t(n));
    const size_t got = std::fread(&out[0], 1, size_t(n), f);
    std::fclose(f);
    return got == size_t(n);
}

using File = std::unique_ptr<std::FILE, int(*)(std::FILE*)>;
using Cancel = std::function<bool()>;
inline bool seek_file(std::FILE* file, uint64_t at) {
    if (at > uint64_t(std::numeric_limits<int64_t>::max())) return false;
#ifdef _WIN32
    return _fseeki64(file, static_cast<__int64>(at), SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(at), SEEK_SET) == 0;
#endif
}
inline bool copy_bytes(std::FILE* input, std::FILE* output, uint64_t count, const Cancel& cancel = {}) {
    char buffer[65536];
    while (count) {
        if (cancel && cancel()) return false;
        const auto n = size_t(std::min<uint64_t>(count, sizeof buffer));
        if (std::fread(buffer, 1, n, input) != n || std::fwrite(buffer, 1, n, output) != n) return false;
        count -= n;
    }
    return true;
}
// 写完、flush 和 close 均成功才替换。异常或取消会回收本次独占创建的临时文件。
template<class Writer> inline bool write_replace_stream(const std::string& path, Writer writer, const Cancel& cancel = {}) {
    static std::atomic<unsigned long long> sequence{0};
    const auto tmp = path + ".tagtmp." + std::to_string(platform::process_id()) + "." + std::to_string(++sequence);
    struct Cleanup { const std::string& path; bool owned = false; ~Cleanup() { if (owned) platform::remove(path); } } cleanup{tmp};
    File output(platform::fopen(tmp, "wbx"), &std::fclose);
    if (!output) return false;
    cleanup.owned = true;
    const bool written = writer(output.get());
    const bool flushed = written && (!cancel || !cancel()) && platform::flush_file(output.get());
    const bool closed = std::fclose(output.release()) == 0;
    return flushed && closed && (!cancel || !cancel()) && platform::replace_file(tmp, path);
}
inline bool write_replace(const std::string& path, const std::string& bytes) {
    return write_replace_stream(path, [&](std::FILE* file) { return std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size(); });
}

// 只读取有界标签块，扫描不把整首音频或 mdat 载入内存。
struct Metadata { std::string title, artist, album, cover; long long duration_ms = 0; };
inline uint32_t le32(const std::string& bytes, size_t at) {
    if (at > bytes.size() || bytes.size() - at < 4) return 0;
    return uint32_t(uint8_t(bytes[at])) | uint32_t(uint8_t(bytes[at+1])) << 8 |
        uint32_t(uint8_t(bytes[at+2])) << 16 | uint32_t(uint8_t(bytes[at+3])) << 24;
}
inline std::string metadata_text(const std::string& bytes, unsigned encoding) {
    if (bytes.size() > 32768) return {};
    std::string out;
    if (encoding == 0) {
        for (unsigned char c : bytes) {
            if (!c) break;
            if (c < 128) out += char(c); else { out += char(0xc0 | (c >> 6)); out += char(0x80 | (c & 63)); }
        }
    } else if (encoding == 3) out = bytes.substr(0, bytes.find('\0'));
    else {
        size_t at = 0; bool little = encoding == 1;
        if (bytes.size() >= 2 && (uint8_t(bytes[0]) == 0xff || uint8_t(bytes[0]) == 0xfe)) {
            little = uint8_t(bytes[0]) == 0xff; at = 2;
        }
        std::u16string text;
        for (; at + 1 < bytes.size(); at += 2) {
            const auto c = char16_t(little ? uint8_t(bytes[at]) | uint16_t(uint8_t(bytes[at+1])) << 8 :
                uint16_t(uint8_t(bytes[at])) << 8 | uint8_t(bytes[at+1]));
            if (!c) break;
            text += c;
        }
        try { out = std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.to_bytes(text); } catch (...) {}
    }
    return out.size() <= 16384 ? out : std::string();
}
inline std::string metadata_base64(const std::string& text) {
    if (text.size() > 12u*1024*1024) return {};
    std::string out; uint32_t value = 0; int bits = 0;
    for (unsigned char c : text) {
        if (c == '=') break;
        const int digit = c >= 'A' && c <= 'Z' ? c-'A' : c >= 'a' && c <= 'z' ? c-'a'+26 :
            c >= '0' && c <= '9' ? c-'0'+52 : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (digit < 0) { if (c == ' ' || c == '\r' || c == '\n') continue; return {}; }
        value = (value << 6) | uint32_t(digit); bits += 6;
        if (bits >= 8) { bits -= 8; out += char(value >> bits); }
        if (out.size() > 8u*1024*1024) return {};
    }
    return out;
}
inline void metadata_picture(const std::string& data, Metadata& out) {
    if (data.size() < 12) return;
    const auto type = rd32(data, 0);
    if (!out.cover.empty() && type != 3) return;
    size_t at = 4;
    for (int i = 0; i < 2; ++i) {
        if (data.size() - at < 4) return;
        const auto size = rd32(data, at); at += 4;
        if (size > data.size() - at) return;
        at += size;
    }
    if (data.size() - at < 20) return;
    at += 16; const auto size = rd32(data, at); at += 4;
    if (size <= data.size() - at && size <= 8u*1024*1024) out.cover = data.substr(at, size);
}
inline void metadata_comments(const std::string& bytes, size_t at, Metadata& out, bool read_cover = true) {
    if (at > bytes.size() || bytes.size() - at < 4) return;
    const auto vendor = le32(bytes, at); at += 4;
    if (vendor > bytes.size() - at) return;
    at += vendor;
    if (bytes.size() - at < 4) return;
    const auto count = le32(bytes, at); at += 4;
    for (uint32_t i = 0; i < std::min(count, 65536u) && bytes.size() - at >= 4; ++i) {
        const auto n = le32(bytes, at); at += 4;
        if (n > bytes.size() - at) return;
        const auto item = bytes.substr(at, n); at += n;
        const auto equal = item.find('=');
        if (equal == std::string::npos) continue;
        auto key = item.substr(0, equal);
        for (auto& c : key) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        const bool bounded = item.size() - equal - 1 <= 16384;
        if (key == "TITLE" && bounded) out.title = item.substr(equal+1);
        else if (key == "ARTIST" && bounded) out.artist = item.substr(equal+1);
        else if (key == "ALBUM" && bounded) out.album = item.substr(equal+1);
        else if (read_cover && key == "METADATA_BLOCK_PICTURE") metadata_picture(metadata_base64(item.substr(equal+1)), out);
    }
}
inline Metadata read_metadata(const std::string& path, bool read_cover = true) {
    Metadata out;
    auto* file = platform::fopen(path, "rb");
    if (!file) return out;
    struct Close { std::FILE* file; ~Close() { std::fclose(file); } } close{file};
    auto read = [&](size_t n) { std::string bytes(std::min<size_t>(n, 16u*1024*1024), '\0'); bytes.resize(std::fread(bytes.data(), 1, bytes.size(), file)); return bytes; };
    auto seek = [&](uint64_t at) {
#ifdef _WIN32
        return _fseeki64(file, static_cast<__int64>(at), SEEK_SET) == 0;
#else
        return fseeko(file, static_cast<off_t>(at), SEEK_SET) == 0;
#endif
    };
    const auto head = read(12);
    if (head.size() < 12) return out;
    if (head.compare(0, 3, "ID3") == 0 && uint8_t(head[3]) >= 2 && uint8_t(head[3]) <= 4) {
        const unsigned version = uint8_t(head[3]);
        const size_t length = size_t(uint8_t(head[6]) & 127) << 21 | size_t(uint8_t(head[7]) & 127) << 14 |
            size_t(uint8_t(head[8]) & 127) << 7 | size_t(uint8_t(head[9]) & 127);
        if (length > 16u*1024*1024 || !seek(10)) return out;
        auto bytes = read(length);
        const bool tag_unsync = (uint8_t(head[5]) & 0x80) != 0;
        auto desync = [](std::string& data) {
            size_t write = 0;
            for (size_t read = 0; read < data.size(); ++read) {
                data[write++] = data[read];
                if (uint8_t(data[read]) == 0xff && read + 1 < data.size() && data[read+1] == 0) ++read;
            }
            data.resize(write);
        };
        if (tag_unsync && version < 4) desync(bytes);
        size_t at = 0;
        if ((uint8_t(head[5]) & 0x40) && version >= 3 && bytes.size() >= 4) {
            const auto extended = rd32(bytes, 0);
            at = version == 3 ? size_t(extended) + 4 : size_t((extended & 0x7f) | (extended >> 1 & 0x3f80) | (extended >> 2 & 0x1fc000) | (extended >> 3 & 0xfe00000));
        }
        const size_t header = version == 2 ? 6 : 10;
        while (at <= bytes.size() && bytes.size() - at >= header) {
            const auto id = bytes.substr(at, version == 2 ? 3 : 4);
            if (id[0] == 0) break;
            size_t n = version == 2 ? size_t(uint8_t(bytes[at+3])) << 16 | size_t(uint8_t(bytes[at+4])) << 8 | uint8_t(bytes[at+5]) : rd32(bytes, at+4);
            if (version == 4) n = (n & 0x7f) | (n >> 1 & 0x3f80) | (n >> 2 & 0x1fc000) | (n >> 3 & 0xfe00000);
            const auto flags = version >= 3 ? uint8_t(bytes[at+9]) : 0;
            const bool encoded = flags & (version == 3 ? 0xe0 : 0x4d);
            at += header;
            if (n > bytes.size() - at || !n) break;
            auto data = bytes.substr(at, n); at += n;
            if (encoded) continue;
            if (version == 4 && (tag_unsync || (flags & 2))) desync(data);
            const unsigned encoding = uint8_t(data[0]);
            if (id == "TIT2" || id == "TT2") out.title = metadata_text(data.substr(1), encoding);
            else if (id == "TPE1" || id == "TP1") out.artist = metadata_text(data.substr(1), encoding);
            else if (id == "TALB" || id == "TAL") out.album = metadata_text(data.substr(1), encoding);
            else if (read_cover && (id == "APIC" || id == "PIC")) {
                size_t image = version == 2 ? 4 : data.find('\0', 1);
                if (image == std::string::npos) continue;
                image += version == 2 ? 1 : 2;
                if (encoding == 1 || encoding == 2) { while (image+1 < data.size() && (data[image] || data[image+1])) image += 2; image += 2; }
                else { const auto end = data.find('\0', image); image = end == std::string::npos ? data.size() : end+1; }
                if (image < data.size() && data.size() - image <= 8u*1024*1024) out.cover = data.substr(image);
            }
        }
    } else if (head.compare(0,4,"fLaC") == 0) {
        uint64_t at = 4;
        for (int i = 0; i < 512 && seek(at); ++i) {
            const auto h = read(4); if (h.size() != 4) break;
            const auto type = uint8_t(h[0]) & 127;
            const size_t n = size_t(uint8_t(h[1])) << 16 | size_t(uint8_t(h[2])) << 8 | uint8_t(h[3]);
            if (n > 16u*1024*1024) break;
            if (type == 0 || type == 4 || (type == 6 && read_cover)) {
                const auto data = read(n);
                if (type == 0 && data.size() >= 18) {
                    uint64_t packed = 0; for (int j = 10; j < 18; ++j) packed = packed << 8 | uint8_t(data[size_t(j)]);
                    const auto rate = packed >> 44; if (rate) out.duration_ms = (packed & 0xfffffffffull) * 1000 / rate;
                } else if (type == 4) metadata_comments(data, 0, out, read_cover);
                else if (type == 6) metadata_picture(data, out);
            }
            at += 4 + n;
            if (uint8_t(h[0]) & 128) break;
        }
    } else if (head.compare(4,4,"ftyp") == 0) {
        const auto total = platform::file_size(path);
        for (uint64_t at=0; at+8 <= uint64_t(std::max(0LL,total)) && seek(at);) {
            const auto h=read(16); if (h.size()<8) break;
            uint64_t n=rd32(h,0); size_t header=8;
            if (n==1) { if(h.size()<16) break; n=uint64_t(rd32(h,8))<<32 | rd32(h,12); header=16; }
            if (!n) n=uint64_t(total)-at;
            if (n<header || n>uint64_t(total)-at) break;
            if (h.compare(4,4,"moov")==0 && n<=16u*1024*1024 && seek(at+header)) {
                const auto data=read(size_t(n)-header);
                std::function<void(size_t,size_t,int)> walk = [&](size_t begin,size_t end,int depth) {
                    if (depth>8) return;
                    for(size_t p=begin;p+8<=end;) {
                        const size_t size=rd32(data,p); if(size<8 || size>end-p) break;
                        const auto type=data.substr(p+4,4); const size_t content=p+8, finish=p+size;
                        if(type=="udta" || type=="ilst") walk(content,finish,depth+1);
                        else if(type=="meta" && finish-content>=4) walk(content+4,finish,depth+1);
                        else if(type==std::string("\xa9" "nam",4) || type==std::string("\xa9" "ART",4) || type=="aART" || type==std::string("\xa9" "alb",4) || (type=="covr" && read_cover)) {
                            for(size_t q=content;q+16<=finish;) {
                                const size_t length=rd32(data,q); if(length<16 || length>finish-q) break;
                                if(data.compare(q+4,4,"data")==0) {
                                    const auto value=data.substr(q+16,length-16);
                                    if(type=="covr") { if(value.size()<=8u*1024*1024) out.cover=value; }
                                    else if(value.size()>16384) {}
                                    else if(type==std::string("\xa9" "nam",4)) out.title=value;
                                    else if(type==std::string("\xa9" "alb",4)) out.album=value;
                                    else if(out.artist.empty()) out.artist=value;
                                }
                                q+=length;
                            }
                        }
                        p+=size;
                    }
                }; walk(0,data.size(),0); break;
            }
            at+=n;
        }
    } else if (head.compare(0,4,"OggS")==0) {
        if (!seek(0)) return out;
        std::string packet; uint32_t serial = 0; bool have_serial = false;
        size_t processed = 0;
        for (int page = 0; page < 512 && processed < 16u*1024*1024; ++page) {
            const auto header = read(27);
            if (header.size() != 27 || header.compare(0,4,"OggS") != 0 || header[4] != 0) break;
            const auto lacing = read(uint8_t(header[26]));
            if (lacing.size() != uint8_t(header[26])) break;
            size_t total = 0; for (unsigned char n : lacing) total += n;
            const auto payload = read(total); if (payload.size() != total) break;
            processed += 27 + lacing.size() + total;
            const auto page_serial = le32(header,14);
            if (!have_serial) { serial = page_serial; have_serial = true; }
            if (page_serial != serial) continue;
            if (!(uint8_t(header[5]) & 1) && !packet.empty()) break;
            size_t at = 0;
            for (unsigned char n : lacing) {
                if (packet.size() + n > 16u*1024*1024) return out;
                packet.append(payload,at,n); at += n;
                if (n == 255) continue;
                if (packet.compare(0,8,"OpusTags") == 0) { metadata_comments(packet,8,out,read_cover); return out; }
                if (packet.compare(0,7,std::string("\x03" "vorbis",7)) == 0) { metadata_comments(packet,7,out,read_cover); return out; }
                packet.clear();
            }
        }
    } else if (head.compare(0,4,"RIFF")==0 && head.compare(8,4,"WAVE")==0) {
        const auto total=platform::file_size(path);
        for(uint64_t at=12;at+8<=uint64_t(std::max(0LL,total)) && seek(at);) {
            const auto h=read(8); if(h.size()<8) break; const auto n=le32(h,4);
            if(n>uint64_t(total)-at-8) break;
            if(h.compare(0,4,"LIST")==0 && n<=1024u*1024) {
                const auto data=read(n);
                if(data.compare(0,4,"INFO")==0) for(size_t p=4;p+8<=data.size();) {
                    const auto size=le32(data,p+4); if(size>data.size()-p-8) break;
                    const auto value=data.substr(p+8,size); const auto text=metadata_text(value,3);
                    if(data.compare(p,4,"INAM")==0) out.title=text; else if(data.compare(p,4,"IART")==0) out.artist=text; else if(data.compare(p,4,"IPRD")==0) out.album=text;
                    p+=8+size+(size&1);
                }
            }
            at+=8+n+(n&1);
        }
    }
    return out;
}

// ---------------------------------------------------------------- mp3 / ID3v2.3

inline std::string id3_frame(const char* id, const std::string& payload) {
    std::string f;
    f.append(id, 4);
    be32(f, uint32_t(payload.size()));  // v2.3 的帧长就是普通 32 位大端
    f += char(0);
    f += char(0);  // flags
    f += payload;
    return f;
}

inline constexpr size_t metadata_limit = 16u * 1024u * 1024u;
inline bool synchsafe(const std::string& bytes, size_t at, uint32_t& value) {
    if (at > bytes.size() || bytes.size() - at < 4) return false;
    value = 0;
    for (size_t i = 0; i < 4; ++i) {
        const auto c = uint8_t(bytes[at+i]);
        if (c & 128) return false;
        value = (value << 7) | c;
    }
    return true;
}
inline void put_synchsafe(std::string& out, uint32_t n) {
    for (int i = 21; i >= 0; i -= 7) out += char((n >> i) & 127);
}
inline std::string id3_unsync(const std::string& bytes, bool encode) {
    std::string out;
    for (size_t i = 0; i < bytes.size(); ++i) {
        const auto c = uint8_t(bytes[i]); out += char(c);
        if (c != 255) continue;
        if (encode) {
            if (i+1 == bytes.size() || bytes[i+1] == 0 || uint8_t(bytes[i+1]) >= 224) out += '\0';
        } else if (i+1 < bytes.size() && bytes[i+1] == 0) ++i;
    }
    return out;
}
inline bool mpeg_audio_header(const std::string& bytes) {
    if (bytes.size() < 4) return false;
    const auto a = uint8_t(bytes[0]), b = uint8_t(bytes[1]), c = uint8_t(bytes[2]);
    // MPEG audio 必须有合法版本、Layer、码率和采样率；ADTS 的 Layer=0 不会命中。
    return a == 255 && (b & 224) == 224 && (b & 24) != 8 && (b & 6) != 0 &&
        (c & 240) != 0 && (c & 240) != 240 && (c & 12) != 12;
}
inline bool embed_mp3(const std::string& path, const std::string& cover, bool is_png,
                      const std::string& lyrics, std::string& err, const Cancel& cancel = {}) {
    File input(platform::fopen(path, "rb"), &std::fclose);
    const auto length = platform::file_size(path);
    if (!input || length < 4) { err = "读文件失败"; return false; }
    std::string head(10, '\0'); head.resize(std::fread(head.data(), 1, head.size(), input.get()));
    unsigned version = 3, flags = 0; uint64_t audio_at = 0;
    std::string old, frames;
    if (head.compare(0, 3, "ID3") == 0) {
        uint32_t n = 0;
        if (head.size() != 10 || (version = uint8_t(head[3])) < 2 || version > 4 || head[4] != 0 ||
            !synchsafe(head, 6, n) || n > metadata_limit) { err = "不支持或损坏的 ID3 标签，保留原文件"; return false; }
        flags = uint8_t(head[5]);
        if ((version == 2 && (flags & 127)) || (version == 3 && (flags & 31)) || (version == 4 && (flags & 15))) {
            err = "不支持的 ID3 标志，保留原文件"; return false;
        }
        audio_at = uint64_t(10) + n + ((version == 4 && (flags & 16)) ? 10 : 0);
        if (audio_at > uint64_t(length)) { err = "ID3 标签或填充无效"; return false; }
        old.resize(n);
        if (std::fread(old.data(), 1, n, input.get()) != n) { err = "读取 ID3 标签失败"; return false; }
        if (version == 4 && (flags & 16)) {
            std::string footer(10, '\0');
            if (std::fread(footer.data(), 1, footer.size(), input.get()) != footer.size() ||
                footer.compare(0, 3, "3DI") != 0 || footer.compare(3, 7, head, 3, 7) != 0) {
                err = "ID3 footer 无效，保留原文件"; return false;
            }
        }
        if (version < 4 && (flags & 128)) old = id3_unsync(old, false);
        size_t at = 0;
        if (flags & 64) {
            uint32_t extended = 0;
            if (version == 4) {
                if (!synchsafe(old, 0, extended) || extended < 6) { err = "ID3 扩展头或长度无效"; return false; }
                at = extended;
            } else { at = size_t(rd32(old, 0)) + 4; if (at < 10) { err = "ID3 扩展头或长度无效"; return false; } }
            if (at > old.size()) { err = "ID3 扩展头或长度无效"; return false; }
        }
        const size_t header = version == 2 ? 6 : 10, id_size = version == 2 ? 3 : 4;
        while (at < old.size()) {
            if (old[at] == 0) {
                if (old.find_first_not_of('\0', at) != std::string::npos) { err = "ID3 标签或填充无效"; return false; }
                break;
            }
            if (old.size() - at < header) { err = "ID3 帧截断或越界"; return false; }
            const auto id = old.substr(at, id_size);
            if (id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != std::string::npos) { err = "ID3 帧 ID 无效"; return false; }
            uint32_t nframe = 0;
            if (version == 2) nframe = uint32_t(uint8_t(old[at+3])) << 16 | uint32_t(uint8_t(old[at+4])) << 8 | uint8_t(old[at+5]);
            else if (version == 3) nframe = rd32(old, at+4);
            else if (!synchsafe(old, at+4, nframe)) { err = "ID3 扩展头或长度无效"; return false; }
            if (nframe > old.size()-at-header) { err = "ID3 帧截断或越界"; return false; }
            const bool replace = (!cover.empty() && (id == "APIC" || id == "PIC")) ||
                (!lyrics.empty() && (id == "USLT" || id == "ULT"));
            if (!replace) frames.append(old, at, header+nframe);
            at += header+nframe;
        }
    }
    if (!seek_file(input.get(), audio_at)) { err = "音频位置无效"; return false; }
    std::string audio(4, '\0'); audio.resize(std::fread(audio.data(), 1, 4, input.get()));
    if (!mpeg_audio_header(audio)) { err = "不是受支持的 MPEG 音频，保留原文件"; return false; }
    input.reset();
    auto add = [&](const char* id, std::string payload) {
        if (version == 4 && (flags & 128)) payload = id3_unsync(payload, true);
        frames.append(id, version == 2 ? 3 : 4);
        if (version == 2) { frames += char(payload.size() >> 16); frames += char(payload.size() >> 8); frames += char(payload.size()); }
        else { if (version == 4) put_synchsafe(frames, uint32_t(payload.size())); else be32(frames, uint32_t(payload.size())); frames.append(2, '\0'); }
        frames += payload;
    };
    if (!cover.empty()) {
        std::string image(1, '\0');
        if (version == 2) image += is_png ? "PNG" : "JPG";
        else { image += is_png ? "image/png" : "image/jpeg"; image += '\0'; }
        image += char(3); image += '\0'; image += cover; add(version == 2 ? "PIC" : "APIC", std::move(image));
    }
    if (!lyrics.empty()) {
        const auto text = utf16le_bom(lyrics);
        if (text.empty()) { err = "歌词不是有效 UTF-8"; return false; }
        std::string lyric(1, char(1)); lyric += "chi"; lyric.append(2, '\0'); lyric += text;
        add(version == 2 ? "ULT" : "USLT", std::move(lyric));
    }
    if (version < 4 && (flags & 128)) frames = id3_unsync(frames, true);
    if (frames.size() > metadata_limit) { err = "标签超过大小上限"; return false; }
    // 扩展头的 CRC/长度已过期；移除扩展头和 footer，保留其他原帧。
    std::string out = "ID3"; out += char(version); out += '\0'; out += char(flags & (version == 2 ? 128 : 160));
    put_synchsafe(out, uint32_t(frames.size())); out += frames;
    if (!write_replace_stream(path, [&](std::FILE* file) {
        File source(platform::fopen(path, "rb"), &std::fclose);
        return source && seek_file(source.get(), audio_at) && std::fwrite(out.data(), 1, out.size(), file) == out.size() &&
            copy_bytes(source.get(), file, uint64_t(length)-audio_at, cancel);
    }, cancel)) { err = "写回失败或已取消，原文件保留"; return false; }
    return true;
}

// ---------------------------------------------------------------- m4a / MP4 atoms
struct Atom { size_t off = 0, size = 0, header = 0; };
inline bool read_atom(const std::string& bytes, size_t at, size_t end, Atom& out) {
    if (end > bytes.size() || at > end || end-at < 8) return false;
    uint64_t size = rd32(bytes, at); size_t header = 8;
    if (size == 1) { if (end-at < 16) return false; size = uint64_t(rd32(bytes, at+8)) << 32 | rd32(bytes, at+12); header = 16; }
    else if (size == 0) size = end-at;
    if (size < header || size > end-at) return false;
    out = {at, size_t(size), header}; return true;
}
inline bool find_atom(const std::string& s, size_t begin, size_t end, const char* type, size_t& off, size_t& size) {
    for (size_t at = begin; at < end;) {
        Atom a; if (!read_atom(s, at, end, a)) return false;
        if (s.compare(at+4, 4, type) == 0) { off = at; size = a.size; return true; }
        at += a.size;
    }
    return false;
}
inline std::string atom(const char* type, const std::string& body) {
    if (body.size() > std::numeric_limits<uint32_t>::max()-8) throw std::length_error("MP4 atom too large");
    std::string out; be32(out, uint32_t(body.size()+8)); out.append(type, 4); out += body; return out;
}
inline std::string data_atom(uint32_t type, const std::string& payload) {
    std::string body; be32(body, type); be32(body, 0); body += payload; return atom("data", body);
}
inline std::string covr_atom(const std::string& cover, bool is_png) { return atom("covr", data_atom(is_png ? 14u : 13u, cover)); }
inline std::string lyric_atom(const std::string& lyrics) { return atom("\xA9lyr", data_atom(1u, lyrics)); }
using MediaRanges = std::vector<std::pair<uint64_t, uint64_t>>;
inline bool media_offset(uint64_t value, const MediaRanges& ranges) {
    for (const auto& range : ranges) if (value >= range.first && value < range.second) return true;
    return false;
}
// 经典 MP4 的绝对 chunk 表限于所属 atom；拒绝碎片化及辅助偏移结构。
inline bool shift_chunk_offsets(std::string& bytes, size_t begin, size_t end, int64_t delta,
                                uint64_t moved_from = 0, const MediaRanges* media = nullptr, unsigned depth = 0) {
    if (depth > 16 || end > bytes.size()) return false;
    for (size_t p = begin; p < end;) {
        Atom a; if (!read_atom(bytes, p, end, a)) return false;
        const auto type = bytes.substr(p+4, 4); const auto body = p+a.header, finish = p+a.size;
        if (type == "mvex" || type == "cmov" || type == "saio" || type == "iloc") return false;
        if (type == "stco" || type == "co64") {
            if (finish-body < 8 || rd32(bytes, body) != 0) return false;
            const size_t step = type == "stco" ? 4 : 8;
            const auto count = rd32(bytes, body+4);
            if (count != (finish-body-8)/step || (finish-body-8)%step) return false;
            for (size_t i = 0; i < count; ++i) {
                const auto at = body+8+i*step;
                const uint64_t value = step == 4 ? rd32(bytes, at) : uint64_t(rd32(bytes, at)) << 32 | rd32(bytes, at+4);
                if (media && !media_offset(value, *media)) return false;
                uint64_t updated = value;
                if (value >= moved_from) {
                    if (delta >= 0) { if (value > UINT64_MAX-uint64_t(delta)) return false; updated += uint64_t(delta); }
                    else { const auto n = uint64_t(-(delta+1))+1; if (value < n) return false; updated -= n; }
                }
                if (step == 4) { if (updated > UINT32_MAX) return false; wr32(bytes, at, uint32_t(updated)); }
                else { wr32(bytes, at, uint32_t(updated >> 32)); wr32(bytes, at+4, uint32_t(updated)); }
            }
        } else if (type == "moov" || type == "trak" || type == "mdia" || type == "minf" || type == "stbl" || type == "edts" || type == "udta" || type == "meta") {
            const size_t prefix = type == "meta" ? 4 : 0;
            if (finish-body < prefix || (prefix && rd32(bytes, body) != 0) ||
                !shift_chunk_offsets(bytes, body+prefix, finish, delta, moved_from, media, depth+1)) return false;
        }
        p = finish;
    }
    return true;
}
inline bool child_atom(const std::string& parent, const char* type, std::string& child, size_t prefix = 0) {
    Atom root; if (!read_atom(parent, 0, parent.size(), root) || root.size != parent.size() || root.size-root.header < prefix) return false;
    for (size_t at = root.header+prefix; at < parent.size();) {
        Atom a; if (!read_atom(parent, at, parent.size(), a)) return false;
        if (parent.compare(at+4, 4, type) == 0) { if (!child.empty()) return false; child = parent.substr(at, a.size); }
        at += a.size;
    }
    return true;
}
inline bool replace_child(std::string& parent, const char* type, const std::string& child, size_t prefix = 0) {
    Atom root; if (!read_atom(parent, 0, parent.size(), root) || root.size != parent.size() || root.size-root.header < prefix) return false;
    std::string body = parent.substr(root.header, prefix); bool replaced = false;
    for (size_t at = root.header+prefix; at < parent.size();) {
        Atom a; if (!read_atom(parent, at, parent.size(), a)) return false;
        if (parent.compare(at+4, 4, type) == 0) { if (replaced) return false; body += child; replaced = true; }
        else body.append(parent, at, a.size);
        at += a.size;
    }
    if (!replaced) body += child;
    parent = atom(parent.substr(4,4).c_str(), body); return parent.size() <= metadata_limit;
}
inline bool update_moov(std::string& moov, const std::string& items) {
    std::string udta, meta, ilst;
    if (!child_atom(moov, "udta", udta)) return false;
    if (udta.empty()) udta = atom("udta", {});
    if (!child_atom(udta, "meta", meta)) return false;
    if (meta.empty()) meta = atom("meta", std::string(4, '\0'));
    std::string handler;
    if (!child_atom(meta, "hdlr", handler, 4)) return false;
    if (handler.empty()) {
        std::string body(8, '\0'); body += "mdir"; body.append(12, '\0'); body += '\0';
        if (!replace_child(meta, "hdlr", atom("hdlr", body), 4)) return false;
    } else {
        Atom h;
        if (!read_atom(handler, 0, handler.size(), h) || h.size-h.header < 24 || handler.compare(h.header+8, 4, "mdir") != 0) return false;
    }
    if (!child_atom(meta, "ilst", ilst, 4)) return false;
    if (ilst.empty()) ilst = atom("ilst", {});
    for (size_t p = 0; p < items.size();) {
        Atom a; if (!read_atom(items, p, items.size(), a)) return false;
        if (!replace_child(ilst, items.substr(p+4,4).c_str(), items.substr(p, a.size))) return false;
        p += a.size;
    }
    return replace_child(meta, "ilst", ilst, 4) && replace_child(udta, "meta", meta) && replace_child(moov, "udta", udta);
}
inline bool inject_mp4_tags(std::string& file, const std::string& items, std::string& err) {
    size_t off = 0, size = 0; MediaRanges media;
    if (!find_atom(file, 0, file.size(), "moov", off, size)) { err = "没有有效 moov"; return false; }
    for (size_t p = 0; p < file.size();) {
        Atom a; if (!read_atom(file, p, file.size(), a)) { err = "MP4 atom 越界"; return false; }
        const auto type = file.substr(p+4, 4);
        if ((type == "moov" && p != off) || type == "moof" || type == "mfra" || type == "sidx") { err = "不支持此 MP4 偏移结构"; return false; }
        if (type == "mdat") media.emplace_back(p+a.header, p+a.size);
        p += a.size;
    }
    auto moov = file.substr(off, size);
    if (media.empty() || !shift_chunk_offsets(moov, 0, moov.size(), 0, 0, &media) || !update_moov(moov, items) ||
        !shift_chunk_offsets(moov, 0, moov.size(), int64_t(moov.size())-int64_t(size), off+size, &media)) {
        err = "MP4 元数据或偏移校验失败，保留原文件"; return false;
    }
    file.replace(off, size, moov); return true;
}
inline bool verify_mp4(const std::string& file) {
    MediaRanges media; size_t moov = 0; unsigned count = 0;
    for (size_t p = 0; p < file.size();) {
        Atom a; if (!read_atom(file, p, file.size(), a)) return false;
        const auto type = file.substr(p+4,4);
        if (type == "mdat") media.emplace_back(p+a.header, p+a.size);
        if (type == "moov") { moov = p; ++count; }
        p += a.size;
    }
    if (count != 1 || media.empty()) return false;
    Atom a; if (!read_atom(file, moov, file.size(), a)) return false;
    auto bytes = file.substr(moov, a.size);
    return shift_chunk_offsets(bytes, 0, bytes.size(), 0, 0, &media);
}
inline bool embed_m4a(const std::string& path, const std::string& cover, bool is_png,
                      const std::string& lyrics, std::string& err, const Cancel& cancel = {}) {
    const auto size = platform::file_size(path); File input(platform::fopen(path, "rb"), &std::fclose);
    if (!input || size < 8) { err = "读文件失败"; return false; }
    uint64_t moov_at = 0, moov_size = 0; std::string moov; MediaRanges media;
    unsigned count = 0;
    for (uint64_t at = 0; at < uint64_t(size);) {
        if (++count > 65536 || (cancel && cancel()) || uint64_t(size)-at < 8 || !seek_file(input.get(), at)) { err = "MP4 结构无效或已取消"; return false; }
        std::string head(size_t(std::min<uint64_t>(16, uint64_t(size)-at)), '\0');
        if (std::fread(head.data(),1,head.size(),input.get()) != head.size()) { err = "读取 MP4 失败"; return false; }
        uint64_t n = rd32(head, 0), header = 8;
        if (n == 1) { if (head.size() < 16) { err = "MP4 扩展头截断或越界"; return false; } n = uint64_t(rd32(head,8)) << 32 | rd32(head,12); header = 16; }
        else if (!n) n = uint64_t(size)-at;
        if (n < header || n > uint64_t(size)-at) { err = "MP4 atom 越界"; return false; }
        const auto type = head.substr(4,4);
        if (type == "moof" || type == "mfra" || type == "sidx") { err = "暂不改写碎片化 MP4"; return false; }
        if (type == "mdat") media.emplace_back(at+header, at+n);
        if (type == "moov") {
            if (!moov.empty() || n > metadata_limit || !seek_file(input.get(),at)) { err = "moov 重复或过大"; return false; }
            moov_at = at; moov_size = n; moov.resize(size_t(n));
            if (std::fread(moov.data(),1,moov.size(),input.get()) != moov.size()) { err = "moov 截断"; return false; }
        }
        at += n;
    }
    input.reset();
    std::string items; if (!cover.empty()) items += covr_atom(cover,is_png); if (!lyrics.empty()) items += lyric_atom(lyrics);
    if (moov.empty() || media.empty() || !shift_chunk_offsets(moov,0,moov.size(),0,0,&media) || !update_moov(moov,items) ||
        !shift_chunk_offsets(moov,0,moov.size(),int64_t(moov.size())-int64_t(moov_size),moov_at+moov_size,&media)) {
        err = "MP4 元数据或偏移校验失败，保留原文件"; return false;
    }
    if (!write_replace_stream(path, [&](std::FILE* output) {
        File source(platform::fopen(path,"rb"), &std::fclose);
        return source && copy_bytes(source.get(),output,moov_at,cancel) &&
            std::fwrite(moov.data(),1,moov.size(),output) == moov.size() && seek_file(source.get(),moov_at+moov_size) &&
            copy_bytes(source.get(),output,uint64_t(size)-moov_at-moov_size,cancel);
    },cancel)) { err = "写回失败或已取消，原文件保留"; return false; }
    return true;
}

// 内容白名单：ID3 前缀不能证明是 MP3，还需检查标签后的 MPEG 帧头。
inline bool embed(const std::string& path, const std::string& cover, bool is_png, const std::string& lyrics,
                  std::string& err, const Cancel& cancel = {}) {
    err.clear();
    if (cover.empty() && lyrics.empty()) return true;
    if (cover.size() > 8u*1024*1024 || lyrics.size() > 4u*1024*1024) { err = "封面或歌词超过内嵌上限"; return false; }
    File input(platform::fopen(path, "rb"), &std::fclose);
    if (!input) { err = "读文件失败"; return false; }
    std::string head(12,'\0'); head.resize(std::fread(head.data(),1,head.size(),input.get())); input.reset();
    if (head.size() >= 8 && head.compare(4,4,"ftyp") == 0) return embed_m4a(path,cover,is_png,lyrics,err,cancel);
    if (head.compare(0,3,"ID3") == 0 || mpeg_audio_header(head)) return embed_mp3(path,cover,is_png,lyrics,err,cancel);
    err = "此音频容器暂不支持内嵌，保留原文件"; return false;
}

}  // namespace tag
