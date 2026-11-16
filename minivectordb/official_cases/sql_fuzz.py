# -*- coding: utf-8 -*-
"""SQL semantics fuzz: random statement sequences, hang detection (8s per request)."""
import json, os, subprocess, tempfile, shutil, threading, queue, random, sys

BIN = os.environ.get("MVDB_BIN", "../minivectordb.exe")

class Dead(Exception):
    pass

class Sess:
    def __init__(self, d):
        self.p = subprocess.Popen([BIN, "--data-dir", d], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        self._lines = queue.Queue()
        threading.Thread(target=self._reader, daemon=True).start()
        r = self._recv()
        assert r.get("type") == "ready", r
    def _reader(self):
        for line in self.p.stdout:
            self._lines.put(line)
        self._lines.put(None)
    def q(self, sql):
        self.p.stdin.write(json.dumps({"id": 1, "op": "execute", "sql": sql}) + "\n")
        self.p.stdin.flush()
        return self._recv()
    def _recv(self):
        try:
            line = self._lines.get(timeout=8)
        except queue.Empty:
            raise Dead("HANG: no response in 8s; last sql pending")
        if line is None:
            raise Dead("process exited unexpectedly")
        return json.loads(line)
    def close(self):
        try:
            self.p.stdin.write(json.dumps({"id": 99, "op": "shutdown"}) + "\n"); self.p.stdin.flush()
            self.p.wait(timeout=8)
        except Exception:
            self.p.kill()

random.seed(42)
COLS = ["id", "g", "s", "v"]
hangs = []
errors = 0
ok = 0

def rand_vec(dim=3):
    return "[" + ",".join(str(random.randint(-9, 9)) for _ in range(dim)) + "]"

def rand_where(table_cols):
    c = random.choice(table_cols)
    k = random.random()
    if c == "v":
        if k < 0.5:
            return f"l2_distance(v, '{rand_vec()}') < {random.uniform(0, 50):.2f}"
        return f"l2_distance(v, '{rand_vec()}') IS NULL"
    op = random.choice(["=", "!=", "<", ">", "<=", ">="])
    val = f"{random.randint(-100, 100)}" if c in ("id", "g") else f"'{random.choice(['a','b','c','你好'])}'"
    if k < 0.1:
        val = "NULL"
    return f"{c} {op} {val}"

def gen_sql(cols):
    k = random.random()
    if k < 0.30:
        rows = ",".join(f"({random.randint(0,200)}, {random.randint(-50,50)}, '{random.choice(['a','b','c'])}', {rand_vec()})"
                        for _ in range(random.randint(1, 5)))
        return f"INSERT INTO t VALUES {rows}"
    if k < 0.45:
        col = random.choice(["g", "s", "v"])
        val = random.randint(-50, 50) if col == "g" else ("'" + random.choice(['x','y']) + "'" if col == "s" else rand_vec())
        return f"UPDATE t SET {col} = {val} WHERE {rand_where(cols)}"
    if k < 0.55:
        return f"DELETE FROM t WHERE {rand_where(cols)}"
    if k < 0.60:
        return random.choice(["BEGIN", "COMMIT", "ROLLBACK"])
    if k < 0.65:
        return f"CREATE VECTOR INDEX idx{random.randint(0,3)} ON t(v)"
    if k < 0.70:
        return f"DROP VECTOR INDEX idx{random.randint(0,3)} ON t"
    # selects
    proj = random.choice(["*", "id, g", "id, l2_distance(v, '%s') AS d" % rand_vec(), "s AS ss, v"])
    order = random.choice(["", f" ORDER BY {random.choice(['id','g','s','d'])}" +
                           random.choice(["", " DESC"]),
                           " ORDER BY l2_distance(v, '%s')" % rand_vec() + random.choice(["", " DESC"])])
    limit = random.choice(["", f" LIMIT {random.randint(0,10)}"])
    where = random.choice(["", f" WHERE {rand_where(cols)}"])
    return f"SELECT {proj} FROM t{where}{order}{limit}"

d = tempfile.mkdtemp(prefix="mvdb-fuzz-")
try:
    for trial in range(6):
        s = Sess(d)
        r = s.q("DROP TABLE IF EXISTS t")
        if not r.get("ok"):
            s.q("DROP TABLE t")
        r = s.q("CREATE TABLE t (id BIGINT PRIMARY KEY, g BIGINT, s VARCHAR(20), v VECTOR(3))")
        assert r.get("ok"), r
        cols = COLS
        for i in range(300):
            sql = gen_sql(cols)
            try:
                r = s.q(sql)
                if r.get("ok"): ok += 1
                else: errors += 1
            except Dead as e:
                hangs.append((trial, i, sql, str(e)))
                print(f"HANG at trial {trial} stmt {i}: {sql}")
                s.p.kill()
                break
        if not hangs:
            s.close()
        else:
            break
finally:
    shutil.rmtree(d, ignore_errors=True)

print(f"\nok={ok} err={errors} hangs={len(hangs)}")
if hangs:
    sys.exit(1)
print("FUZZ ALL CLEAR (no hangs)")
