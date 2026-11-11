#pragma once
// Recursive-descent SQL parser
#include <memory>
#include <vector>
#include "lexer.h"

enum class StmtType {
    CREATE_TABLE, DROP_TABLE, INSERT, DELETE, UPDATE, SELECT,
    BEGIN_TXN, COMMIT_TXN, ROLLBACK_TXN,
    CREATE_VECTOR_INDEX, DROP_VECTOR_INDEX
};

enum class ExprKind { COL, LIT, L2, CMP, AND, OR, NOT };

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
    ExprKind kind;
    int col = -1;          // bound column index (COL / L2)
    // COL
    std::string col_name;
    // LIT
    Value lit;
    // L2: l2_col_name + l2_vec
    std::string l2_col_name;
    std::vector<float> l2_vec;
    // CMP
    std::string op;    // = != <> < <= > >=
    ExprPtr a, b;
};

struct SelectItem {
    bool is_l2 = false;
    std::string col_name;
    std::string l2_col_name;
    std::vector<float> l2_vec;
    std::string alias; // from "AS alias" (optional)
};

struct OrderItem {
    bool is_l2 = false;
    std::string col_name;
    std::string l2_col_name;
    std::vector<float> l2_vec;
    bool desc = false;
};

struct Stmt {
    StmtType type;
    std::string table;                              // all data statements
    Schema schema;                                  // CREATE_TABLE
    std::vector<Row> insert_rows;                   // INSERT (raw values, coerce at bind)
    ExprPtr where;                                  // DELETE / UPDATE / SELECT
    std::vector<std::pair<std::string, Value>> sets;// UPDATE
    bool select_star = false;                       // SELECT
    std::vector<SelectItem> select_items;
    std::vector<OrderItem> order_by;
    int64_t limit = -1;                             // SELECT (no limit if < 0)
    std::string index_name;                         // CREATE/DROP VECTOR INDEX
    std::string index_col;                          // CREATE VECTOR INDEX ... ON t(col)
};

std::unique_ptr<Stmt> parse_statement(const std::vector<Token>& tokens);
