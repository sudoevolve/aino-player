// json.hpp —— 极简 JSON 解析器（单头文件，零依赖）
//
// 只做播放器需要的事：把接口返回的 JSON 解析成可查询的树。
// 特点：
//   * lenient 模式能吃下接口返回的单引号"JSON"（{'NAME':'花海',...}）
//   * \uXXXX 与 UTF-16 代理对都转 UTF-8
//   * 解析失败返回 false + 错误位置，不抛异常
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <cmath>
#include <limits>
#include <locale>
#include <sstream>

namespace js {

inline std::string quote(const std::string& text) {
    std::string out = "\""; const char* hex = "0123456789abcdef";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += char(c);
    }
    return out + '"';
}

inline bool finite_number(const std::string& text, double& value) {
    std::istringstream input(text); input.imbue(std::locale::classic());
    double parsed = 0;
    if (!(input >> parsed) || !std::isfinite(parsed)) return false;
    input >> std::ws;
    if (!input.eof()) return false;
    value = parsed; return true;
}

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value {
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    bool is_null() const { return type == Type::Null; }
    bool is_array() const { return type == Type::Array; }
    bool is_object() const { return type == Type::Object; }

    size_t size() const {
        if (type == Type::Array) return array.size();
        if (type == Type::Object) return object.size();
        return 0;
    }

    // 找不到返回一个共享的空值，调用方可以一路 .get() 下去而不判空
    static const Value& null_value() {
        static const Value v;
        return v;
    }

    const Value& operator[](const char* key) const {
        if (type != Type::Object) return null_value();
        for (const auto& kv : object)
            if (kv.first == key) return kv.second;
        return null_value();
    }
    const Value& operator[](size_t i) const {
        if (type != Type::Array || i >= array.size()) return null_value();
        return array[i];
    }
    const Value& at(size_t i) const { return (*this)[i]; }

    std::string str(const std::string& fallback = std::string()) const {
        return type == Type::String ? string : fallback;
    }
    double num(double fallback = 0.0) const {
        if (type == Type::Number) return std::isfinite(number) ? number : fallback;
        if (type == Type::String) {  // 有些接口把数字写成字符串
            double value = 0; return finite_number(string, value) ? value : fallback;
        }
        if (type == Type::Bool) return boolean ? 1.0 : 0.0;
        return fallback;
    }
    long long integer(long long fallback = 0) const {
        const double d = num(std::numeric_limits<double>::quiet_NaN());
        const double rounded = std::round(d);
        // LLONG_MAX 转成 double 会向上舍入到 2^63，故上界必须用严格小于。
        if (!std::isfinite(rounded) || rounded < -9223372036854775808.0 || rounded >= 9223372036854775808.0) return fallback;
        return static_cast<long long>(rounded);
    }
    int int_value(int fallback = 0) const {
        const auto value = integer(fallback);
        return value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max() ? fallback : int(value);
    }
    long long milliseconds(long long fallback = 0) const {
        const auto seconds = integer(-1);
        return seconds < 0 || seconds > std::numeric_limits<long long>::max()/1000 ? fallback : seconds*1000;
    }
    bool truthy() const {
        switch (type) {
            case Type::Bool: return boolean;
            case Type::Number: return number != 0.0;
            case Type::String: return !string.empty();
            case Type::Array: return !array.empty();
            case Type::Object: return !object.empty();
            default: return false;
        }
    }
};

namespace detail {

struct Parser {
    const char* begin;
    const char* p;
    const char* end;
    bool lenient = false;
    std::string error;
    int depth = 0;

    void skip_ws() {
        while (p < end) {
            const char c = *p;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++p; continue; }
            // 兼容接口响应中的 UTF-8 BOM。
            if ((lenient || p == begin) && (unsigned char)c == 0xEF && end - p >= 3 && (unsigned char)p[1] == 0xBB &&
                (unsigned char)p[2] == 0xBF) { p += 3; continue; }
            break;
        }
    }

    bool fail(const char* why) {
        if (error.empty()) error = std::string(why) + " @" + std::to_string(long(p - begin));
        return false;
    }

