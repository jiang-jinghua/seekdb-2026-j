#include "engine.h"
#include "vector_ops.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

// ---------- portable low-level I/O ----------
namespace {

int open_append(const std::string& path) {
#ifdef _WIN32
    return _open(path.c_str(), _O_WRONLY | _O_CREAT | _O_APPEND | _O_BINARY, 0644);
#else
    return ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
#endif
}
int open_read(const std::string& path) {
#ifdef _WIN32
    return _open(path.c_str(), _O_RDONLY | _O_BINARY);
#else
    return ::open(path.c_str(), O_RDONLY);
#endif
}
void close_fd(int fd) {
#ifdef _WIN32
    _close(fd);
#else
    ::close(fd);
#endif
}
void sync_fd(int fd) {
#ifdef _WIN32
    _commit(fd);
#else
    ::fsync(fd);
#endif
}
void truncate_fd(const std::string& path, long long len) {
#ifdef _WIN32
    (void)path; (void)len; // never truncated on the Windows dev path
#else
    ::truncate(path.c_str(), (long)len);
#endif
}
// positioned read; does not depend on the fd's file position
bool pread_full(int fd, char* buf, size_t n, uint64_t off) {
    size_t done = 0;
    while (done < n) {
        size_t chunk = n - done;
        if (chunk > (1u << 30)) chunk = 1u << 30;
        ssize_t r;
#ifdef _WIN32
        if (_lseeki64(fd, (long long)(off + done), SEEK_SET) < 0) return false;
        r = _read(fd, buf + done, (unsigned)chunk);
#else
        r = ::pread(fd, buf + done, chunk, (off_t)(off + done));
#endif
        if (r <= 0) return false;
        done += (size_t)r;
    }
    return true;
}

} // namespace

// ---------- crc32 ----------
namespace {

uint32_t crc32(const void* data, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < n; ++i)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void put_u32(std::string& s, uint32_t v) { s.append((const char*)&v, 4); }
void put_i64(std::string& s, int64_t v) { s.append((const char*)&v, 8); }
void put_f32(std::string& s, float v) { s.append((const char*)&v, 4); }
uint32_t get_u32(const char* p) { uint32_t v; memcpy(&v, p, 4); return v; }
int64_t get_i64(const char* p) { int64_t v; memcpy(&v, p, 8); return v; }
float get_f32(const char* p) { float v; memcpy(&v, p, 4); return v; }

// record payload: [op u8][pk i64][row]   (row present only for op=0)
constexpr uint8_t OP_INSERT = 0;
constexpr uint8_t OP_DELETE = 1;

std::string encode_payload(uint8_t op, int64_t pk, const Row* row) {
    std::string p;
    p += (char)op;
    put_i64(p, pk);
    if (row) {
        put_u32(p, (uint32_t)row->size());
        for (const Value& v : *row) {
            uint8_t t;
            switch (v.type) {
                case ColType::BIGINT: t = 0; break;
                case ColType::VARCHAR: t = 1; break;
                default: t = 2; break;
            }
            p += (char)t;
            p += (char)(v.null ? 1 : 0);
            switch (t) {
                case 0: put_i64(p, v.i); break;
                case 1:
                    put_u32(p, (uint32_t)v.s.size());
                    p += v.s;
                    break;
                default:
                    put_u32(p, (uint32_t)v.v.size());
                    for (float f : v.v) put_f32(p, f);
                    break;
            }
        }
    }
    return p;
}

// record framing: [len u32][crc u32][payload]
std::string frame_record(const std::string& payload) {
    std::string r;
    put_u32(r, (uint32_t)payload.size());
    put_u32(r, crc32(payload.data(), payload.size()));
    r += payload;
    return r;
}

std::string lower_name(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}

// count UTF-8 code points (VARCHAR(n) is measured in characters, not bytes)
int utf8_count_chars(const std::string& s) {
    int n = 0;
    for (size_t i = 0; i < s.size(); )
        if ((s[i] & 0xC0) != 0x80) { ++n; ++i; }
        else ++i;
    return n;
}

bool compare_vals(const Value& a, const Value& b, const std::string& op) {
    int c;
    if (a.type == ColType::BIGINT && b.type == ColType::BIGINT) {
        // exact int64 comparison — must not go through double (>2^53 loses precision)
        c = (a.i < b.i) ? -1 : (a.i > b.i ? 1 : 0);
    } else if (is_numeric(a.type) && is_numeric(b.type)) {
        double x = (a.type == ColType::DOUBLE) ? a.d : (double)a.i;
        double y = (b.type == ColType::DOUBLE) ? b.d : (double)b.i;
        c = (x < y) ? -1 : (x > y ? 1 : 0);
    } else if (a.type == ColType::VARCHAR && b.type == ColType::VARCHAR)
        c = a.s.compare(b.s);
    else
        return false; // type mismatch -> predicate is false
    if (op == "=") return c == 0;
    if (op == "!=" || op == "<>") return c != 0;
    if (op == "<") return c < 0;
    if (op == "<=") return c <= 0;
    if (op == ">") return c > 0;
    if (op == ">=") return c >= 0;
    throw SqlError(errcode::SYNTAX, "unknown operator: " + op);
}

} // namespace

// ---------- Engine ----------

Engine::Engine(const std::string& dir) : dir_(dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    // MVDB_FSYNC=1 -> fsync on every auto-commit (strict durability)
    strict_sync_ = std::getenv("MVDB_FSYNC") != nullptr;
    const char* se = std::getenv("MVDB_SYNC_EVERY");
    if (se && *se) sync_every_ = (uint32_t)std::max(1, std::atoi(se));
    const char* rc = std::getenv("MVDB_ROW_CACHE_MB");
    if (rc && *rc) row_cache_budget_ = (size_t)std::max(1, std::atoi(rc)) << 20;
    load_dir();
}

Engine::~Engine() {
    sync_all();
    for (auto& kv : tables_) {
        Table& t = *kv.second;
        if (t.wal_buf.size() > 0) { try { wal_flush_buf(t); } catch (...) {} }
        if (t.wal_fd >= 0) close_fd(t.wal_fd);
        if (t.data_fd >= 0) close_fd(t.data_fd);
    }
}

