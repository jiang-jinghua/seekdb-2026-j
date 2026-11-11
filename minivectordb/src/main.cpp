// MiniVectorDB entry point — official JSONL stdio protocol (2026-competition-round2)
//   stdout: exactly one JSON object per line, flushed immediately
//     startup: {"type":"ready"}
//     response: {"id":<echoed>,"ok":true[,"affected_rows":N | "columns":[..],"rows":[[..]]]}
//               or {"id":<echoed>,"ok":false,"error":{"code":"...","message":"..."}}
//   stdin : one JSON request per line: {"id":..,"op":"execute"|"shutdown","sql":"..","search_mode":"exact"|"ann"}
//   stderr: diagnostics only
#include <cstdio>
#include <cctype>
#include <cstring>
#include <iostream>
#include <string>

#include "common.h"
#include "engine.h"
#include "json.h"
#include "parser.h"

#ifndef _WIN32
#include <unistd.h> // _exit
#endif

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

void emit_line(const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// serialize one Value as JSON
std::string value_to_json(const Value& v) {
    if (v.null) return "null";
    switch (v.type) {
        case ColType::BIGINT: return json::number(v.i);
        case ColType::DOUBLE: return json::number(v.d);
        case ColType::VARCHAR: return json::escape_string(v.s);
        case ColType::VECTOR: {
            std::string s = "[";
            for (size_t i = 0; i < v.v.size(); ++i) {
                if (i) s += ",";
                s += json::number((double)v.v[i]);
            }
            s += "]";
            return s;
        }
    }
    return "null";
}

// execute one SQL statement; returns the JSON response body (after "ok":true)
std::string run_statement(Engine& eng, const std::string& sql) {
    auto toks = lex(sql);
    bool any = false;
    for (const auto& tk : toks)
        if (tk.type != TokType::END) { any = true; break; }
    if (!any) throw SqlError(errcode::SYNTAX, "empty statement");
    auto st = parse_statement(toks);
    Result res = eng.execute(*st);
    if (res.is_select) {
        std::string out = ",\"columns\":[";
        for (size_t i = 0; i < res.header.size(); ++i) {
            if (i) out += ",";
            out += json::escape_string(res.header[i]);
        }
        out += "],\"rows\":[";
        for (size_t i = 0; i < res.rows.size(); ++i) {
            if (i) out += ",";
            out += "[";
            for (size_t j = 0; j < res.rows[i].size(); ++j) {
                if (j) out += ",";
                out += value_to_json(res.rows[i][j]);
            }
            out += "]";
        }
        out += "]";
        return out;
    }
    if (res.has_affected)
        return ",\"affected_rows\":" + json::number(res.affected_rows);
    return "";
}

void handle_request(Engine& eng, const Request& req) {
    try {
        if (req.op == "shutdown") {
            eng.shutdown();
            emit_line("{\"id\":" + req.id_raw + ",\"ok\":true}");
            std::fflush(stdout);
            _exit(0); // shutdown(): rollback + flush + fsync already done
        }
        if (req.op == "execute") {
            std::string body = run_statement(eng, req.sql);
            emit_line("{\"id\":" + req.id_raw + ",\"ok\":true" + body + "}");
            return;
        }
        emit_line("{\"id\":" + req.id_raw + ",\"ok\":false,\"error\":{\"code\":\"unsupported_op\",\"message\":\"unknown op\"}}");
    } catch (const SqlError& e) {
        emit_line("{\"id\":" + req.id_raw + ",\"ok\":false,\"error\":{\"code\":" +
                  json::escape_string(e.code.empty() ? "internal_error" : e.code) + ",\"message\":" +
                  json::escape_string(e.what()) + "}}");
    } catch (const std::exception& e) {
        emit_line("{\"id\":" + req.id_raw + ",\"ok\":false,\"error\":{\"code\":\"internal_error\",\"message\":" +
                  json::escape_string(e.what()) + "}}");
    }
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    std::string dir = "/data";
    bool have_dir = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--data-dir" && i + 1 < argc) { dir = argv[++i]; have_dir = true; }
        else if (a.rfind("--data-dir=", 0) == 0) { dir = a.substr(11); have_dir = true; }
        else if (!have_dir && a.rfind("--", 0) != 0) { dir = a; have_dir = true; }
    }
    fprintf(stderr, "[minivectordb] starting, data dir=%s\n", dir.c_str());

    Engine eng(dir);

    // handshake BEFORE reading the first request
    emit_line("{\"type\":\"ready\"}");

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // skip blank lines silently (not a request, no response)
        bool nonws = false;
        for (char c : line)
            if (!std::isspace((unsigned char)c)) { nonws = true; break; }
        if (!nonws) continue;
        try {
            Request req = json::parse_line(line);
            handle_request(eng, req);
        } catch (const JsonParseError& e) {
            // id unknown; the judge always sends valid JSON
            fprintf(stderr, "[minivectordb] bad request JSON: %s\n", e.what());
            emit_line("{\"id\":null,\"ok\":false,\"error\":{\"code\":\"invalid_request\",\"message\":\"bad request JSON\"}}");
        }
    }
    // EOF without shutdown: flush and exit cleanly
    eng.shutdown();
    return 0;
}
