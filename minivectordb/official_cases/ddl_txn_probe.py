import json, subprocess, tempfile, shutil, threading, queue, sys
BIN = "../minivectordb.exe"

class Sess:
    def __init__(self, d):
        self.p = subprocess.Popen([BIN, "--data-dir", d], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        self._q = queue.Queue()
        threading.Thread(target=lambda: [self._q.put(l) for l in self.p.stdout] or self._q.put(None), daemon=True).start()
        assert '"ready"' in self._q.get(timeout=8)
    def q(self, sql):
        self.p.stdin.write(json.dumps({"id": 1, "op": "execute", "sql": sql}) + "\n"); self.p.stdin.flush()
        try:
            return json.loads(self._q.get(timeout=8))
        except queue.Empty:
            return {"HANG": True}
    def close(self):
        try:
            self.p.stdin.write(json.dumps({"id": 9, "op": "shutdown"}) + "\n"); self.p.stdin.flush()
            self.p.wait(timeout=8)
        except Exception:
            self.p.kill()

ok_all = True
def expect(label, r, cond):
    global ok_all
    good = cond(r)
    print(("PASS " if good else "FAIL ") + label + f" -> {json.dumps(r, ensure_ascii=False)[:110]}")
    if not good: ok_all = False

d = tempfile.mkdtemp(prefix="mvdb-ddltxn2-")
try:
    s = Sess(d)
    s.q("CREATE TABLE base (id BIGINT PRIMARY KEY, g BIGINT, v VECTOR(2))")
    s.q("INSERT INTO base VALUES (1, 10, '[1,2]')")
    # MySQL semantics: DDL implicitly commits; pending DML before DDL is committed too
    s.q("BEGIN")
    s.q("INSERT INTO base VALUES (2, 20, '[3,4]')")          # staged
    expect("create-in-txn", s.q("CREATE TABLE tx1 (id BIGINT PRIMARY KEY)"), lambda r: r.get("ok"))
    expect("insert(2) committed by DDL", s.q("SELECT * FROM base"), lambda r: len(r.get("rows", [])) == 2)
    expect("rollback rest", s.q("ROLLBACK"), lambda r: r.get("ok"))
    expect("tx1 exists after rollback (implicit commit)", s.q("SELECT * FROM tx1"), lambda r: r.get("ok"))
    # DROP TABLE in txn: implicitly committed, table gone even after ROLLBACK
    s.q("BEGIN")
    expect("drop-in-txn", s.q("DROP TABLE tx1"), lambda r: r.get("ok"))
    expect("dropped invisible in txn", s.q("SELECT * FROM tx1"), lambda r: not r.get("ok"))
    expect("rollback", s.q("ROLLBACK"), lambda r: r.get("ok"))
    expect("tx1 still gone after rollback (implicit commit)", s.q("SELECT * FROM tx1"), lambda r: not r.get("ok"))
    # index DDL
    s.q("BEGIN")
    expect("create-index-in-txn", s.q("CREATE VECTOR INDEX ix ON base(v)"), lambda r: r.get("ok"))
    expect("rollback", s.q("ROLLBACK"), lambda r: r.get("ok"))
    expect("index exists after rollback (implicit commit)", s.q("DROP VECTOR INDEX ix ON base"), lambda r: r.get("ok"))
    s.close()
    s = Sess(d)
    expect("base rows persist after restart", s.q("SELECT * FROM base"), lambda r: len(r.get("rows", [])) == 2)
    s.close()
finally:
    shutil.rmtree(d, ignore_errors=True)
print("ddl-txn probe (MySQL semantics):", "ALL PASS" if ok_all else "FAILURES")
sys.exit(0 if ok_all else 1)
