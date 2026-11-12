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

namespace tag {

// ---------------------------------------------------------------- 小工具

inline void be32(std::string& s, uint32_t v) {
    s += char((v >> 24) & 0xFF);
    s += char((v >> 16) & 0xFF);
    s += char((v >> 8) & 0xFF);
    s += char(v & 0xFF);
}

inline uint32_t rd32(const std::string& s, size_t at) {
    if (at + 4 > s.size()) return 0;
    return (uint32_t(uint8_t(s[at])) << 24) | (uint32_t(uint8_t(s[at + 1])) << 16) |
           (uint32_t(uint8_t(s[at + 2])) << 8) | uint32_t(uint8_t(s[at + 3]));
}

inline void wr32(std::string& s, size_t at, uint32_t v) {
    if (at + 4 > s.size()) return;
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
    for (wchar_t c : w) {
        out += char(c & 0xFF);
        out += char((c >> 8) & 0xFF);
    }
    return out;
}

inline std::wstring widen(const std::string& s) { return platform::widen(s); }

inline bool read_file(const std::string& path, std::string& out) {
    std::FILE* f = platform::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        std::fclose(f);
        return false;
    }
    out.resize(size_t(n));
    const size_t got = std::fread(&out[0], 1, size_t(n), f);
    std::fclose(f);
    return got == size_t(n);
}

// 写临时文件再整体替换：中途失败也不会留下半个损坏的音频
inline bool write_replace(const std::string& path, const std::string& bytes) {
    const std::string tmp = path + ".tagtmp";
    std::FILE* f = platform::fopen(tmp, "wb");
    if (!f) return false;
    const size_t n = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (n != bytes.size()) {
        platform::remove(tmp);
        return false;
    }
    if (!platform::replace_file(tmp, path)) {
        platform::remove(tmp);
        return false;
    }
    return true;
}

inline std::string narrow(const std::wstring& s) { return platform::narrow(s); }

