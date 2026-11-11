#include "parser.h"
#include <cmath>

namespace {

std::string upper(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = (char)std::toupper((unsigned char)c);
    return r;
}

class Parser {
public:
    explicit Parser(const std::vector<Token>& toks) : toks_(toks) {}

    std::unique_ptr<Stmt> parse() {
        auto st = std::make_unique<Stmt>();
        const Token& t = cur();
        if (t.type != TokType::IDENT) throw SqlError(errcode::SYNTAX, "expected SQL statement");
        std::string kw = upper(t.text);
        if (kw == "CREATE") {
            next();
            if (accept_kw("VECTOR")) {
                expect_kw("INDEX");
                st->type = StmtType::CREATE_VECTOR_INDEX;
                st->index_name = expect_ident("index name");
                expect_kw("ON");
                st->table = expect_ident("table name");
                expect_sym("(");
                st->index_col = expect_ident("column name");
                expect_sym(")");
            } else { expect_kw("TABLE"); parse_create(*st); }
        }
        else if (kw == "DROP") {
            next();
            if (accept_kw("VECTOR")) {
                expect_kw("INDEX");
                st->type = StmtType::DROP_VECTOR_INDEX;
                st->index_name = expect_ident("index name");
                if (accept_kw("ON")) st->table = expect_ident("table name");
            } else { expect_kw("TABLE"); st->type = StmtType::DROP_TABLE; st->table = expect_ident("table name"); }
        }
        else if (kw == "INSERT") { next(); expect_kw("INTO"); st->type = StmtType::INSERT; parse_insert(*st); }
        else if (kw == "DELETE") { next(); expect_kw("FROM"); st->type = StmtType::DELETE; st->table = expect_ident("table name"); parse_opt_where(*st); }
        else if (kw == "UPDATE") { next(); st->type = StmtType::UPDATE; parse_update(*st); }
        else if (kw == "SELECT") { next(); st->type = StmtType::SELECT; parse_select(*st); }
        else if (kw == "BEGIN" || kw == "START") {
            next();
            if (kw == "START") expect_kw("TRANSACTION");
            st->type = StmtType::BEGIN_TXN;
        }
        else if (kw == "COMMIT") { next(); st->type = StmtType::COMMIT_TXN; }
        else if (kw == "ROLLBACK") { next(); st->type = StmtType::ROLLBACK_TXN; }
        else throw SqlError(errcode::SYNTAX, "unsupported statement: " + kw);

        // allow trailing ';' or end
        if (cur().type == TokType::SYM && cur().text == ";") next();
        if (cur().type != TokType::END) throw SqlError(errcode::SYNTAX, "unexpected token after statement: '" + cur().text + "'");
        return st;
    }

private:
    const std::vector<Token>& toks_;
    size_t pos_ = 0;

    const Token& cur() const { return toks_[pos_]; }
    const Token& peek(size_t k = 1) const {
        size_t p = std::min(pos_ + k, toks_.size() - 1);
        return toks_[p];
    }
    void next() { if (pos_ + 1 < toks_.size()) ++pos_; }

    static bool is_kw(const Token& t, const char* kw) {
        return t.type == TokType::IDENT && upper(t.text) == kw;
    }
    bool accept_kw(const char* kw) {
        if (is_kw(cur(), kw)) { next(); return true; }
        return false;
    }
    void expect_kw(const char* kw) {
        if (!accept_kw(kw))
            throw SqlError(errcode::SYNTAX, std::string("expected keyword ") + kw + ", got '" + cur().text + "'");
    }
    bool accept_sym(const char* s) {
        if (cur().type == TokType::SYM && cur().text == s) { next(); return true; }
        return false;
    }
    void expect_sym(const char* s) {
        if (!accept_sym(s))
            throw SqlError(errcode::SYNTAX, std::string("expected '") + s + "', got '" + cur().text + "'");
    }
    std::string expect_ident(const char* what) {
        if (cur().type != TokType::IDENT)
            throw SqlError(errcode::SYNTAX, std::string("expected ") + what);
        std::string r = cur().text; next(); return r;
    }
    int64_t expect_int(const char* what) {
        if (cur().type != TokType::NUMBER || cur().is_float || cur().num < 0)
            throw SqlError(errcode::SYNTAX, std::string("expected non-negative integer for ") + what);
        int64_t v = cur().num; next(); return v;
    }

