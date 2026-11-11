#pragma once
// MiniVectorDB - common types
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>

// ---- Output-format knobs (single place to adapt to the official test driver) ----
namespace outfmt {
    static const char* OK       = "SUCCESS";  // printed for non-SELECT statements
    static const char* FAIL     = "FAIL";     // printed for failed statements
    static const char* SEP      = " | ";      // column separator in SELECT output
    static const char* NULLSTR  = "NULL";
}

enum class ColType : uint8_t { BIGINT, VARCHAR, VECTOR, DOUBLE /* literal-only, never stored */ };

struct ColumnDef {
    std::string name;
    ColType type = ColType::BIGINT;
    int vec_dim = 0;      // VECTOR: dimension
    int varchar_len = 0;  // VARCHAR: max length (0 = unlimited)
    bool primary_key = false;
};

struct Schema {
    std::string name;
    std::vector<ColumnDef> cols;
    int pk_index = -1;

    int find_col(const std::string& n) const {
        for (size_t i = 0; i < cols.size(); ++i)
            if (ieq(cols[i].name, n)) return (int)i;
        return -1;
    }
    static bool ieq(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
        return true;
    }
};

struct Value {
    ColType type = ColType::BIGINT;
    bool null = false;
    int64_t i = 0;          // BIGINT
    double d = 0;           // DOUBLE (expression results / float literals)
    std::string s;          // VARCHAR
    std::vector<float> v;   // VECTOR
};
using Row = std::vector<Value>;

struct SqlError : std::runtime_error {
    std::string code; // semantic error code for the JSON protocol
    explicit SqlError(const std::string& m) : std::runtime_error(m), code("internal_error") {}
    SqlError(const std::string& c, const std::string& m) : std::runtime_error(m), code(c) {}
};

// well-known semantic codes (frozen ones: duplicate_key, unsupported_sql, dimension_mismatch)
namespace errcode {
    static const char* SYNTAX        = "syntax_error";
    static const char* DUP_KEY       = "duplicate_key";
    static const char* UNSUPPORTED   = "unsupported_sql";
    static const char* DIM_MISMATCH  = "dimension_mismatch";
    static const char* INVALID_VEC   = "invalid_vector";
    static const char* TABLE_MISSING = "table_not_found";
    static const char* TABLE_EXISTS  = "table_already_exists";
    static const char* COL_MISSING   = "unknown_column";
    static const char* INVALID_VALUE = "invalid_value";
    static const char* INDEX_EXISTS  = "index_already_exists";
    static const char* INDEX_MISSING = "index_not_found";
}

inline bool is_numeric(ColType t) { return t == ColType::BIGINT || t == ColType::DOUBLE; }

// format a float/double the same way everywhere (single knob)
inline std::string format_float(double d) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.7g", d);
    return std::string(buf);
}