inline bool read_file_w(const std::wstring& wpath, std::string& out) {
    std::FILE* f = platform::fopen(narrow(wpath), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        std::fclose(f);
        return false;
    }
    out.resize(size_t(n));
    const size_t got = std::fread(&out[0], 1, size_t(n), f);
    std::fclose(f);
    return got == size_t(n);
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

inline bool embed_mp3(const std::string& path, const std::string& cover, bool is_png,
                      const std::string& lyrics, std::string& err) {
    std::string all;
    if (!read_file(path, all)) {
        err = "读文件失败";
        return false;
    }
    // 丢掉原来的 ID3v2 标签（否则会出现两个标签，播放器取第一个）
    size_t audio_at = 0;
    if (all.size() > 10 && all.compare(0, 3, "ID3") == 0) {
        const size_t a = size_t(uint8_t(all[6])), b = size_t(uint8_t(all[7]));
        const size_t c = size_t(uint8_t(all[8])), d = size_t(uint8_t(all[9]));
        const size_t sz = (a << 21) | (b << 14) | (c << 7) | d;  // synchsafe
        audio_at = 10 + sz;
        if ((all[5] & 0x10) != 0) audio_at += 10;  // footer
        if (audio_at > all.size()) audio_at = 0;
    }

    std::string frames;
    if (!cover.empty()) {
        std::string apic;
        apic += char(0);  // encoding: ISO-8859-1（mime/描述都是 ASCII）
        const char* mime = is_png ? "image/png" : "image/jpeg";
        apic += mime;
        apic += char(0);
        apic += char(3);  // 图片类型：3 = 正面封面
        apic += char(0);  // 空描述
        apic += cover;
        frames += id3_frame("APIC", apic);
    }
    if (!lyrics.empty()) {
        std::string uslt;
        uslt += char(1);  // encoding：UTF-16 with BOM
        uslt += "chi";
        uslt += char(0);  // 空描述：UTF-16 下是 4 字节对齐的 0，这里给两个 0 就够
        uslt += char(0);
        uslt += utf16le_bom(lyrics);
        frames += id3_frame("USLT", uslt);
    }
    if (frames.empty()) return true;

    std::string out = "ID3";
    out += char(3);  // v2.3
    out += char(0);
    out += char(0);  // flags
    const size_t n = frames.size();
    out += char((n >> 21) & 0x7F);
    out += char((n >> 14) & 0x7F);
    out += char((n >> 7) & 0x7F);
    out += char(n & 0x7F);
    out += frames;
    out.append(all, audio_at, std::string::npos);

    if (!write_replace(path, out)) {
        err = "写回文件失败（被占用？）";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- m4a / MP4 atoms

// 在 [begin, end) 里找指定 type 的 atom，返回偏移与大小（注意 size==1 的 64 位变体）
inline bool find_atom(const std::string& s, size_t begin, size_t end, const char* type, size_t& off, size_t& size) {
    size_t p = begin;
    while (p + 8 <= end) {
        uint32_t sz = rd32(s, p);
        if (s.compare(p + 4, 4, type) == 0) {
            size_t real = sz;
            if (sz == 1) {
                if (p + 16 > end) return false;
                real = (size_t(rd32(s, p + 8)) << 32) | rd32(s, p + 12);
            } else if (sz == 0) {
                real = end - p;
            }
            if (real < 8 || p + real > end) return false;
            off = p;
            size = real;
            return true;
        }
        if (sz == 0) return false;
        if (sz == 1) {
            if (p + 16 > end) return false;
            p += (size_t(rd32(s, p + 8)) << 32) | rd32(s, p + 12);
        } else {
            p += sz;
        }
    }
    return false;
}

inline std::string atom(const char* type, const std::string& body) {
    std::string a;
    be32(a, uint32_t(body.size() + 8));
    a.append(type, 4);
    a += body;
    return a;
}

// data atom：version/flags(4) + locale(4) + 负载
inline std::string data_atom(uint32_t type, const std::string& payload) {
    std::string body;
    be32(body, type);
    be32(body, 0);
    body += payload;
    return atom("data", body);
}

inline std::string covr_atom(const std::string& cover, bool is_png) {
    return atom("covr", data_atom(is_png ? 14u : 13u, cover));
}

inline std::string lyric_atom(const std::string& lyrics) {
    return atom("\xA9lyr", data_atom(1u, lyrics));  // ©lyr
}

// moov 变大 → 后面的 mdat 整体后移，所有 stco/co64 的 chunk 偏移都要加 delta。
// 要**递归**找：stco 藏在 trak/mdia/minf/stbl 里，不是 moov 的直接子节点。
inline void shift_chunk_offsets(std::string& file, size_t begin, size_t end, int64_t delta) {
    size_t p = begin;
    while (p + 8 <= end && p + 8 <= file.size()) {
        uint32_t sz = rd32(file, p);
        if (sz == 0) sz = uint32_t(end - p);
        if (sz < 8 || p + sz > end) return;
        const std::string type = file.substr(p + 4, 4);
        if ((type == "stco" || type == "co64") && p + 16 <= file.size()) {
            const uint32_t n = rd32(file, p + 12);
            const size_t step = type == "stco" ? 4 : 8;
            for (uint32_t i = 0; i < n; ++i) {
                const size_t at = p + 16 + size_t(i) * step;
                if (at + step > file.size()) break;
                if (step == 4) {
                    wr32(file, at, uint32_t(int64_t(rd32(file, at)) + delta));
                } else {
                    const uint64_t v = (uint64_t(rd32(file, at)) << 32) | rd32(file, at + 4);
                    const uint64_t nv = uint64_t(int64_t(v) + delta);
                    wr32(file, at, uint32_t(nv >> 32));
                    wr32(file, at + 4, uint32_t(nv & 0xFFFFFFFFu));
                }
            }
        } else if (type == "trak" || type == "mdia" || type == "minf" || type == "stbl" || type == "edts" ||
                   type == "udta" || type == "moov") {
            shift_chunk_offsets(file, p + 8, p + sz, delta);
        }
        p += sz;
    }
}

inline const char* kIlstHdlr =
    "\x00\x00\x00\x00"                    // version/flags
    "\x00\x00\x00\x00"                    // predefined
    "mdirappl"                            // handler type
    "\x00\x00\x00\x00\x00\x00\x00\x00"    // reserved
    "\x00";                               // name（空，以 0 结尾）

// 把 ilst 条目塞进 moov。结构缺失就补出来（udta / meta / ilst 层层新建）。
inline bool inject_mp4_tags(std::string& file, const std::string& items, std::string& err) {
    size_t moov_off = 0, moov_size = 0;
    if (!find_atom(file, 0, file.size(), "moov", moov_off, moov_size)) {
        err = "没有 moov";
        return false;
    }
    const size_t moov_end = moov_off + moov_size;

    size_t udta_off = 0, udta_size = 0;
    const bool has_udta = find_atom(file, moov_off + 8, moov_end, "udta", udta_off, udta_size);

    size_t insert_at = 0;  // 插入位置
    if (!has_udta) {
        // 直接造一个完整的 udta(meta(hdlr, ilst(items))) 追加到 moov 末尾
        std::string ilst = atom("ilst", items);
        std::string meta_body;
        meta_body += "\x00\x00\x00\x00";  // version/flags
        meta_body.append(kIlstHdlr, 25);
        meta_body += ilst;
        std::string udta = atom("udta", atom("meta", meta_body));
        insert_at = moov_end;
        file.insert(insert_at, udta);
        const uint32_t grew = uint32_t(udta.size());
        wr32(file, moov_off, uint32_t(moov_size + grew));
        // moov 变大 → 后面的 mdat 位移，chunk 偏移要跟着改
        if (moov_end <= file.size()) shift_chunk_offsets(file, moov_off, moov_off + moov_size + grew, int64_t(grew));
        return true;
    }

    const size_t udta_end = udta_off + udta_size;
    size_t meta_off = 0, meta_size = 0;
    if (!find_atom(file, udta_off + 8, udta_end, "meta", meta_off, meta_size)) {
        std::string ilst = atom("ilst", items);
        std::string meta_body;
        meta_body += "\x00\x00\x00\x00";
        meta_body.append(kIlstHdlr, 25);
        meta_body += ilst;
        const std::string meta = atom("meta", meta_body);
        insert_at = udta_end;
        file.insert(insert_at, meta);
        const uint32_t grew = uint32_t(meta.size());
        wr32(file, udta_off, uint32_t(udta_size + grew));
        wr32(file, moov_off, uint32_t(moov_size + grew));
        shift_chunk_offsets(file, moov_off, moov_off + moov_size + grew, int64_t(grew));
        return true;
    }

    // meta 的头 4 字节是 version/flags，子 atom 从 +12 开始
    const size_t meta_body = meta_off + 12;
    const size_t meta_end = meta_off + meta_size;
    size_t ilst_off = 0, ilst_size = 0;
    if (!find_atom(file, meta_body, meta_end, "ilst", ilst_off, ilst_size)) {
        insert_at = meta_end;
        file.insert(insert_at, atom("ilst", items));
        const uint32_t grew = uint32_t(8 + items.size());
        wr32(file, meta_off, uint32_t(meta_size + grew));
        wr32(file, udta_off, uint32_t(udta_size + grew));
        wr32(file, moov_off, uint32_t(moov_size + grew));
        shift_chunk_offsets(file, moov_off, moov_off + moov_size + grew, int64_t(grew));
        return true;
    }

    insert_at = ilst_off + ilst_size;
    file.insert(insert_at, items);
    const uint32_t grew = uint32_t(items.size());
    wr32(file, ilst_off, uint32_t(ilst_size + grew));
    wr32(file, meta_off, uint32_t(meta_size + grew));
    wr32(file, udta_off, uint32_t(udta_size + grew));
    wr32(file, moov_off, uint32_t(moov_size + grew));
    shift_chunk_offsets(file, moov_off, moov_off + moov_size + grew, int64_t(grew));
    return true;
}

// 落盘前自检：顶层 atom 要首尾相接铺满整个文件。过不了就不写，
// 宁可放弃内嵌，也不能产出打不开的文件。
inline bool verify_mp4(const std::string& file) {
    size_t p = 0;
    while (p + 8 <= file.size()) {
        const uint32_t sz = rd32(file, p);
        if (sz < 8 || p + sz > file.size()) return false;
        p += sz;
    }
    return p == file.size();
}

inline bool embed_m4a(const std::string& path, const std::string& cover, bool is_png,
                      const std::string& lyrics, std::string& err) {
    std::string all;
    if (!read_file(path, all)) {
        err = "读文件失败";
        return false;
    }
    std::string items;
    if (!cover.empty()) items += covr_atom(cover, is_png);
    if (!lyrics.empty()) items += lyric_atom(lyrics);
    if (items.empty()) return true;

    std::string work = all;
    if (!inject_mp4_tags(work, items, err)) return false;
    if (!verify_mp4(work)) {
        err = "m4a 结构校验没过，放弃内嵌";
        return false;
    }
    if (!write_replace(path, work)) {
        err = "写回文件失败（被占用？）";
        return false;
    }
    return true;
}

// 按文件内容判格式，别看扩展名（m4a 下载下来也常被存成 .mp3）
inline bool embed(const std::string& path, const std::string& cover, bool is_png, const std::string& lyrics,
                  std::string& err) {
    if (cover.empty() && lyrics.empty()) return true;
    std::string head;
    {
        std::FILE* f = platform::fopen(path, "rb");
        if (!f) {
            err = "读文件失败";
            return false;
        }
        char b[12] = {0};
        const size_t n = std::fread(b, 1, sizeof b, f);
        std::fclose(f);
        head.assign(b, n);
    }
    if (head.size() >= 8 && head.compare(4, 4, "ftyp") == 0) return embed_m4a(path, cover, is_png, lyrics, err);
    if (head.size() >= 4 && head.compare(0, 4, "fLaC") == 0) {
        err = "flac 暂不支持内嵌";
        return false;
    }
    return embed_mp3(path, cover, is_png, lyrics, err);
}

}  // namespace tag
