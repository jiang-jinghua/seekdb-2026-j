"""Data-edge probe: the hidden cases differ by DATA, not by grammar.
Check numeric/string/vector boundary behaviour that public cases do not exercise."""
import json, subprocess, tempfile, shutil, threading, queue, sys

BIN = "../minivectordb.exe"

class Sess:
    def __init__(self, d):
        self.p = subprocess.Popen([BIN, "--data-dir", d], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        self._q = queue.Queue()
        threading.Thread(target=lambda: [self._q.put(l) for l in self.p.stdout] or self._q.put(None),
                         daemon=True).start()
        assert '"ready"' in self._q.get(timeout=8)
    def q(self, sql, timeout=15):
        self.p.stdin.write(json.dumps({"id": 1, "op": "execute", "sql": sql}) + "\n")
        self.p.stdin.flush()
        try:
            return json.loads(self._q.get(timeout=timeout))
        except queue.Empty:
            return {"HANG": True}
    def close(self):
        try:
            self.p.stdin.write(json.dumps({"id": 9, "op": "shutdown"}) + "\n")
            self.p.stdin.flush()
            self.p.wait(timeout=8)
        except Exception:
            self.p.kill()

def fmt(r):
    if r.get("HANG"):
        return "HANG"
    if not r.get("ok"):
        return "ERR " + r.get("error", {}).get("code", "?")
    if "rows" in r:
        return "rows=" + json.dumps(r["rows"], ensure_ascii=False)[:90]
    if "affected_rows" in r:
        return "aff=" + str(r["affected_rows"])
    return "ok"

d = tempfile.mkdtemp(prefix="mvdb-edge2-")
try:
    s = Sess(d)
    steps = [
        ("BIGINT boundaries", [
            "CREATE TABLE b (id BIGINT PRIMARY KEY, v BIGINT)",
            "INSERT INTO b VALUES (-9223372036854775808, 9223372036854775807)",
            "INSERT INTO b VALUES (9223372036854775807, -9223372036854775808)",
            "SELECT * FROM b ORDER BY id",
            "SELECT id FROM b WHERE v = -9223372036854775808",
            "SELECT id FROM b WHERE v > 9223372036854775806",
        ]),
        ("BIGINT overflow literal", [
            "INSERT INTO b VALUES (9223372036854775808, 0)",
        ]),
        ("string escapes", [
            "CREATE TABLE s (id BIGINT PRIMARY KEY, t VARCHAR(40))",
            "INSERT INTO s VALUES (1, 'a''b')",
            "INSERT INTO s VALUES (2, 'q\"q')",
            "INSERT INTO s VALUES (3, 'b\\\\b')",
            "INSERT INTO s VALUES (4, '\\ttab')",
            "INSERT INTO s VALUES (5, 'emoji😀')",
            "INSERT INTO s VALUES (6, '')",
            "SELECT * FROM s ORDER BY id",
        ]),
        ("varchar char-vs-byte limit", [
            "CREATE TABLE u (id BIGINT PRIMARY KEY, t VARCHAR(4))",
            "INSERT INTO u VALUES (1, '中文字符')",     # 4 chars / 12 bytes
            "INSERT INTO u VALUES (2, '中文字符串')",   # 5 chars
            "SELECT * FROM u ORDER BY id",
        ]),
        ("duplicate pk inside one multi-row insert", [
            "CREATE TABLE m (id BIGINT PRIMARY KEY, t VARCHAR(8))",
            "INSERT INTO m VALUES (1, 'a'), (2, 'b')",
            "INSERT INTO m VALUES (3, 'c'), (1, 'dup')",
            "SELECT * FROM m ORDER BY id",
        ]),
        ("update to existing pk", [
            "UPDATE m SET id = 2 WHERE id = 1",
            "SELECT * FROM m ORDER BY id",
        ]),
        ("update to same value (affected_rows)", [
            "UPDATE m SET t = 'b' WHERE id = 2",
            "SELECT * FROM m ORDER BY id",
        ]),
        ("vector literal formats", [
            "CREATE TABLE v (id BIGINT PRIMARY KEY, e VECTOR(3))",
            "INSERT INTO v VALUES (1, '[1, 2, 3]')",     # spaces
            "INSERT INTO v VALUES (2, '[1.5, -2, 3e-2]')",
            "INSERT INTO v VALUES (3, '[]')",
            "INSERT INTO v VALUES (4, '[1,2]')",
            "INSERT INTO v VALUES (5, '[nan,0,0]')",
            "INSERT INTO v VALUES (6, '[inf,0,0]')",
            "INSERT INTO v VALUES (7, '[3.5e38,0,0]')",  # float32 overflow
            "INSERT INTO v VALUES (8, '[1e39,0,0]')",
            "SELECT id, L2_DISTANCE(e, '[0, 0, 0]') AS d FROM v ORDER BY id",
        ]),
        ("vector null + query", [
            "CREATE TABLE n (id BIGINT PRIMARY KEY, e VECTOR(2))",
            "INSERT INTO n VALUES (1, NULL), (2, '[0,0]')",
            "SELECT id, L2_DISTANCE(e, '[0,0]') AS d FROM n ORDER BY d LIMIT 5",
            "SELECT id FROM n ORDER BY id",
        ]),
        ("multi-row insert with one bad row (atomic)", [
            "CREATE TABLE m2 (id BIGINT PRIMARY KEY, e VECTOR(2))",
            "INSERT INTO m2 VALUES (1, '[0,0]'), (2, '[1]')",
            "SELECT id FROM m2",
        ]),
        ("float32 rounding of big ints in vector", [
            "CREATE TABLE f (id BIGINT PRIMARY KEY, e VECTOR(1))",
            "INSERT INTO f VALUES (1, '[16777217]')",
            "SELECT id, L2_DISTANCE(e, '[16777216]') AS d FROM f",
        ]),
    ]
    for title, sqls in steps:
        print("==", title)
        for sql in sqls:
            print(f"   {sql[:72]:<74} {fmt(s.q(sql))}")
    s.close()
finally:
    shutil.rmtree(d, ignore_errors=True)
