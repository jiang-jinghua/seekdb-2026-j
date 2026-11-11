#include "json.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace json {

void skip_ws(const char*& p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
}

static void utf8_from_codepoint(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
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
}

static uint32_t hex4(const char*& p, const char* end) {
    if (p + 4 > end) throw JsonParseError("bad \\u escape");
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else { throw JsonParseError("bad \\u escape"); }
    }
    p += 4;
    return v;
}

std::string parse_string(const char*& p, const char* end) {
    if (p >= end || *p != '"') throw JsonParseError("expected string");
    ++p;
    std::string out;
    while (p < end) {
        char c = *p++;
        if (c == '"') return out;
        if (c == '\\') {
            if (p >= end) throw JsonParseError("bad escape");
            char e = *p++;
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
                    uint32_t cp = hex4(p, end);
                    if (cp >= 0xD800 && cp <= 0xDBFF && p + 1 < end && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        uint32_t lo = hex4(p, end);
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else
                            utf8_from_codepoint(out, cp), cp = lo; // lone surrogate: emit, then treat next as its own
                        // note: if next wasn't a low surrogate we emit it separately below
                        if (cp >= 0xD800 && cp <= 0xDFFF) { utf8_from_codepoint(out, cp); break; }
                    }
                    utf8_from_codepoint(out, cp);
                    break;
                }
                default: throw JsonParseError("bad escape char");
            }
            continue;
        }
        if ((unsigned char)c < 0x20) throw JsonParseError("raw control char in string");
        out += c; // UTF-8 passthrough
    }
    throw JsonParseError("unterminated string");
}

void expect_lit(const char*& p, const char* end, const char* lit) {
    size_t n = std::strlen(lit);
    if ((size_t)(end - p) < n || std::memcmp(p, lit, n) != 0)
        throw JsonParseError(std::string("expected ") + lit);
    p += n;
}

// capture raw JSON text of a value starting at p (for id echo)
static std::string capture_value_raw(const char*& p, const char* end) {
    const char* start = p;
    skip_ws(p, end);
    start = p;
    if (p < end && *p == '"') {
        parse_string(p, end);
    } else if (p < end && (*p == '-' || std::isdigit((unsigned char)*p))) {
        if (*p == '-') ++p;
        while (p < end && (std::isdigit((unsigned char)*p) || *p == '.' || *p == 'e' || *p == 'E' ||
                           *p == '+' || *p == '-')) ++p;
    } else if (p < end && *p == '{') {
        int depth = 0;
        while (p < end) {
            char c = *p;
            if (c == '"') { parse_string(p, end); continue; }
            if (c == '{') ++depth;
            if (c == '}') { --depth; ++p; if (depth == 0) break; continue; }
            ++p;
        }
    } else if (p < end && *p == '[') {
        int depth = 0;
        while (p < end) {
            char c = *p;
            if (c == '"') { parse_string(p, end); continue; }
            if (c == '[') ++depth;
            if (c == ']') { --depth; ++p; if (depth == 0) break; continue; }
            ++p;
        }
    } else {
        while (p < end && std::isalpha((unsigned char)*p)) ++p; // true/false/null
    }
    if (p > end) p = end;
    return std::string(start, (size_t)(p - start));
}

static std::string parse_key(const char*& p, const char* end) {
    skip_ws(p, end);
    return parse_string(p, end);
}

Request parse_request(const char* begin, const char* end) {
    Request req;
    skip_ws(begin, end);
    if (begin >= end || *begin != '{') throw JsonParseError("expected JSON object");
    ++begin;
    bool have_id = false;
    skip_ws(begin, end);
    if (begin < end && *begin == '}') return req; // empty object (unlikely)
    while (true) {
        std::string key = parse_key(begin, end);
        skip_ws(begin, end);
        expect_lit(begin, end, ":");
        skip_ws(begin, end);
        if (key == "id") {
            req.id_raw = capture_value_raw(begin, end);
            have_id = true;
        } else if (key == "op") {
            req.op = parse_string(begin, end);
        } else if (key == "sql") {
            req.sql = parse_string(begin, end);
        } else if (key == "search_mode") {
            req.search_mode = parse_string(begin, end);
        } else {
            capture_value_raw(begin, end); // ignore unknown fields
        }
        skip_ws(begin, end);
        if (begin < end && *begin == ',') { ++begin; continue; }
        if (begin < end && *begin == '}') { ++begin; break; }
        throw JsonParseError("expected ',' or '}'");
    }
    if (!have_id) throw JsonParseError("missing id");
    return req;
}

std::string escape_string(const std::string& s) {
    std::string out;
    out += '"';
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
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
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c; // UTF-8 bytes passthrough
                }
        }
    }
    out += '"';
    return out;
}

std::string number(double v) {
    if (!std::isfinite(v)) return "0"; // protocol forbids NaN/Infinity
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return std::string(buf);
}

} // namespace json