    static std::vector<float> parse_vector_string(const std::string& s) {
        // accepted forms: "[1,2,3]" or "1,2,3"
        std::string body = s;
        size_t b0 = body.find('['), b1 = body.find(']');
        if (b0 != std::string::npos || b1 != std::string::npos) {
            if (b0 == std::string::npos || b1 == std::string::npos || b1 < b0)
                throw SqlError(errcode::SYNTAX, "bad vector literal: " + s);
            body = body.substr(b0 + 1, b1 - b0 - 1);
        }
        std::vector<float> v;
        size_t i = 0, n = body.size();
        while (true) {
            while (i < n && (std::isspace((unsigned char)body[i]) || body[i] == ',')) ++i;
            if (i >= n) break;
            size_t start = i;
            while (i < n && (std::isdigit((unsigned char)body[i]) || body[i] == '.' ||
                             body[i] == '-' || body[i] == '+' ||
                             body[i] == 'e' || body[i] == 'E')) ++i;
                if (i == start) throw SqlError(errcode::SYNTAX, "bad vector literal: " + s);
                float f = std::strtof(body.substr(start, i - start).c_str(), nullptr);
                if (!std::isfinite(f)) throw SqlError(errcode::INVALID_VEC, "vector value not finite: " + s);
                v.push_back(f);
        }
        if (v.empty()) throw SqlError(errcode::SYNTAX, "empty vector literal: " + s);
        return v;
    }

    // ---- CREATE TABLE ----
    void parse_create(Stmt& st) {
        st.type = StmtType::CREATE_TABLE;
        st.schema.name = expect_ident("table name");
        expect_sym("(");
        int pk_count = 0;
        while (true) {
            ColumnDef c;
            c.name = expect_ident("column name");
            std::string ty = upper(expect_ident("column type"));
            if (ty == "BIGINT" || ty == "INT" || ty == "INT64" || ty == "LONG") {
                c.type = ColType::BIGINT;
            } else if (ty == "VARCHAR" || ty == "TEXT" || ty == "CHAR") {
                c.type = ColType::VARCHAR;
                if (accept_sym("(")) { c.varchar_len = (int)expect_int("varchar length"); expect_sym(")"); }
            } else if (ty == "VECTOR" || ty == "FLOAT_VECTOR") {
                c.type = ColType::VECTOR;
                if (accept_sym("(")) { c.vec_dim = (int)expect_int("vector dimension"); expect_sym(")"); }
                else c.vec_dim = 2048; // default like MiniOB
            } else {
                throw SqlError(errcode::SYNTAX, "unsupported column type: " + ty);
            }
            while (true) {
                if (accept_kw("PRIMARY")) { expect_kw("KEY"); c.primary_key = true; ++pk_count; continue; }
                if (accept_kw("NOT")) { expect_kw("NULL"); continue; }
                if (accept_kw("NULL")) continue;
                break;
            }
            st.schema.cols.push_back(std::move(c));
            if (accept_sym(",")) continue;
            expect_sym(")");
            break;
        }
        if (pk_count != 1)
            throw SqlError(errcode::SYNTAX, "table must define exactly one BIGINT PRIMARY KEY column");
        // pk must be BIGINT
        int pki = -1;
        for (size_t i = 0; i < st.schema.cols.size(); ++i)
            if (st.schema.cols[i].primary_key) pki = (int)i;
        if (st.schema.cols[pki].type != ColType::BIGINT)
            throw SqlError(errcode::SYNTAX, "primary key column must be BIGINT");
        st.schema.pk_index = pki;
    }

