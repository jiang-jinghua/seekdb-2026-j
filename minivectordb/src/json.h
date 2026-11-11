#pragma once
// Minimal JSON support for the official JSONL stdio protocol.
// - Request parsing: preserve the RAW text of the "id" field so it can be
//   echoed back with its exact JSON type (number stays number, etc.).
// - Response building: string escaping (UTF-8 passthrough), int64 exactness
//   (no double conversion), finite doubles only.
#include <cstdint>
#include <cstdio>
#include <string>
#include <stdexcept>

struct JsonParseError : std::runtime_error {
    explicit JsonParseError(const std::string& m) : std::runtime_error(m) {}
};

struct Request {
    std::string id_raw;    // verbatim JSON text of the id value
    std::string op;        // "execute" / "shutdown"
    std::string sql;
    std::string search_mode; // "exact" / "ann" (default exact)
};

namespace json {

// ---- parsing ----
void skip_ws(const char*& p, const char* end);
std::string parse_string(const char*& p, const char* end);
void expect_lit(const char*& p, const char* end, const char* lit);
// Parse one request object from [begin,end). Throws JsonParseError.
Request parse_request(const char* begin, const char* end);

// ---- serialization helpers ----
std::string escape_string(const std::string& s); // returns "..." with quotes
inline std::string number(int64_t v) { return std::to_string(v); }
std::string number(double v); // finite doubles only; never NaN/Inf

// Parse a full request line (no trailing newline). Throws on bad JSON.
inline Request parse_line(const std::string& line) {
    const char* b = line.data();
    const char* e = b + line.size();
    return parse_request(b, e);
}

} // namespace json
