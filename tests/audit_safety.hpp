#pragma once
#include "../src/tag.hpp"
#include "../src/json.hpp"
#include "../src/lyrics.hpp"
#include <stdexcept>

// 离线、独占创建的 fixtures；只在显式发布回归中由 player_tests 调用。
inline void audit_safety_checks(const std::string& directory) {
    const auto require = [](bool ok, const char* why) { if (!ok) throw std::runtime_error(why); };
    js::Value value;
    for (const auto* invalid : {"{}garbage", "1.2.3", "01", "+1", "1e", "1.", "{'a':1}", "None", "\"\\q\"", "\"\\uD800\"", "[1,]", "{\"a\":}", "1e9999"})
        require(!js::parse(invalid, value), "strict JSON rejects invalid complete documents");
    require(js::parse("{'a':None 'b':'ok'}", value, true), "explicit lenient network compatibility remains");
    require(js::parse("\"\\uD83D\\uDE00\"", value) && value.str().size() == 4, "strict JSON surrogate pair");
    require(!js::parse(std::string("\"\xc0\x80\"",4), value), "strict JSON rejects overlong UTF-8");
    value.type = js::Type::Number;
    for (double number : {1e100, -1e100, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(), 9223372036854775808.0}) {
        value.number = number; require(value.integer(17) == 17, "invalid integer conversion returns exact fallback");
    }
    value.number = 1.5; require(value.integer() == 2, "integer rounding compatibility");
    value.number = -1.5; require(value.integer() == -2, "negative integer rounding compatibility");
    value.type = js::Type::String; value.string = "12junk";
    require(value.num(7) == 7, "numeric strings require complete consumption");
    value.string = "9223372036854776"; require(value.milliseconds(9) == 9, "seconds conversion cannot overflow");
    const auto stamp = [](const std::string& text) { return lyr::parse_stamp(text,0,text.size()); };
    require(stamp("01:02.5") == 62500 && stamp("01:02:53") == 62530 && stamp("01:02.532999") == 62532, "LRC fractions use identical decimal scaling");
    for (const auto* invalid : {"999999999999999999999999:01", "01:60", "01::2", "01.2", ":02", "01:02.", "01:02.2.3"})
        require(stamp(invalid) == -1, "LRC rejects invalid timestamps without overflow");
    require(lyr::parse_stamp("00:01",0,99) == -1, "LRC bounds checked");
    const auto doc = lyr::from_lrc("plain\n[00:01]first\n[00:01]last\n[00:03]next", "test");
    require(lyr::current_line(doc.lines,999) == -1 && lyr::current_line(doc.lines,1000) == 2 && lyr::current_line(doc.lines,3000) == 3, "lyric binary search retains duplicate and unsynced semantics");
    require(!net::safe_request_headers("http://music.163.com/", {"cOoKiE: token"}) &&
        !net::safe_request_headers("http://example.test/", {"Authorization: Bearer token"}) &&
        !net::safe_request_headers("https://example.test/", {"X-Test: ok\r\nCookie: token"}) &&
        net::safe_request_headers("http://127.0.0.1:1234/", {"Range: bytes=0-3"}) &&
        net::safe_request_headers("https://music.163.com/", {"Cookie: token"}) &&
        net::safe_request_headers("http://script.example/", {"Cookie: script-token"}, true) &&
        !net::safe_request_headers("https://example.test/", {" Cookie: token"}) &&
        !net::safe_request_headers("https://example.test/", {"X-Test: ok\r\nCookie: token"}, true),
        "host credential transport, script compatibility and header syntax policy");
    require(net::cookie_value("a=first; a=second; empty=; token=a=b", "a") == "first" &&
        net::cookie_value("token=a=b", "token") == "a=b", "shared cookie parsing retains first-match semantics");
    require(music::providers::Md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e" &&
        music::providers::Md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72", "shared MD5 known answers");
    struct Fixtures {
        std::vector<std::string> files;
        ~Fixtures() { for (const auto& path : files) platform::remove(path); }
    } fixtures;
    const auto fixture = [&](const std::string& bytes) {
        const auto path = platform::join(directory,"audit-"+std::to_string(fixtures.files.size())+".audio");
        tag::File file(platform::fopen(path,"wbx"), &std::fclose); require(bool(file),"exclusive audit fixture");
        fixtures.files.push_back(path);
        require(std::fwrite(bytes.data(),1,bytes.size(),file.get()) == bytes.size(),"audit fixture write");
        return path;
    };
    std::string error, after;
    const std::string audio = std::string("\xff\xfb\x90\x64",4) + std::string(1024,'a');
    const auto title = tag::id3_frame("TIT2",std::string(1,char(3))+"original");
    std::string id3("ID3\x03\0\0",6); tag::put_synchsafe(id3,uint32_t(title.size())); id3 += title;
    const auto mp3 = fixture(id3+audio);
    require(tag::embed(mp3,"cover",false,"lyrics",error),"MP3 embeds safely");
    require(tag::read_file(mp3,after) && after.find(title) != std::string::npos && after.substr(after.size()-audio.size()) == audio,"MP3 preserves original text and audio bytes");
    const auto preserved = after;
    for (unsigned version : {2u, 4u}) {
        const auto payload = std::string(1, '\0') + "original";
        std::string frame = version == 2 ? "TT2" : "TIT2";
        if (version == 2) { frame.append(2, '\0'); frame += char(payload.size()); }
        else { tag::put_synchsafe(frame, uint32_t(payload.size())); frame.append(2, '\0'); }
        frame += payload;
        std::string header = "ID3"; header += char(version); header += '\0'; header += char(version == 4 ? 16 : 0);
        tag::put_synchsafe(header, uint32_t(frame.size()));
        const auto footer = version == 4 ? "3DI" + header.substr(3) : std::string();
        const auto path = fixture(header + frame + footer + audio);
        require(tag::embed(path,"cover",false,"lyrics",error) && tag::read_file(path,after) &&
            after.find(frame) != std::string::npos && after.substr(after.size()-audio.size()) == audio,
            "ID3v2.2/v2.4 preserve original text and audio while replacing selected tags");
        if (version == 4) {
            const auto broken = header + frame + std::string(10, '?') + audio;
            const auto invalid = fixture(broken);
            require(!tag::embed(invalid,"cover",false,"",error) && tag::read_file(invalid,after) && after == broken,
                "invalid ID3 footer cannot remove audio bytes");
        }
    }
    require(!tag::write_replace_stream(mp3,[](std::FILE*) { return false; }) && tag::read_file(mp3,after) && after == preserved,"failed writer preserves original");
    require(!tag::embed(mp3,"new",false,"",error,[] { return true; }) && tag::read_file(mp3,after) && after == preserved,"cancelled tagging preserves original");
    for (const auto& bytes : {std::string("OggScontainer"), std::string("RIFF1234WAVEdata"), std::string("\xff\xf1\x50\x80",4)+"aac", id3+"OggScontainer"}) {
        const auto path = fixture(bytes);
        require(!tag::embed(path,"cover",false,"",error) && tag::read_file(path,after) && after == bytes,"unsupported containers remain byte-identical");
    }
    const auto ftyp = tag::atom("ftyp","M4A ");
    const auto mdat = tag::atom("mdat",std::string(64,'s'));
    for (bool before : {false,true}) for (bool wide : {false,true}) for (int layout = 0; layout < 4; ++layout) {
        const auto moov = [&](uint64_t offset) {
            std::string table(4,'\0'); tag::be32(table,1);
            if (wide) tag::be32(table,uint32_t(offset >> 32)); tag::be32(table,uint32_t(offset));
            auto body = tag::atom("trak",tag::atom("mdia",tag::atom("minf",tag::atom("stbl",tag::atom(wide ? "co64" : "stco",table)))));
            if (layout == 1) body += tag::atom("udta",{});
            if (layout >= 2) body += tag::atom("udta",tag::atom("meta",std::string(4,'\0')+(layout == 3 ? tag::atom("ilst",tag::atom("name",tag::data_atom(1,"keep"))) : "")));
            return tag::atom("moov",body);
        };
        const auto original = ftyp + (before ? moov(ftyp.size()+moov(0).size()+8)+mdat : mdat+moov(ftyp.size()+8));
        const auto path = fixture(original);
        require(tag::embed(path,"picture",false,"lyric",error),"classic MP4 layouts embed safely");
        require(tag::read_file(path,after) && tag::verify_mp4(after),"MP4 offsets point into the relocated mdat");
        size_t moov_at = 0, moov_size = 0; std::string udta, meta, handler;
        require(tag::find_atom(after,0,after.size(),"moov",moov_at,moov_size) &&
            tag::child_atom(after.substr(moov_at,moov_size),"udta",udta) &&
            tag::child_atom(udta,"meta",meta) && tag::child_atom(meta,"hdlr",handler,4) &&
            meta.size() >= 12 && tag::rd32(meta,8) == 0 && handler.size() >= 33 &&
            handler.compare(16,4,"mdir") == 0, "MP4 metadata contains a real FullBox and mdir handler atom");
        require(tag::read_metadata(path).cover == "picture" && after.find(std::string(64,'s')) != std::string::npos,"MP4 metadata readable and media retained");
        const auto first_size = after.size();
        require(tag::embed(path,"picture",false,"lyric",error) && tag::read_file(path,after) && after.size() == first_size,"MP4 replaces selected metadata without unbounded duplicate growth");
    }
    std::string malformed; tag::be32(malformed,1); malformed += "free"; malformed.append(8,'\0');
    size_t off = 0, size = 0;
    require(!tag::find_atom(malformed,0,malformed.size(),"moov",off,size),"zero extended atom makes finite progress");
    std::string table(4,'\0'); tag::be32(table,UINT32_MAX);
    auto bad = tag::atom("moov",tag::atom("stco",table));
    require(!tag::shift_chunk_offsets(bad,0,bad.size(),1),"chunk count bounded to its own table");
    table.assign(4,'\0'); tag::be32(table,1); tag::be32(table,UINT32_MAX);
    bad = tag::atom("moov",tag::atom("stco",table));
    require(!tag::shift_chunk_offsets(bad,0,bad.size(),1),"stco overflow rejected before publication");
    std::vector<music::Track> tracks; std::map<std::string,size_t> index;
    music::Variant variant; variant.title = "Song"; variant.artist = "Artist"; variant.duration_ms = 180000; variant.id = "low"; variant.bitrate = 128000;
    music::detail::merge_into(tracks,index,variant); variant.id = "high"; variant.bitrate = 320000;
    music::detail::merge_into(tracks,index,variant);
    require(tracks.size() == 1 && tracks[0].variants.size() == 1 && tracks[0].current().id == "high","merge retains one variant per platform");
}