Engine::Table* Engine::get_table(const std::string& name) {
    auto it = tables_.find(lower_name(name));
    if (it == tables_.end()) throw SqlError(errcode::TABLE_MISSING, "table not found: " + name);
    return it->second.get();
}

void Engine::save_meta(const Table& t) {
    FILE* f = std::fopen(t.meta_path.c_str(), "wb");
    if (!f) throw SqlError(errcode::INVALID_VALUE, "cannot write meta file: " + t.meta_path);
    std::string s;
    s += "table " + t.schema.name + "\n";
    s += "ncols " + std::to_string(t.schema.cols.size()) + "\n";
    for (const auto& c : t.schema.cols) {
        int ty = c.type == ColType::BIGINT ? 0 : (c.type == ColType::VARCHAR ? 1 : 2);
        s += "col " + c.name + " " + std::to_string(ty) + " " +
             std::to_string(ty == 2 ? c.vec_dim : c.varchar_len) + " " +
             std::to_string(c.primary_key ? 1 : 0) + "\n";
    }
    for (const auto& ix : t.indexes)
        s += "index " + ix.first + " " + ix.second + "\n";
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
}

void Engine::load_dir() {
    std::error_code ec;
    fs::directory_iterator dit(dir_, ec), dend;
    if (ec) {
        // missing/unreadable data dir: start with an empty database rather than dying
        fprintf(stderr, "[minivectordb] data dir '%s' not readable (%s), starting empty\n",
                dir_.c_str(), ec.message().c_str());
        return;
    }
    for (auto it = dit; it != dend; ++it) {
        std::string p = it->path().string();
        if (p.size() < 5 || p.substr(p.size() - 5) != ".meta") continue;
        FILE* f = std::fopen(p.c_str(), "rb");
        if (!f) continue;
        auto t = std::make_unique<Table>();
        char line[512];
        int ncols = 0;
        while (std::fgets(line, sizeof(line), f)) {
            char name[256], word2[256];
            int a = 0, b = 0, c = 0;
            if (std::sscanf(line, "table %255s", name) == 1) t->schema.name = name;
            else if (std::sscanf(line, "ncols %d", &ncols) == 1) { (void)ncols; }
            else if (std::sscanf(line, "col %255s %d %d %d", name, &a, &b, &c) == 4) {
                ColumnDef cd;
                cd.name = name;
                cd.type = a == 0 ? ColType::BIGINT : (a == 1 ? ColType::VARCHAR : ColType::VECTOR);
                cd.vec_dim = b; cd.varchar_len = b;
                cd.primary_key = c != 0;
                t->schema.cols.push_back(std::move(cd));
            }
            else if (std::sscanf(line, "index %255s %255s", name, word2) == 2) {
                t->indexes.emplace_back(name, word2);
            }
        }
        std::fclose(f);
        for (size_t i = 0; i < t->schema.cols.size(); ++i)
            if (t->schema.cols[i].primary_key) t->schema.pk_index = (int)i;
        std::string base = p.substr(0, p.size() - 5);
        t->meta_path = p;
        t->wal_path = base + ".wal";
        // replay the data file: build pk -> (offset,len) index only; rows stay on disk.
        // Streaming: read in fixed chunks so recovery memory stays bounded even for
        // data files far larger than the container memory limit (only the pending
        // partial frame is carried over between chunks).
        int fd = open_read(t->wal_path);
        uint64_t off = 0;
        if (fd >= 0) {
            std::string pend; // bytes not yet consumed (partial frame carry-over)
            pend.reserve((2u << 20));
            char tmp[1 << 20];
            ssize_t r;
            bool stop = false;
            while (!stop && (r = read(fd, tmp, sizeof(tmp))) > 0) {
                pend.append(tmp, (size_t)r);
                size_t pos = 0;
                while (true) {
                    // need at least a frame header, and the declared payload must be
                    // fully buffered before we can judge the frame (streaming-safe)
                    if (pend.size() - pos < 8) break;
                    uint32_t flen = get_u32(pend.data() + pos);
                    if (flen > (64u << 20)) { // absurd length: corrupt header, truncate
                        fprintf(stderr, "[minivectordb] data file %s: corrupt frame length %u at offset %llu, truncating\n",
                                t->wal_path.c_str(), flen, (unsigned long long)off);
                        truncate_fd(t->wal_path, (long long)off);
                        stop = true;
                        break;
                    }
                    if (flen > (pend.size() - pos) - 8) break; // frame spans chunk boundary: read more
                    size_t consumed = wal_replay_one(pend.data() + pos, pend.size() - pos, off, *t, true);
                    if (consumed == 0) { // CRC mismatch: genuine corruption, truncate
                        fprintf(stderr, "[minivectordb] data file %s: stop replay at offset %llu (corrupt tail, truncating)\n",
                                t->wal_path.c_str(), (unsigned long long)off);
                        truncate_fd(t->wal_path, (long long)off);
                        stop = true;
                        break;
                    }
                    pos += consumed;
                    off += consumed;
                }
                if (pos > 0) pend.erase(0, pos);
            }
            if (!stop && !pend.empty()) {
                // EOF with an incomplete frame: process was killed mid-append; drop the tail
                fprintf(stderr, "[minivectordb] data file %s: partial frame at EOF, truncating to %llu\n",
                        t->wal_path.c_str(), (unsigned long long)off);
                truncate_fd(t->wal_path, (long long)off);
            }
            close_fd(fd);
        }
        t->file_off = off;
        t->wal_fd = open_append(t->wal_path);
        t->data_fd = open_read(t->wal_path);
        std::string key = lower_name(t->schema.name);
        tables_[key] = std::move(t);
        fprintf(stderr, "[minivectordb] loaded table '%s' (%zu rows indexed)\n",
                key.c_str(), tables_[key]->entries.size());
    }
}