    // ---- INSERT ----
    Value parse_value() {
        Value v;
        const Token& t = cur();
        if (t.type == TokType::NUMBER) {
            next();
            if (t.is_float) { v.type = ColType::DOUBLE; v.d = t.dnum; }
            else            { v.type = ColType::BIGINT; v.i = t.num; }
            return v;
        }
        if (t.type == TokType::STRING) {
            next();
            v.type = ColType::VARCHAR; v.s = t.text;
            return v;
        }
        if (t.type == TokType::SYM && t.text == "[") {
            // direct vector literal: [1,2,3]
            next();
            std::string body;
            while (cur().type != TokType::END && !(cur().type == TokType::SYM && cur().text == "]")) {
                if (cur().type == TokType::NUMBER) {
                    body += cur().is_float ? std::to_string(cur().dnum) : std::to_string(cur().num);
                } else if (cur().type == TokType::SYM && cur().text == ",") {
                    body += ",";
                } else {
                    throw SqlError(errcode::SYNTAX, "bad vector literal");
                }
                next();
            }
            expect_sym("]");
            v.type = ColType::VECTOR; v.v = parse_vector_string(body);
            return v;
        }
        if (t.type == TokType::IDENT) {
            std::string kw = upper(t.text);
            if (kw == "NULL") { next(); v.type = ColType::BIGINT; v.null = true; return v; }
            if (kw == "STRING_TO_VECTOR") {
                next(); expect_sym("(");
                if (cur().type != TokType::STRING) throw SqlError(errcode::SYNTAX, "STRING_TO_VECTOR expects a string literal");
                std::string s = cur().text; next();
                expect_sym(")");
                v.type = ColType::VECTOR; v.v = parse_vector_string(s);
                return v;
            }
        }
        throw SqlError(errcode::SYNTAX, "unsupported value: '" + t.text + "'");
    }

    void parse_insert(Stmt& st) {
        st.type = StmtType::INSERT;
        st.table = expect_ident("table name");
        expect_kw("VALUES");
        while (true) {
            expect_sym("(");
            Row row;
            while (true) {
                row.push_back(parse_value());
                if (accept_sym(",")) continue;
                expect_sym(")");
                break;
            }
            st.insert_rows.push_back(std::move(row));
            if (accept_sym(",")) continue;
            break;
        }
    }

    // ---- UPDATE ----
    void parse_update(Stmt& st) {
        st.type = StmtType::UPDATE;
        st.table = expect_ident("table name");
        expect_kw("SET");
        while (true) {
            std::string col = expect_ident("column name");
            expect_sym("=");
            Value v = parse_value();
            st.sets.emplace_back(std::move(col), std::move(v));
            if (accept_sym(",")) continue;
            break;
        }
        parse_opt_where(st);
    }

    // ---- SELECT ----
    bool try_parse_l2(std::string& col_name, std::vector<float>& vec) {
        if (cur().type == TokType::IDENT && upper(cur().text) == "L2_DISTANCE") {
            next(); expect_sym("(");
            col_name = expect_ident("vector column name");
            expect_sym(",");
            if (cur().type != TokType::STRING) throw SqlError(errcode::SYNTAX, "L2_DISTANCE expects a vector string literal");
            vec = parse_vector_string(cur().text);
            next();
            expect_sym(")");
            return true;
        }
        return false;
    }

    void parse_select(Stmt& st) {
        st.type = StmtType::SELECT;
        if (accept_sym("*")) {
            st.select_star = true;
        } else {
            while (true) {
                SelectItem it;
                if (try_parse_l2(it.l2_col_name, it.l2_vec)) {
                    it.is_l2 = true;
                } else {
                    it.col_name = expect_ident("column name");
                }
                if (accept_kw("AS")) it.alias = expect_ident("alias");
                st.select_items.push_back(std::move(it));
                if (accept_sym(",")) continue;
                break;
            }
        }
        expect_kw("FROM");
        st.table = expect_ident("table name");
        parse_opt_where(st);
        if (accept_kw("ORDER")) {
            expect_kw("BY");
            while (true) {
                OrderItem oi;
                if (try_parse_l2(oi.l2_col_name, oi.l2_vec)) {
                    oi.is_l2 = true;
                } else {
                    oi.col_name = expect_ident("order column");
                }
                if (accept_kw("DESC")) oi.desc = true;
                else accept_kw("ASC");
                st.order_by.push_back(std::move(oi));
                if (accept_sym(",")) continue;
                break;
            }
        }
        if (accept_kw("LIMIT")) {
            st.limit = expect_int("limit");
        }
    }

