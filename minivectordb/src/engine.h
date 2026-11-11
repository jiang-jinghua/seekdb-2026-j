#pragma once
// Engine: catalog, transactions, WAL persistence, query execution
// Row data lives in the append-only .wal data file; memory keeps a
// pk -> (offset,len) index plus a bounded row cache, so datasets larger
// than the container memory limit are handled by reading rows on demand.
#include <deque>
#include <functional>
#include <map>
#include <memory>
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
    void sync_all();

private:
    struct Loc { uint64_t off; uint32_t len; };   // framed record in the data file
    struct Entry { Loc loc; std::shared_ptr<Row> cache; };
    struct Table {
        Schema schema;
        std::unordered_map<int64_t, Entry> entries;   // pk -> record location (+optional cache)
        std::deque<int64_t> cache_q;                  // FIFO of cached pks (may contain stale pks)
        size_t cache_bytes = 0;
        uint64_t file_off = 0;                        // logical data-file size (flushed + buffered)
        std::string meta_path, wal_path;
        int wal_fd = -1;       // append-only write fd
        int data_fd = -1;      // read fd for positioned row reads
        uint32_t unsynced = 0;
        std::string wal_buf; // userspace write buffer, flushed to fd in bulk
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
    // append one framed record; returns its location; flush/fsync policy as before
    Loc wal_record(Table& t, const std::string& rec, bool force_sync = false);
    // replay one framed record; returns bytes consumed, 0 on corrupt/truncated tail
    size_t wal_replay_one(const char* data, size_t size, uint64_t off, Table& t, bool apply);
    // decode an OP_INSERT payload into a typed row
    static void decode_row_payload(const char* payload, uint32_t len, Row& row);
    // read + decode one row from the data file (cache-aware)
    std::shared_ptr<Row> read_row(Table& t, int64_t pk);
    // insert/replace a committed row: entry loc + bounded cache
    void put_row(Table& t, int64_t pk, Loc loc, std::shared_ptr<Row> row);
    void erase_row(Table& t, int64_t pk);
    void evict_if_needed(Table& t);
    static size_t row_bytes(const Row& row);
    // iterate the visible (committed + staged overlay) rows without materializing them;
    // entry ptr is null for rows staged inside the current transaction
    void for_each_visible(Table& t,
                          const std::function<void(int64_t, const Row&, const Entry*)>& fn);
    bool strict_sync_ = false;
    uint32_t sync_every_ = 64;
    size_t row_cache_budget_ = 256u << 20; // bytes; rows beyond this stay on disk only
    // group-commit: write() immediately, fsync on explicit commit / threshold / shutdown

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
    void check_row(const Table& t, const Row& row, int64_t pk,
                   bool check_dup_committed, bool check_dup_staged) const;
    int64_t row_pk(const Schema& sch, const Row& row) const;
    void coerce_value(const ColumnDef& cd, Value& v) const;

    std::string dir_;
    std::unordered_map<std::string, std::unique_ptr<Table>> tables_; // key: lowercase name
};