size_t Engine::wal_replay_one(const char* data, size_t size, uint64_t off, Table& t, bool apply) {
    if (size < 8) return 0;
    uint32_t len = get_u32(data);
    if (len > size - 8) return 0; // overflow-safe bounds check
    uint32_t crc = get_u32(data + 4);
    const char* payload = data + 8;
    if (crc32(payload, len) != crc) return 0;
    if (!apply) return len + 8;
    uint8_t op = (uint8_t)payload[0];
    int64_t pk = get_i64(payload + 1);
    if (op == OP_DELETE) {
        erase_row(t, pk);
        return len + 8;
    }
    t.entries[pk] = Entry{ Loc{off, len + 8}, nullptr };
    return len + 8;
}

void Engine::decode_row_payload(const char* payload, uint32_t len, Row& row) {
    const char* p = payload + 9;
    const char* end = payload + len;
    if (len < 9 || p + 4 > end) { row.clear(); return; } // needs op byte + pk + ncols
    uint32_t ncols = get_u32(p); p += 4;
    row.clear();
    row.reserve(ncols);
    for (uint32_t i = 0; i < ncols && p + 2 <= end; ++i) {
        Value v;
        uint8_t ty = (uint8_t)p[0];
        uint8_t isnull = (uint8_t)p[1];
        p += 2;
        v.null = isnull != 0;
        switch (ty) {
            case 0:
                if (p + 8 > end) { row.clear(); return; }
                v.type = ColType::BIGINT;
                v.i = get_i64(p); p += 8;
                break;
            case 1: {
                if (p + 4 > end) { row.clear(); return; }
                v.type = ColType::VARCHAR;
                uint32_t l = get_u32(p); p += 4;
                if (p + l > end) { row.clear(); return; }
                v.s.assign(p, l); p += l;
                break;
            }
            default: {
                if (p + 4 > end) { row.clear(); return; }
                v.type = ColType::VECTOR;
                uint32_t dim = get_u32(p); p += 4;
                if (p + (uint64_t)dim * 4 > end) { row.clear(); return; }
                v.v.reserve(dim);
                for (uint32_t k = 0; k < dim; ++k, p += 4) v.v.push_back(get_f32(p));
                break;
            }
        }
        row.push_back(std::move(v));
    }
}

std::string Engine::encode_record_insert(int64_t pk, const Row& row) {
    // pk must come from the schema's pk column — do NOT assume it is row[0]
    return frame_record(encode_payload(OP_INSERT, pk, &row));
}
std::string Engine::encode_record_delete(int64_t pk) {
    return frame_record(encode_payload(OP_DELETE, pk, nullptr));
}
void Engine::wal_append(Table& t, const std::string& rec) {
    t.wal_buf += rec;
    t.file_off += rec.size();
    if (t.wal_buf.size() >= (256u << 10)) wal_flush_buf(t);
}
void Engine::wal_flush_buf(Table& t) {
    if (t.wal_fd < 0) throw SqlError("data file not open for table " + t.schema.name);
    size_t off = 0;
    while (off < t.wal_buf.size()) {
#ifdef _WIN32
        int w = _write(t.wal_fd, t.wal_buf.data() + off, (unsigned)(t.wal_buf.size() - off));
#else
        ssize_t w = ::write(t.wal_fd, t.wal_buf.data() + off, t.wal_buf.size() - off);
#endif
        if (w <= 0) throw SqlError("data file write failed");
        off += (size_t)w;
    }
    t.wal_buf.clear();
}
void Engine::wal_sync(Table& t) {
    if (t.wal_buf.size() > 0) wal_flush_buf(t);
    if (t.wal_fd >= 0) sync_fd(t.wal_fd);
}

Engine::Loc Engine::wal_record(Table& t, const std::string& rec, bool force_sync) {
    Loc loc{ t.file_off, (uint32_t)rec.size() };
    wal_append(t, rec);
    // write-through: committed data must be in the OS (survive SIGKILL) before ack
    wal_flush_buf(t);
    if (force_sync || strict_sync_) { wal_sync(t); t.unsynced = 0; return loc; }
    if (++t.unsynced >= sync_every_) { wal_sync(t); t.unsynced = 0; }
    return loc;
}

// ---------- bounded row cache ----------

size_t Engine::row_bytes(const Row& row) {
    size_t b = 64;
    for (const Value& v : row) {
        b += 32;
        if (v.type == ColType::VARCHAR) b += v.s.size();
        else if (v.type == ColType::VECTOR) b += v.v.size() * 4;
    }
    return b;
}

void Engine::evict_if_needed(Table& t) {
    while (t.cache_bytes > row_cache_budget_ && !t.cache_q.empty()) {
        int64_t pk = t.cache_q.front();
        t.cache_q.pop_front();
        auto it = t.entries.find(pk);
        if (it == t.entries.end() || !it->second.cache) continue;
        t.cache_bytes -= row_bytes(*it->second.cache);
        it->second.cache.reset();
    }
}

void Engine::put_row(Table& t, int64_t pk, Loc loc, std::shared_ptr<Row> row) {
    auto it = t.entries.find(pk);
    if (it == t.entries.end())
        it = t.entries.emplace(pk, Entry{ loc, nullptr }).first;
    else
        it->second.loc = loc;
    if (row) {
        size_t b = row_bytes(*row);
        it->second.cache = std::move(row);
        t.cache_bytes += b;
        t.cache_q.push_back(pk);
        evict_if_needed(t);
    }
}

void Engine::erase_row(Table& t, int64_t pk) {
    auto it = t.entries.find(pk);
    if (it == t.entries.end()) return;
    if (it->second.cache) t.cache_bytes -= row_bytes(*it->second.cache);
    t.entries.erase(it);
}

