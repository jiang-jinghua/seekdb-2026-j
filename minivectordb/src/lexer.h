#pragma once
// SQL lexer
#include <string>
#include <vector>
#include <cstdint>
#include "common.h"

enum class TokType { IDENT, NUMBER, STRING, SYM, END };

struct Token {
    TokType type = TokType::END;
    std::string text;   // IDENT (raw case), STRING (unescaped content), SYM (1-2 chars)
    int64_t num = 0;    // NUMBER integer value
    double dnum = 0;    // NUMBER float value (when is_float)
    bool is_float = false;
};

// Tokenize one SQL statement (should not contain ';', except trailing).
// Supports comments: -- line, # line, /* block */
std::vector<Token> lex(const std::string& sql);
