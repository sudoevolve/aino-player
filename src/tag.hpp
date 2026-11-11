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

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), nullptr, 0);
    if (n <= 0) return out;
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), w.data(), n);
    out += char(0xFF);
    out += char(0xFE);
    for (wchar_t c : w) {
        out += char(c & 0xFF);
        out += char((c >> 8) & 0xFF);
    }
    return out;
}

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

inline bool read_file(const std::string& path, std::string& out) {
    std::FILE* f = _wfopen(widen(path).c_str(), L"rb");
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
    std::FILE* f = _wfopen(widen(tmp).c_str(), L"wb");
    if (!f) return false;
    const size_t n = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (n != bytes.size()) {
        DeleteFileW(widen(tmp).c_str());
        return false;
    }
    if (!MoveFileExW(widen(tmp).c_str(), widen(path).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(widen(tmp).c_str());
        return false;
    }
    return true;
}

inline std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

inline bool read_file_w(const std::wstring& wpath, std::string& out) {
    std::FILE* f = _wfopen(wpath.c_str(), L"rb");
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
        std::FILE* f = _wfopen(widen(path).c_str(), L"rb");
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