    // ---- WHERE ----
    void parse_opt_where(Stmt& st) {
        if (accept_kw("WHERE")) st.where = parse_or();
    }

    ExprPtr parse_or() {
        ExprPtr e = parse_and();
        while (accept_kw("OR")) {
            ExprPtr r = parse_and();
            ExprPtr n = std::make_unique<Expr>();
            n->kind = ExprKind::OR; n->a = std::move(e); n->b = std::move(r);
            e = std::move(n);
        }
        return e;
    }
    ExprPtr parse_and() {
        ExprPtr e = parse_not();
        while (accept_kw("AND")) {
            ExprPtr r = parse_not();
            ExprPtr n = std::make_unique<Expr>();
            n->kind = ExprKind::AND; n->a = std::move(e); n->b = std::move(r);
            e = std::move(n);
        }
        return e;
    }
    ExprPtr parse_not() {
        if (accept_kw("NOT")) {
            ExprPtr n = std::make_unique<Expr>();
            n->kind = ExprKind::NOT; n->a = parse_not();
            return n;
        }
        return parse_pred();
    }
    static bool is_cmp_op(const Token& t) {
        return t.type == TokType::SYM &&
               (t.text == "=" || t.text == "!=" || t.text == "<>" || t.text == "<" ||
                t.text == "<=" || t.text == ">" || t.text == ">=");
    }
    ExprPtr parse_operand() {
        ExprPtr e = std::make_unique<Expr>();
        const Token& t = cur();
        if (t.type == TokType::SYM && t.text == "-") {
            next();
            const Token& n2 = cur();
            if (n2.type != TokType::NUMBER) throw SqlError(errcode::SYNTAX, "expected number after '-'");
            next();
            e->kind = ExprKind::LIT;
            if (n2.is_float) { e->lit.type = ColType::DOUBLE; e->lit.d = -n2.dnum; }
            else { e->lit.type = ColType::BIGINT; e->lit.i = -n2.num; }
            return e;
        }
        if (t.type == TokType::NUMBER) {
            next();
            e->kind = ExprKind::LIT;
            if (t.is_float) { e->lit.type = ColType::DOUBLE; e->lit.d = t.dnum; }
            else { e->lit.type = ColType::BIGINT; e->lit.i = t.num; }
            return e;
        }
        if (t.type == TokType::STRING) {
            next();
            e->kind = ExprKind::LIT;
            e->lit.type = ColType::VARCHAR; e->lit.s = t.text;
            return e;
        }
        if (t.type == TokType::IDENT) {
            std::string kw = upper(t.text);
            if (kw == "L2_DISTANCE") {
                std::string cn; std::vector<float> vec;
                try_parse_l2(cn, vec);
                e->kind = ExprKind::L2; e->l2_col_name = cn; e->l2_vec = vec;
                return e;
            }
            if (kw == "NULL") {
                next();
                e->kind = ExprKind::LIT;
                e->lit.type = ColType::BIGINT; e->lit.null = true;
                return e;
            }
            next();
            e->kind = ExprKind::COL; e->col_name = t.text;
            return e;
        }
        throw SqlError(errcode::SYNTAX, "bad operand: '" + t.text + "'");
    }
    ExprPtr parse_pred() {
        if (cur().type == TokType::SYM && cur().text == "(") {
            next();
            ExprPtr e = parse_or();
            expect_sym(")");
            return e;
        }
        ExprPtr a = parse_operand();
        if (is_cmp_op(cur())) {
            std::string op = cur().text;
            next();
            ExprPtr b = parse_operand();
            ExprPtr n = std::make_unique<Expr>();
            n->kind = ExprKind::CMP; n->op = op;
            n->a = std::move(a); n->b = std::move(b);
            return n;
        }
        throw SqlError(errcode::SYNTAX, "expected comparison operator after operand");
    }
};

} // namespace

std::unique_ptr<Stmt> parse_statement(const std::vector<Token>& tokens) {
    Parser p(tokens);
    return p.parse();
}