std::shared_ptr<Row> Engine::read_row(Table& t, int64_t pk) {
    auto it = t.entries.find(pk);
    if (it == t.entries.end()) return nullptr;
    if (it->second.cache) return it->second.cache;
    std::string buf(it->second.loc.len, '\0');
    if (!pread_full(t.data_fd, buf.data(), buf.size(), it->second.loc.off))
        throw SqlError(errcode::INVALID_VALUE, "data file read failed for pk " + std::to_string(pk));
    uint32_t len = get_u32(buf.data());
    uint32_t crc = get_u32(buf.data() + 4);
    const char* payload = buf.data() + 8;
    if (len + 8 != buf.size() || crc32(payload, len) != crc)
        throw SqlError(errcode::INVALID_VALUE, "data file record corrupt for pk " + std::to_string(pk));
    auto r = std::make_shared<Row>();
    decode_row_payload(payload, len, *r);
    // fill the cache (bounded); the returned shared_ptr stays valid even if evicted
    it->second.cache = r;
    t.cache_bytes += row_bytes(*r);
    t.cache_q.push_back(pk);
    evict_if_needed(t);
    return r;
}

void Engine::for_each_visible(Table& t,
                              const std::function<void(int64_t, const Row&, const Entry*)>& fn) {
    const auto ops_it = staged_.find(lower_name(t.schema.name));
    const std::map<int64_t, std::optional<Row>>* ops =
        (in_txn_ && ops_it != staged_.end()) ? &ops_it->second : nullptr;
    for (auto& kv : t.entries) {
        if (ops) {
            auto oit = ops->find(kv.first);
            if (oit != ops->end()) {
                if (!oit->second) continue;          // deleted in txn
                fn(kv.first, *oit->second, nullptr); // staged row wins
                continue;
            }
        }
        std::shared_ptr<Row> r = read_row(t, kv.first);
        if (r) fn(kv.first, *r, &kv.second);
    }
    if (ops) {
        for (const auto& kv : *ops) {
            if (kv.second && !t.entries.count(kv.first))   // newly inserted in txn
                fn(kv.first, *kv.second, nullptr);
        }
    }
}

void Engine::shutdown() {
    if (in_txn_) txn_rollback();
    sync_all(); // flush remaining buffers + fsync
}

void Engine::sync_all() {
    for (auto& kv : tables_) {
        Table& t = *kv.second;
        if (t.unsynced > 0 || t.wal_buf.size() > 0) { wal_sync(t); t.unsynced = 0; }
    }
}

void Engine::create_table(const Schema& schema) {
    std::string key = lower_name(schema.name);
    if (tables_.count(key)) throw SqlError(errcode::TABLE_EXISTS, "table already exists: " + schema.name);
    auto t = std::make_unique<Table>();
    t->schema = schema;
    t->meta_path = dir_ + "/" + key + ".meta";
    t->wal_path = dir_ + "/" + key + ".wal";
    save_meta(*t);
    t->wal_fd = open_append(t->wal_path);
    t->data_fd = open_read(t->wal_path);
    tables_[key] = std::move(t);
}

void Engine::drop_table(const std::string& name) {
    std::string key = lower_name(name);
    auto it = tables_.find(key);
    if (it == tables_.end()) throw SqlError(errcode::TABLE_MISSING, "table not found: " + name);
    if (it->second->wal_fd >= 0) close_fd(it->second->wal_fd);
    if (it->second->data_fd >= 0) close_fd(it->second->data_fd);
    std::remove(it->second->meta_path.c_str());
    std::remove(it->second->wal_path.c_str());
    tables_.erase(it);
    staged_.erase(key);
}

// ---------- transactions ----------

void Engine::txn_stage(const std::string& tbl, int64_t pk, std::optional<Row> row) {
    staged_[lower_name(tbl)][pk] = std::move(row);
}

void Engine::txn_commit() {
    // deterministic order
    std::vector<std::string> names;
    for (const auto& kv : staged_) names.push_back(kv.first);
    std::sort(names.begin(), names.end());
    for (const auto& tn : names) {
        auto tit = tables_.find(tn);
        if (tit == tables_.end()) continue;
        Table& t = *tit->second;
        const auto& ops = staged_[tn];
        struct Applied { int64_t pk; Loc loc; std::shared_ptr<Row> row; };
        std::vector<Applied> applied;
        for (const auto& kv : ops) {
            if (kv.second) {
                // pk from the staged key (= schema pk column value), not row[0]
                std::string rec = encode_record_insert(kv.first, *kv.second);
                Loc loc{ t.file_off, (uint32_t)rec.size() };
                wal_append(t, rec);
                applied.push_back({ kv.first, loc, std::make_shared<Row>(std::move(*kv.second)) });
            } else {
                std::string rec = encode_record_delete(kv.first);
                Loc loc{ t.file_off, (uint32_t)rec.size() };
                wal_append(t, rec);
                applied.push_back({ kv.first, loc, nullptr });
            }
        }
        wal_sync(t); // explicit COMMIT: force durability
        t.unsynced = 0;
        for (auto& a : applied) {
            if (a.row) put_row(t, a.pk, a.loc, a.row);
            else erase_row(t, a.pk);
        }
    }
    staged_.clear();
    in_txn_ = false;
}

void Engine::txn_rollback() {
    staged_.clear();
    in_txn_ = false;
}

// ---------- row helpers ----------

int64_t Engine::row_pk(const Schema& sch, const Row& row) const {
    if (sch.pk_index < 0 || sch.pk_index >= (int)row.size())
        throw SqlError(errcode::INVALID_VALUE, "internal error: missing primary key");
    const Value& v = row[sch.pk_index];
    if (v.null || v.type != ColType::BIGINT)
        throw SqlError(errcode::INVALID_VALUE, "primary key must be a non-null BIGINT");
    return v.i;
}