    static void append_utf8(std::string& out, uint32_t cp) {
        if (cp <= 0x7F) {
            out += char(cp);
        } else if (cp <= 0x7FF) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(uint32_t& out) {
        if (end - p < 4) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *p++;
            out <<= 4;
            if (c >= '0' && c <= '9') out |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') out |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= uint32_t(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool parse_string(std::string& out) {
        if (p >= end) return false;
        const char quote = *p;
        if (quote != '"' && !(lenient && quote == '\'')) return fail("string");
        ++p;
        out.clear();
        while (p < end) {
            const char c = *p++;
            if (c == quote) return true;
            if (c == '\\') {
                if (p >= end) return false;
                const char e = *p++;
                switch (e) {
                    case '"': out += '"'; break;
                    case '\'': if (!lenient) return fail("escape"); out += '\''; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        uint32_t cp = 0;
                        if (!hex4(cp)) return fail("\\u");
                        if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                            const char* save = p;
                            p += 2;
                            uint32_t lo = 0;
                            if (hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF)
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            else
                                p = save;
                        }
                        if (cp >= 0xD800 && cp <= 0xDFFF) {
                            if (!lenient) return fail("unpaired surrogate");
                            cp = 0xFFFD;
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default: if (!lenient) return fail("escape"); out += e; break;
                }
                continue;
            }
            if (!lenient && static_cast<unsigned char>(c) < 32) return fail("control character");
            if (!lenient && static_cast<unsigned char>(c) >= 128) {
                const auto first = static_cast<unsigned char>(c); unsigned count = 0; uint32_t cp = 0, minimum = 0;
                if (first >= 194 && first <= 223) { count = 1; cp = first & 31; minimum = 128; }
                else if (first >= 224 && first <= 239) { count = 2; cp = first & 15; minimum = 2048; }
                else if (first >= 240 && first <= 244) { count = 3; cp = first & 7; minimum = 65536; }
                else return fail("UTF-8 lead");
                if (size_t(end-p) < count) return fail("UTF-8 truncated");
                const auto* start = p-1;
                for (unsigned i = 0; i < count; ++i) {
                    const auto next = static_cast<unsigned char>(*p++);
                    if ((next & 192) != 128) return fail("UTF-8 continuation");
                    cp = (cp << 6) | (next & 63);
                }
                if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return fail("UTF-8 codepoint");
                out.append(start, p); continue;
            }
            out += c;
        }
        return fail("unterminated string");
    }

    bool parse_number(Value& v) {
        const char* start = p;
        const auto digit = [&] { return p < end && *p >= '0' && *p <= '9'; };
        if (p < end && (*p == '-' || (lenient && *p == '+'))) ++p;
        if (!digit()) return fail("number");
        if (*p == '0' && !lenient) ++p;
        else while (digit()) ++p;
        if (p < end && *p == '.') { ++p; if (!digit()) return fail("fraction"); while (digit()) ++p; }
        if (p < end && (*p == 'e' || *p == 'E')) {
            ++p; if (p < end && (*p == '-' || *p == '+')) ++p;
            if (!digit()) return fail("exponent"); while (digit()) ++p;
        }
        if (p < end && (*p == '.' || *p == 'e' || *p == 'E' || (*p >= '0' && *p <= '9'))) return fail("number suffix");
        if (!finite_number(std::string(start, p), v.number)) return fail("number range");
        v.type = Type::Number;
        return true;
    }

    bool parse_bare_word(Value& v) {
        const char* start = p;
        while (p < end) {
            const char c = *p;
            if (c == ',' || c == '}' || c == ']' || c == ':' || c == ' ' || c == '\t' ||
                c == '\r' || c == '\n')
                break;
            ++p;
        }
        const std::string word(start, p);
        if (word == "true") { v.type = Type::Bool; v.boolean = true; return true; }
        if (word == "false") { v.type = Type::Bool; v.boolean = false; return true; }
        if (word == "null" || (lenient && word == "None")) { v.type = Type::Null; return true; }
        if (word.empty()) return fail("empty value");
        if (lenient) {  // 裸字符串，当字符串收下
            v.type = Type::String;
            v.string = word;
            return true;
        }
        return fail("bare word");
    }

    bool parse_value(Value& v) {
        if (++depth > 200) return fail("too deep");
        skip_ws();
        if (p >= end) { --depth; return fail("eof"); }
        bool ok = false;
        const char c = *p;
        if (c == '{') {
            ++p;
            v.type = Type::Object;
            skip_ws();
            if (p < end && *p == '}') { ++p; --depth; return true; }
            while (p < end) {
                skip_ws();
                std::string key;
                if (!parse_string(key)) { --depth; return false; }
                skip_ws();
                if (p < end && *p == ':') ++p;
                else if (!lenient) { --depth; return fail("expect :"); }
                Value child;
                if (!parse_value(child)) { --depth; return false; }
                v.object.emplace_back(std::move(key), std::move(child));
                skip_ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; ok = true; break; }
                if (lenient && p < end) {  // 少逗号也认
                    continue;
                }
                --depth;
                return fail("object");
            }
            --depth;
            return ok || lenient;
        }
        if (c == '[') {
            ++p;
            v.type = Type::Array;
            skip_ws();
            if (p < end && *p == ']') { ++p; --depth; return true; }
            while (p < end) {
                Value child;
                if (!parse_value(child)) { --depth; return false; }
                v.array.push_back(std::move(child));
                skip_ws();
                if (p < end && *p == ',') { ++p; skip_ws(); continue; }
                if (p < end && *p == ']') { ++p; ok = true; break; }
                if (lenient && p < end) continue;
                --depth;
                return fail("array");
            }
            --depth;
            return ok || lenient;
        }
        if (c == '"' || c == '\'') {
            v.type = Type::String;
            ok = parse_string(v.string);
            --depth;
            return ok;
        }
        if ((c >= '0' && c <= '9') || c == '-' || c == '+') {
            ok = parse_number(v);
            --depth;
            return ok;
        }
        ok = parse_bare_word(v);
        --depth;
        return ok;
    }
};

}  // namespace detail

// 解析整段文本；lenient = 容忍单引号 / 裸词 / 缺逗号
inline bool parse(const std::string& text, Value& out, bool lenient = false, std::string* error = nullptr) {
    detail::Parser ps;
    ps.begin = text.data();
    ps.p = text.data();
    ps.end = text.data() + text.size();
    ps.lenient = lenient;
    out = Value();
    bool ok = ps.parse_value(out);
    ps.skip_ws();
    if (ok && !lenient && ps.p != ps.end) ok = ps.fail("trailing input");
    if (!ok) out = Value();
    if (error) *error = ps.error;
    return ok;
}

}  // namespace js
