// json.cpp — 极简 JSON 实现
#include "json.h"
#include <stdexcept>
#include <sstream>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace mvdb {

void Json::set(const std::string& k, Json v) {
    for (auto& kv : obj) if (kv.first == k) { kv.second = std::move(v); return; }
    obj.emplace_back(k, std::move(v));
}
const Json* Json::get(const std::string& k) const {
    for (auto& kv : obj) if (kv.first == k) return &kv.second;
    return nullptr;
}

namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;
    explicit Parser(const std::string& t) : s(t) {}

    void ws() { while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) i++; }
    char peek() { return i < s.size() ? s[i] : '\0'; }

    [[noreturn]] void fail(const std::string& m) {
        throw std::runtime_error("json parse error at " + std::to_string(i) + ": " + m);
    }

    void expect(char c) {
        if (i >= s.size() || s[i] != c) fail(std::string("expected '") + c + "'");
        i++;
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (i < s.size()) {
            unsigned char c = (unsigned char)s[i++];
            if (c == '"') return out;
            if (c == '\\') {
                if (i >= s.size()) fail("bad escape");
                char e = s[i++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        if (i + 4 > s.size()) fail("bad \\u");
                        unsigned cp = 0;
                        for (int k = 0; k < 4; k++) {
                            char h = s[i++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                            else fail("bad hex");
                        }
                        // 代理对
                        if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size()
                            && s[i] == '\\' && s[i+1] == 'u') {
                            i += 2;
                            unsigned lo = 0;
                            for (int k = 0; k < 4; k++) {
                                char h = s[i++];
                                lo <<= 4;
                                if (h >= '0' && h <= '9') lo |= (h - '0');
                                else if (h >= 'a' && h <= 'f') lo |= (h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') lo |= (h - 'A' + 10);
                                else fail("bad hex2");
                            }
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        // UTF-8 编码
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else if (cp < 0x10000) {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else {
                            out += (char)(0xF0 | (cp >> 18));
                            out += (char)(0x80 | ((cp >> 12) & 0x3F));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: fail("bad escape char");
                }
            } else {
                out += (char)c;
            }
        }
        fail("unterminated string");
    }

    Json parse_number() {
        size_t start = i;
        if (peek() == '-') i++;
        while (i < s.size() && (isdigit((unsigned char)s[i]))) i++;
        bool is_int = true;
        if (peek() == '.') { is_int = false; i++; while (i < s.size() && isdigit((unsigned char)s[i])) i++; }
        if (peek() == 'e' || peek() == 'E') {
            is_int = false; i++;
            if (peek() == '+' || peek() == '-') i++;
            while (i < s.size() && isdigit((unsigned char)s[i])) i++;
        }
        std::string tok = s.substr(start, i - start);
        if (tok.empty()) fail("bad number");
        Json j;
        j.type = Json::Type::NUMBER;
        if (is_int) {
            errno = 0;
            long long v = strtoll(tok.c_str(), nullptr, 10);
            if (errno == ERANGE) { // 超 long long, 退化为 double
                j.is_integer = false;
                j.num = strtod(tok.c_str(), nullptr);
            } else {
                j.is_integer = true;
                j.inum = (int64_t)v;
                j.num = (double)v;
            }
        } else {
            j.is_integer = false;
            j.num = strtod(tok.c_str(), nullptr);
        }
        return j;
    }

    Json parse_value() {
        ws();
        char c = peek();
        if (c == '{') {
            i++;
            Json j = Json::make_obj();
            ws();
            if (peek() == '}') { i++; return j; }
            while (true) {
                ws();
                std::string k = parse_string();
                ws(); expect(':');
                Json v = parse_value();
                j.set(k, std::move(v));
                ws();
                if (peek() == ',') { i++; continue; }
                expect('}');
                return j;
            }
        } else if (c == '[') {
            i++;
            Json j = Json::make_arr();
            ws();
            if (peek() == ']') { i++; return j; }
            while (true) {
                Json v = parse_value();
                j.push(std::move(v));
                ws();
                if (peek() == ',') { i++; continue; }
                expect(']');
                return j;
            }
        } else if (c == '"') {
            return Json::make_str(parse_string());
        } else if (c == 't') {
            if (s.compare(i, 4, "true") != 0) fail("bad true");
            i += 4; return Json::make_bool(true);
        } else if (c == 'f') {
            if (s.compare(i, 5, "false") != 0) fail("bad false");
            i += 5; return Json::make_bool(false);
        } else if (c == 'n') {
            if (s.compare(i, 4, "null") != 0) fail("bad null");
            i += 4; return Json::make_null();
        } else if (c == '-' || isdigit((unsigned char)c)) {
            return parse_number();
        }
        fail("unexpected char");
    }

    Json parse_top() {
        Json v = parse_value();
        ws();
        if (i != s.size()) fail("trailing characters");
        return v;
    }
};

void dump_string(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
}

void dump_num(std::string& out, double v) {
    if (!std::isfinite(v)) { out += "null"; return; }
    char buf[64];
    // %.17g 保证 round-trip; 但距离输出希望简洁, 用 %.10g
    snprintf(buf, sizeof(buf), "%.10g", v);
    out += buf;
}

void dump_value(std::string& out, const Json& j) {
    switch (j.type) {
        case Json::Type::NUL: out += "null"; break;
        case Json::Type::BOOL: out += j.b ? "true" : "false"; break;
        case Json::Type::NUMBER:
            if (j.is_integer) out += std::to_string(j.inum);
            else dump_num(out, j.num);
            break;
        case Json::Type::STRING: dump_string(out, j.str); break;
        case Json::Type::ARRAY: {
            out += '[';
            for (size_t k = 0; k < j.arr.size(); k++) {
                if (k) out += ',';
                dump_value(out, j.arr[k]);
            }
            out += ']';
            break;
        }
        case Json::Type::OBJECT: {
            out += '{';
            for (size_t k = 0; k < j.obj.size(); k++) {
                if (k) out += ',';
                dump_string(out, j.obj[k].first);
                out += ':';
                dump_value(out, j.obj[k].second);
            }
            out += '}';
            break;
        }
    }
}

} // namespace

Json Json::parse(const std::string& text) {
    Parser p(text);
    return p.parse_top();
}

std::string Json::dump() const {
    std::string out;
    out.reserve(256);
    dump_value(out, *this);
    return out;
}

} // namespace mvdb