void Engine::coerce_value(const ColumnDef& cd, Value& v) const {
    if (v.null) return;
    if (cd.type == ColType::BIGINT) {
        if (v.type == ColType::DOUBLE && v.d == (double)(int64_t)v.d) { v.type = ColType::BIGINT; v.i = (int64_t)v.d; return; }
        if (v.type != ColType::BIGINT) throw SqlError(errcode::INVALID_VALUE, "expected BIGINT value for column " + cd.name);
    } else if (cd.type == ColType::VARCHAR) {
        if (v.type == ColType::BIGINT) { v.s = std::to_string(v.i); v.type = ColType::VARCHAR; return; }
        if (v.type != ColType::VARCHAR) throw SqlError(errcode::INVALID_VALUE, "expected string value for column " + cd.name);
        if (cd.varchar_len > 0 && utf8_count_chars(v.s) > cd.varchar_len)
            throw SqlError(errcode::INVALID_VALUE, "string too long for column " + cd.name);
    } else if (cd.type == ColType::VECTOR) {
        if (v.type == ColType::VARCHAR) {
            // coerce '[1,2,3]' / '1,2,3' string into vector
            v.v.clear();
            const std::string& s = v.s;
            size_t b0 = s.find('['), b1 = s.find(']');
            std::string body = s;
            if (b0 != std::string::npos || b1 != std::string::npos) {
                if (b0 == std::string::npos || b1 == std::string::npos || b1 < b0)
                    throw SqlError(errcode::INVALID_VEC, "bad vector literal: " + s);
                body = s.substr(b0 + 1, b1 - b0 - 1);
            }
            size_t i = 0;
            while (true) {
                while (i < body.size() && (std::isspace((unsigned char)body[i]) || body[i] == ',')) ++i;
                if (i >= body.size()) break;
                size_t st = i;
                while (i < body.size() && (std::isdigit((unsigned char)body[i]) || body[i] == '.' ||
                                           body[i] == '-' || body[i] == '+' ||
                                           body[i] == 'e' || body[i] == 'E')) ++i;
                if (i == st) throw SqlError(errcode::INVALID_VEC, "bad vector literal: " + s);
                float f = std::strtof(body.substr(st, i - st).c_str(), nullptr);
                if (!std::isfinite(f)) throw SqlError(errcode::INVALID_VEC, "vector value not finite: " + s);
                v.v.push_back(f);
            }
            v.s.clear();
            v.type = ColType::VECTOR;
        }
        if (v.type != ColType::VECTOR) throw SqlError(errcode::INVALID_VEC, "expected vector value for column " + cd.name);
        if ((int)v.v.size() != cd.vec_dim)
            throw SqlError(errcode::DIM_MISMATCH, "vector dimension mismatch for column " + cd.name +
                           ": expected " + std::to_string(cd.vec_dim) + ", got " + std::to_string(v.v.size()));
        for (float f : v.v)
            if (!std::isfinite(f)) throw SqlError(errcode::INVALID_VEC, "vector value not finite");
    }
}

void Engine::check_row(const Table& t, const Row& /*row*/, int64_t pk,
                       bool check_dup_committed, bool check_dup_staged) const {
    if (check_dup_committed && t.entries.count(pk))
        throw SqlError(errcode::DUP_KEY, "duplicate primary key: " + std::to_string(pk));
    if (check_dup_staged && staged_.count(lower_name(t.schema.name))) {
        const auto& ops = staged_.at(lower_name(t.schema.name));
        auto it = ops.find(pk);
        if (it != ops.end() && it->second)
            throw SqlError(errcode::DUP_KEY, "duplicate primary key in transaction: " + std::to_string(pk));
    }
}

// ---------- expression binding / evaluation ----------

void Engine::bind_expr(Expr* e, const Schema& sch) const {
    if (!e) return;
    if (e->kind == ExprKind::COL) {
        int idx = sch.find_col(e->col_name);
        if (idx < 0) throw SqlError(errcode::COL_MISSING, "unknown column: " + e->col_name);
        e->col = idx;
    } else if (e->kind == ExprKind::L2) {
        int idx = sch.find_col(e->l2_col_name);
        if (idx < 0) throw SqlError(errcode::COL_MISSING, "unknown column: " + e->l2_col_name);
        const ColumnDef& cd = sch.cols[idx];
        if (cd.type != ColType::VECTOR)
            throw SqlError(errcode::INVALID_VALUE, "L2_DISTANCE expects a VECTOR column: " + e->l2_col_name);
        if ((int)e->l2_vec.size() != cd.vec_dim)
            throw SqlError(errcode::DIM_MISMATCH, "L2_DISTANCE vector dimension mismatch: expected " +
                           std::to_string(cd.vec_dim) + ", got " + std::to_string(e->l2_vec.size()));
        e->col = idx;
    } else {
        bind_expr(e->a.get(), sch);
        bind_expr(e->b.get(), sch);
    }
}

Value Engine::eval_expr(const Expr* e, const Row& row, const Schema& sch) const {
    switch (e->kind) {
        case ExprKind::COL:
            if (e->col < 0 || e->col >= (int)row.size())
                throw SqlError(errcode::INVALID_VALUE, "internal error: column index out of range");
            return row[e->col];
        case ExprKind::LIT:
            return e->lit;
        case ExprKind::L2: {
            const Value& v = row[e->col];
            Value r;
            r.type = ColType::DOUBLE;
            if (v.null) { r.null = true; return r; }
            r.d = (double)l2_distance(v.v.data(), e->l2_vec.data(), v.v.size());
            return r;
        }
        case ExprKind::CMP: {
            Value a = eval_expr(e->a.get(), row, sch);
            Value b = eval_expr(e->b.get(), row, sch);
            Value r;
            r.type = ColType::BIGINT;
            r.i = (compare_vals(a, b, e->op) && !a.null && !b.null) ? 1 : 0;
            return r;
        }
        case ExprKind::AND: {
            Value l = eval_expr(e->a.get(), row, sch);
            if (l.null || l.i == 0) { Value f; f.type = ColType::BIGINT; f.i = 0; return f; }
            Value r2 = eval_expr(e->b.get(), row, sch);
            Value o; o.type = ColType::BIGINT;
            o.i = (!r2.null && r2.i != 0) ? 1 : 0;
            return o;
        }
        case ExprKind::OR: {
            Value l = eval_expr(e->a.get(), row, sch);
            if (!l.null && l.i != 0) { Value t; t.type = ColType::BIGINT; t.i = 1; return t; }
            Value r2 = eval_expr(e->b.get(), row, sch);
            Value o; o.type = ColType::BIGINT;
            o.i = (!r2.null && r2.i != 0) ? 1 : 0;
            return o;
        }
        case ExprKind::NOT: {
            Value l = eval_expr(e->a.get(), row, sch);
            Value o; o.type = ColType::BIGINT;
            o.i = (!l.null && l.i == 0) ? 1 : 0;
            return o;
        }
    }
    throw SqlError(errcode::INVALID_VALUE, "internal error: bad expression");
}

