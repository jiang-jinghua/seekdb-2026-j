#pragma once
// Engine: catalog, transactions, WAL persistence, query execution
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include "parser.h"

struct Result {
    bool is_select = false;
    bool has_affected = false;               // true for INSERT/DELETE/UPDATE
    int64_t affected_rows = 0;
    std::vector<std::string> header;
    std::vector<Row> rows;                   // typed values (serialized by the protocol layer)
};

class Engine {
public:
    explicit Engine(const std::string& data_dir);
    ~Engine();
    Result execute(const Stmt& st);
    void shutdown(); // rollback uncommitted txn, flush + fsync everything

private:
    struct Table {
        Schema schema;
        std::unordered_map<int64_t, Row> rows;
        std::string meta_path, wal_path;
        int wal_fd = -1;
        uint32_t unsynced = 0;
        std::string wal_buf; // userspace WAL buffer, flushed to fd in bulk
        std::vector<std::pair<std::string, std::string>> indexes; // (name, column)
    };

    // ---- catalog / storage ----
    Table* get_table(const std::string& name);
    void create_table(const Schema& schema);
    void drop_table(const std::string& name);
    void load_dir();
    static void save_meta(const Table& t);
    static std::string encode_record_insert(const Row& row);
    static std::string encode_record_delete(int64_t pk);
    static void wal_append(Table& t, const std::string& rec);
    static void wal_flush_buf(Table& t);
    static void wal_sync(Table& t);
    // group-commit: write() immediately, fsync on explicit commit / threshold / shutdown
    void wal_auto_commit(Table& t, const std::string& rec, bool force_sync = false);
    void sync_all();
    bool strict_sync_ = false;
    uint32_t sync_every_ = 64;
    // replay one record; returns bytes consumed, 0 on corrupt/truncated tail
    static size_t wal_replay_one(const char* data, size_t size, Table& t, bool apply);

    // ---- transaction staging: table(lower name) -> pk -> nullopt=delete / row=upsert ----
    bool in_txn_ = false;
    std::unordered_map<std::string, std::map<int64_t, std::optional<Row>>> staged_;

    void txn_stage(const std::string& tbl, int64_t pk, std::optional<Row> row);
    void txn_commit();
    void txn_rollback();

    // ---- execution helpers ----
    void bind_expr(Expr* e, const Schema& sch) const;
    Value eval_expr(const Expr* e, const Row& row, const Schema& sch) const;
    bool eval_where(const Expr* e, const Row& row, const Schema& sch) const;
    // visible view of a table (committed + staged overlay)
    void collect_view(const Table& t,
                      std::vector<std::pair<int64_t, const Row*>>& view) const;
    void check_row(const Table& t, const Row& row, int64_t pk,
                   bool check_dup_committed, bool check_dup_staged) const;
    int64_t row_pk(const Schema& sch, const Row& row) const;
    void coerce_value(const ColumnDef& cd, Value& v) const;

    std::string dir_;
    std::unordered_map<std::string, std::unique_ptr<Table>> tables_; // key: lowercase name
};
