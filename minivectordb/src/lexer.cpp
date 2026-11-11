#include "lexer.h"

static bool is_ident_start(char c) {
    return std::isalpha((unsigned char)c) || c == '_';
}
static bool is_ident_char(char c) {
    return std::isalnum((unsigned char)c) || c == '_' || c == '$';
}

std::vector<Token> lex(const std::string& sql) {
    std::vector<Token> out;
    size_t i = 0, n = sql.size();
    while (i < n) {
        char c = sql[i];
        if (std::isspace((unsigned char)c)) { ++i; continue; }
        // comments
        if (c == '-' && i + 1 < n && sql[i + 1] == '-') { while (i < n && sql[i] != '\n') ++i; continue; }
        if (c == '#') { while (i < n && sql[i] != '\n') ++i; continue; }
        if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(sql[i] == '*' && sql[i + 1] == '/')) ++i;
            i = (i + 1 < n) ? i + 2 : n;
            continue;
        }
        // string literal: '...' with '' escape
        if (c == '\'') {
            ++i;
            std::string s;
            bool closed = false;
            while (i < n) {
                if (sql[i] == '\'') {
                    if (i + 1 < n && sql[i + 1] == '\'') { s += '\''; i += 2; continue; }
                    ++i; closed = true; break;
                }
                s += sql[i++];
            }
            if (!closed) throw SqlError(errcode::SYNTAX, "unterminated string literal");
            Token t; t.type = TokType::STRING; t.text = std::move(s);
            out.push_back(std::move(t));
            continue;
        }
        // number: [-]digits[.digits]
        if (std::isdigit((unsigned char)c) ||
            (c == '-' && i + 1 < n && std::isdigit((unsigned char)sql[i + 1]))) {
            size_t start = i;
            if (c == '-') ++i;
            bool fl = false;
            while (i < n && std::isdigit((unsigned char)sql[i])) ++i;
            if (i < n && sql[i] == '.' && i + 1 < n && std::isdigit((unsigned char)sql[i + 1])) {
                fl = true; ++i;
                while (i < n && std::isdigit((unsigned char)sql[i])) ++i;
            }
            std::string num = sql.substr(start, i - start);
            Token t; t.type = TokType::NUMBER; t.is_float = fl; t.text = num;
            if (fl) t.dnum = std::strtod(num.c_str(), nullptr);
            else    t.num = std::strtoll(num.c_str(), nullptr, 10);
            out.push_back(std::move(t));
            continue;
        }
        // identifier / keyword
        if (is_ident_start(c)) {
            size_t start = i;
            while (i < n && is_ident_char(sql[i])) ++i;
            Token t; t.type = TokType::IDENT; t.text = sql.substr(start, i - start);
            out.push_back(std::move(t));
            continue;
        }
        // symbols
        auto push_sym = [&](const char* s) {
            Token t; t.type = TokType::SYM; t.text = s;
            out.push_back(std::move(t));
        };
        if (c == '<' && i + 1 < n && (sql[i + 1] == '=' || sql[i + 1] == '>')) {
            std::string s = sql.substr(i, 2); i += 2;
            Token t; t.type = TokType::SYM; t.text = s; out.push_back(std::move(t));
            continue;
        }
        if (c == '>' && i + 1 < n && sql[i + 1] == '=') { push_sym(">="); i += 2; continue; }
        if (c == '!' && i + 1 < n && sql[i + 1] == '=') { push_sym("!="); i += 2; continue; }
        switch (c) {
            case '(': case ')': case ',': case ';': case '=':
            case '<': case '>': case '*': case '[': case ']': case '.':
                push_sym(std::string(1, c).c_str()); ++i; continue;
            default:
                throw SqlError(errcode::SYNTAX, std::string("unexpected character: '") + c + "'");
        }
    }
    Token end; end.type = TokType::END;
    out.push_back(std::move(end));
    return out;
}