bool Engine::eval_where(const Expr* e, const Row& row, const Schema& sch) const {
    if (!e) return true;
    Value v = eval_expr(e, row, sch);
    return !v.null && v.i != 0;
}

// ---------- execute ----------

Result Engine::execute(const Stmt& st) {
    switch (st.type) {
        case StmtType::CREATE_TABLE: {
            // DDL is auto-committed immediately
            create_table(st.schema);
            return {};
        }
        case StmtType::DROP_TABLE: {
            drop_table(st.table);
            return {};
        }
        case StmtType::BEGIN_TXN: {
            if (in_txn_) txn_commit(); // implicit commit of previous txn
            in_txn_ = true;
            return {};
        }
        case StmtType::COMMIT_TXN: {
            if (in_txn_) txn_commit();
            return {};
        }
        case StmtType::ROLLBACK_TXN: {
            if (in_txn_) txn_rollback();
            return {};
        }
        case StmtType::INSERT: {
            Table& t = *get_table(st.table);
            const Schema& sch = t.schema;
            // statement-level atomicity: validate and coerce everything first
            struct PendingRow { int64_t pk; Row row; };
            std::vector<PendingRow> pend;
            std::set<int64_t> seen; // duplicates inside one statement
            for (const Row& raw : st.insert_rows) {
                if ((int)raw.size() != (int)sch.cols.size())
                    throw SqlError(errcode::INVALID_VALUE, "column count mismatch in INSERT");
                Row row;
                row.reserve(raw.size());
                for (size_t i = 0; i < raw.size(); ++i) {
                    Value v = raw[i];
                    coerce_value(sch.cols[i], v);
                    row.push_back(std::move(v));
                }
                int64_t pk = row_pk(sch, row);
                if (!seen.insert(pk).second)
                    throw SqlError(errcode::DUP_KEY, "duplicate primary key: " + std::to_string(pk));
                check_row(t, row, pk, true, true);
                pend.push_back({pk, std::move(row)});
            }
            // all validated -> apply
            for (auto& p : pend) {
                if (in_txn_) {
                    txn_stage(st.table, p.pk, std::move(p.row));
                } else {
                    std::string rec = encode_record_insert(p.pk, p.row);
                    Loc loc{ t.file_off, (uint32_t)rec.size() };
                    wal_record(t, rec);
                    put_row(t, p.pk, loc, std::make_shared<Row>(std::move(p.row)));
                }
            }
            Result r;
            r.has_affected = true;
            r.affected_rows = (int64_t)pend.size();
            return r;
        }
        case StmtType::DELETE: {
            Table& t = *get_table(st.table);
            bind_expr(st.where.get(), t.schema);
            std::vector<int64_t> targets;
            for_each_visible(t, [&](int64_t pk, const Row& row, const Entry*) {
                if (eval_where(st.where.get(), row, t.schema))
                    targets.push_back(pk);
            });
            for (int64_t pk : targets) {
                if (in_txn_) txn_stage(st.table, pk, std::nullopt);
                else {
                    std::string rec = encode_record_delete(pk);
                    wal_record(t, rec);
                    erase_row(t, pk);
                }
            }
            Result r;
            r.has_affected = true;
            r.affected_rows = (int64_t)targets.size();
            return r;
        }
        case StmtType::UPDATE: {
            Table& t = *get_table(st.table);
            const Schema& sch = t.schema;
            // resolve set columns
            std::vector<std::pair<int, Value>> sets;
            for (const auto& kv : st.sets) {
                int idx = sch.find_col(kv.first);
                if (idx < 0) throw SqlError(errcode::COL_MISSING, "unknown column: " + kv.first);
                sets.emplace_back(idx, kv.second);
            }
            bind_expr(st.where.get(), sch);
            // rows to update: pk -> new row
            std::map<int64_t, Row> updates;
            std::vector<int64_t> matched;
            for_each_visible(t, [&](int64_t pk, const Row& row, const Entry*) {
                if (eval_where(st.where.get(), row, sch))
                    matched.push_back(pk);
            });
            // re-read matched rows and build the new versions
            for (int64_t pk : matched) {
                std::shared_ptr<Row> cur;
                {   // staged rows win; read_row handles the committed case
                    const auto ops_it = staged_.find(lower_name(t.schema.name));
                    if (in_txn_ && ops_it != staged_.end()) {
                        auto oit = ops_it->second.find(pk);
                        if (oit != ops_it->second.end() && oit->second)
                            cur = std::make_shared<Row>(*oit->second);
                    }
                    if (!cur) cur = read_row(t, pk);
                }
                if (!cur) continue;
                Row newrow = *cur;
                for (auto& sv : sets) {
                    Value v = sv.second;
                    coerce_value(sch.cols[sv.first], v);
                    newrow[sv.first] = std::move(v);
                }
                int64_t newpk = row_pk(sch, newrow);
                updates[pk] = std::move(newrow);
                if (newpk != pk) {
                    // pk changed: ensure target free
                    if (t.entries.count(newpk) && !updates.count(newpk))
                        throw SqlError(errcode::DUP_KEY, "duplicate primary key on update: " + std::to_string(newpk));
                }
            }
            // dup check among updates
            {
                std::set<int64_t> pks;
                for (const auto& kv : updates) {
                    int64_t npk = row_pk(sch, kv.second);
                    if (!pks.insert(npk).second)
                        throw SqlError(errcode::DUP_KEY, "duplicate primary key on update: " + std::to_string(npk));
                }
            }
            for (const auto& kv : updates) {
                int64_t npk = row_pk(sch, kv.second);
                if (in_txn_) {
                    txn_stage(st.table, kv.first, std::nullopt);
                    txn_stage(st.table, npk, kv.second);
                } else {
                    if (npk != kv.first) {
                        // pk changed: persist a DELETE for the old pk, otherwise the
                        // old row would resurrect after restart (replay has both)
                        std::string drec = encode_record_delete(kv.first);
                        wal_record(t, drec);
                    }
                    erase_row(t, kv.first);
                    Row copy = kv.second;
                    std::string rec = encode_record_insert(npk, copy);
                    Loc loc{ t.file_off, (uint32_t)rec.size() };
                    wal_record(t, rec);
                    put_row(t, npk, loc, std::make_shared<Row>(std::move(kv.second)));
                }
            }
            Result r;
            r.has_affected = true;
            r.affected_rows = (int64_t)updates.size();
            return r;
        }
        case StmtType::CREATE_VECTOR_INDEX: {
            Table& t = *get_table(st.table);
            int idx = t.schema.find_col(st.index_col);
            if (idx < 0) throw SqlError(errcode::COL_MISSING, "unknown column: " + st.index_col);
            if (t.schema.cols[idx].type != ColType::VECTOR)
                throw SqlError(errcode::INVALID_VALUE, "vector index requires a VECTOR column: " + st.index_col);
            for (const auto& ix : t.indexes)
                if (Schema::ieq(ix.first, st.index_name))
                    throw SqlError(errcode::INDEX_EXISTS, "index already exists: " + st.index_name);
            t.indexes.emplace_back(st.index_name, st.index_col);
            save_meta(t); // index definition persists across restarts
            return {};
        }
        case StmtType::DROP_VECTOR_INDEX: {
            bool dropped = false;
            auto drop_from = [&](Table& t) {
                for (auto it = t.indexes.begin(); it != t.indexes.end(); ++it) {
                    if (Schema::ieq(it->first, st.index_name)) {
                        t.indexes.erase(it);
                        save_meta(t);
                        dropped = true;
                        return true;
                    }
                }
                return false;
            };
            if (!st.table.empty()) {
                Table& t = *get_table(st.table);
                drop_from(t);
            } else {
                for (auto& kv : tables_)
                    if (drop_from(*kv.second)) break;
            }
            if (!dropped) throw SqlError(errcode::INDEX_MISSING, "index not found: " + st.index_name);
            return {};
        }
        case StmtType::SELECT: {
            Table& t = *get_table(st.table);
            const Schema& sch = t.schema;
            bind_expr(st.where.get(), sch);

            // resolve projection
            struct Proj { bool is_l2; int idx; std::string name; const std::vector<float>* vec; };
            std::vector<Proj> proj;
            if (st.select_star) {
                for (const auto& c : sch.cols) proj.push_back({false, sch.find_col(c.name), c.name, nullptr});
            } else {
                for (const auto& it : st.select_items) {
                    if (it.is_l2) {
                        int idx = sch.find_col(it.l2_col_name);
                        if (idx < 0) throw SqlError(errcode::COL_MISSING, "unknown column: " + it.l2_col_name);
                        const ColumnDef& cd = sch.cols[idx];
                        if (cd.type != ColType::VECTOR)
                            throw SqlError(errcode::INVALID_VALUE, "L2_DISTANCE expects a VECTOR column");
                        if ((int)it.l2_vec.size() != cd.vec_dim)
                            throw SqlError(errcode::DIM_MISMATCH, "L2_DISTANCE vector dimension mismatch");
                        std::string name = it.alias.empty() ? ("l2_distance(" + cd.name + ")") : it.alias;
                        proj.push_back({true, idx, name, &it.l2_vec});
                    } else {
                        int idx = sch.find_col(it.col_name);
                        if (idx < 0) throw SqlError(errcode::COL_MISSING, "unknown column: " + it.col_name);
                        std::string name = it.alias.empty() ? sch.cols[idx].name : it.alias;
                        proj.push_back({false, idx, name, nullptr});
                    }
                }
            }

            // alias map for ORDER BY referencing projection aliases (e.g. ORDER BY distance)
            std::map<std::string, const SelectItem*> alias_map;
            if (!st.select_star) {
                for (const auto& it : st.select_items) {
                    if (!it.alias.empty()) {
                        std::string k = lower_name(it.alias);
                        alias_map[k] = &it;
                    }
                }
            }

            // order keys resolution
            struct Ord { bool is_l2; int idx; bool desc; const std::vector<float>* vec; };
            std::vector<Ord> ord;
            for (const auto& it : st.order_by) {
                if (it.is_l2) {
                    int idx = sch.find_col(it.l2_col_name);
                    if (idx < 0) throw SqlError(errcode::COL_MISSING, "unknown column: " + it.l2_col_name);
                    const ColumnDef& cd = sch.cols[idx];
                    if (cd.type != ColType::VECTOR)
                        throw SqlError(errcode::INVALID_VALUE, "ORDER BY expects a VECTOR column for l2_distance");
                    if ((int)it.l2_vec.size() != cd.vec_dim)
                        throw SqlError(errcode::DIM_MISMATCH, "L2_DISTANCE vector dimension mismatch");
                    ord.push_back({true, idx, it.desc, &it.l2_vec});
                } else {
                    // resolve alias first (ORDER BY distance), then real columns
                    auto ait = alias_map.find(lower_name(it.col_name));
                    if (ait != alias_map.end() && ait->second->is_l2) {
                        const SelectItem& sit = *ait->second;
                        int idx = sch.find_col(sit.l2_col_name);
                        const ColumnDef& cd = sch.cols[idx];
                        if ((int)sit.l2_vec.size() != cd.vec_dim)
                            throw SqlError(errcode::DIM_MISMATCH, "L2_DISTANCE vector dimension mismatch");
                        ord.push_back({true, idx, it.desc, &sit.l2_vec});
                    } else if (ait != alias_map.end()) {
                        int idx = sch.find_col(ait->second->col_name);
                        if (idx < 0) throw SqlError(errcode::COL_MISSING, "unknown column: " + ait->second->col_name);
                        ord.push_back({false, idx, it.desc, nullptr});
                    } else {
                        int idx = sch.find_col(it.col_name);
                        if (idx < 0) throw SqlError(errcode::COL_MISSING, "unknown column: " + it.col_name);
                        ord.push_back({false, idx, it.desc, nullptr});
                    }
                }
            }

            // order key: exact comparisons (int64 for BIGINT, string for VARCHAR, double otherwise)
            struct Key {
                int kind = 0; // 0=double, 1=int64, 2=string
                double d = 0;
                int64_t i = 0;
                std::string s;
            };
            auto key_from_value = [](const Value& v) {
                Key k;
                if (v.null) { k.kind = 0; k.d = 0; return k; } // same as previous behavior for NULL scalars
                if (v.type == ColType::BIGINT) { k.kind = 1; k.i = v.i; }
                else if (v.type == ColType::DOUBLE) { k.kind = 0; k.d = v.d; }
                else if (v.type == ColType::VARCHAR) { k.kind = 2; k.s = v.s; }
                else { k.kind = 0; k.d = 0; }
                return k;
            };
            auto key_less = [](const Key& x, const Key& y) {
                if (x.kind != y.kind) return x.kind < y.kind;
                if (x.kind == 1) return x.i < y.i;
                if (x.kind == 2) return x.s < y.s;
                return x.d < y.d;
            };

            // gather + filter — memory-bounded: keep only (order keys, pk) refs, never
            // full row copies; rows are re-fetched on demand for the output slice.
            struct Ref { int64_t pk; std::vector<Key> keys; };
            auto compute_keys = [&](const Row& row, Ref& r) {
                r.keys.reserve(ord.size());
                for (const auto& o : ord) {
                    const Value& v = row[o.idx];
                    if (o.is_l2) {
                        Key k; k.kind = 0;
                        k.d = v.null ? 1e30 : (double)l2_distance(v.v.data(), o.vec->data(), v.v.size());
                        r.keys.push_back(k);
                    } else {
                        r.keys.push_back(key_from_value(v));
                    }
                }
            };
            auto cand_less = [&](const Ref& a, const Ref& b) {
                for (size_t i = 0; i < ord.size(); ++i) {
                    const Key& x = a.keys[i];
                    const Key& y = b.keys[i];
                    bool xl = key_less(x, y), yl = key_less(y, x);
                    if (xl != yl) return ord[i].desc ? yl : xl;
                }
                return a.pk < b.pk;
            };

            bool topk_path = ord.size() == 1 && ord[0].is_l2 && !ord[0].desc && st.limit >= 0;
            std::vector<Ref> refs;
            if (topk_path && st.limit > 0) {
                // bounded heap: keep only the best `limit` refs (max-heap, top = worst kept)
                size_t k = (size_t)st.limit;
                for_each_visible(t, [&](int64_t pk, const Row& row, const Entry*) {
                    if (!eval_where(st.where.get(), row, sch)) return;
                    Ref r; r.pk = pk;
                    compute_keys(row, r);
                    if (refs.size() < k) {
                        refs.push_back(std::move(r));
                        std::push_heap(refs.begin(), refs.end(), cand_less);
                    } else if (cand_less(r, refs.front())) {
                        std::pop_heap(refs.begin(), refs.end(), cand_less);
                        refs.back() = std::move(r);
                        std::push_heap(refs.begin(), refs.end(), cand_less);
                    }
                });
                std::sort(refs.begin(), refs.end(), cand_less);
            } else {
                for_each_visible(t, [&](int64_t pk, const Row& row, const Entry*) {
                    if (!eval_where(st.where.get(), row, sch)) return;
                    Ref r; r.pk = pk;
                    if (!ord.empty()) compute_keys(row, r);
                    refs.push_back(std::move(r));
                });
                if (!ord.empty()) std::sort(refs.begin(), refs.end(), cand_less);
                else std::sort(refs.begin(), refs.end(),
                               [](const Ref& a, const Ref& b) { return a.pk < b.pk; });
            }

            Result res;
            res.is_select = true;
            res.header.reserve(proj.size());
            for (const auto& p : proj) res.header.push_back(p.name);
            size_t nout = (st.limit >= 0) ? std::min((size_t)st.limit, refs.size()) : refs.size();
            res.rows.reserve(nout);
            const std::string tkey = lower_name(t.schema.name);
            for (size_t i = 0; i < nout; ++i) {
                const Ref& ref = refs[i];
                // re-fetch the row on demand (txn-staged via overlay, committed via disk)
                const Row* crow = nullptr;
                std::shared_ptr<Row> keep;
                if (in_txn_) {
                    auto oit = staged_.find(tkey);
                    if (oit != staged_.end()) {
                        auto vit = oit->second.find(ref.pk);
                        if (vit != oit->second.end()) {
                            if (vit->second) crow = &*vit->second;
                            else throw SqlError(errcode::INVALID_VALUE, "row deleted during select");
                        }
                    }
                }
                if (!crow) {
                    keep = read_row(t, ref.pk);
                    if (!keep) throw SqlError(errcode::INVALID_VALUE, "row vanished during select");
                    crow = keep.get();
                }
                Row r;
                r.reserve(proj.size());
                for (size_t j = 0; j < proj.size(); ++j) {
                    const Proj& p = proj[j];
                    if (p.is_l2) {
                        const Value& v = (*crow)[p.idx];
                        Value out;
                        out.type = ColType::DOUBLE;
                        if (v.null) { out.null = true; }
                        else {
                            bool reuse_key = !ord.empty() && ord[0].is_l2 && ord[0].idx == p.idx &&
                                             ref.keys.size() == 1;
                            out.d = reuse_key ? ref.keys[0].d
                                              : (double)l2_distance(v.v.data(), p.vec->data(), v.v.size());
                        }
                        r.push_back(std::move(out));
                    } else {
                        r.push_back((*crow)[p.idx]);
                    }
                }
                res.rows.push_back(std::move(r));
            }
            return res;
        }
    }
    throw SqlError(errcode::UNSUPPORTED, "internal error: unknown statement type");
}
