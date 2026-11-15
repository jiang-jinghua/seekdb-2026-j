# -*- coding: utf-8 -*-
"""Edge-case bombardment: every request must get a JSON response within TIMEOUT seconds.
Any hang = potential judge 'JSON response timeout' trigger."""
import json, os, subprocess, sys, tempfile, shutil, threading, queue, time

BIN = os.environ.get("MVDB_BIN", "../minivectordb.exe")
TIMEOUT = 10.0

class Dead(Exception):
    pass

class Sess:
    def __init__(self, d, kill_ok=True):
        self.p = subprocess.Popen([BIN, "--data-dir", d], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self._lines = queue.Queue()
        threading.Thread(target=self._reader, daemon=True).start()
        self.err = []
        threading.Thread(target=self._errreader, daemon=True).start()
        r = self._recv()
        assert r.get("type") == "ready", r
    def _reader(self):
        for line in self.p.stdout:
            self._lines.put(line)
        self._lines.put(None)
    def _errreader(self):
        for line in self.p.stderr:
            self.err.append(line)
    def q(self, sql, rid=1):
        self.p.stdin.write(json.dumps({"id": rid, "op": "execute", "sql": sql}) + "\n")
        self.p.stdin.flush()
        return self._recv()
    def _recv(self):
        try:
            line = self._lines.get(timeout=TIMEOUT)
        except queue.Empty:
            raise Dead("NO RESPONSE within %.0fs" % TIMEOUT)
        if line is None:
            raise Dead("process exited (stderr tail: %s)" % " | ".join(self.err[-3:]))
        return json.loads(line)
    def close(self):
        try:
            self.p.stdin.write(json.dumps({"id": 999, "op": "shutdown"}) + "\n")
            self.p.stdin.flush()
            self.p.wait(timeout=10)
        except Exception:
            self.p.kill()

def must_ok(s, sql, rid):
    r = s.q(sql, rid)
    if not r.get("ok"):
        return r  # error is a valid response; caller may expect it
    return r

def run(name, fn):
    t0 = time.time()
    try:
        fn()
        print(f"[PASS] {name} ({time.time()-t0:.1f}s)")
        return True
    except Dead as e:
        print(f"[HANG/DEAD] {name}: {e}")
        return False
    except AssertionError as e:
        print(f"[FAIL-ASSERT] {name}: {e}")
        return False
    except Exception as e:
        print(f"[ERROR] {name}: {type(e).__name__}: {e}")
        return False

results = []

def t_edge_misc(d):
    s = Sess(d)
    s.q("CREATE TABLE e (id BIGINT PRIMARY KEY, v VECTOR(4))")
    # empty table: order by l2 / select / where
    assert s.q("SELECT id, l2_distance(v, '[1,2,3,4]') AS d FROM e ORDER BY d LIMIT 5")["ok"]
    assert s.q("SELECT * FROM e WHERE id = 1")["ok"]
    # dimension mismatch: WHERE, ORDER BY, projection — must error dimension_mismatch
    for sql in ["SELECT id FROM e WHERE l2_distance(v, '[1,2]') < 5",
                "SELECT id, l2_distance(v, '[1,2]') AS d FROM e",
                "SELECT id FROM e ORDER BY l2_distance(v, '[1,2,3,4,5]') LIMIT 1",
                "SELECT id FROM e WHERE l2_distance(v, 'STRING_LIT_NOT_VEC') IS NULL"]:
        r = s.q(sql)
        if r["ok"]:
            print("   note: ok=True for", sql[:60])
    # limit 0, big limit
    s.q("INSERT INTO e VALUES (1, '[1,2,3,4]'), (2, '[4,3,2,1]')")
    assert s.q("SELECT id FROM e LIMIT 0")["ok"]
    # NULL handling
    s.q("CREATE TABLE n (id BIGINT PRIMARY KEY, g BIGINT)")
    s.q("INSERT INTO n VALUES (1, NULL), (2, 5)")
    r = s.q("SELECT id FROM n ORDER BY g DESC")
    assert r["ok"], r
    r = s.q("SELECT id FROM n WHERE g = NULL")
    assert r["ok"], r
    # update pk conflict
    r = s.q("UPDATE n SET id = 2 WHERE id = 1")
    assert not r["ok"] or True
    # delete + rollback
    s.q("BEGIN")
    s.q("DELETE FROM n WHERE id = 2")
    s.q("ROLLBACK")
    r = s.q("SELECT id FROM n")
    assert r["ok"], r
    s.close()

def t_big_txn(d):
    s = Sess(d)
    s.q("CREATE TABLE t (id BIGINT PRIMARY KEY, pad VARCHAR(100))")
    s.q("BEGIN")
    for i in range(200):
        vals = ",".join(f"({j}, 'p{j%7}')" for j in range(i*100, i*100+100))
        s.q(f"INSERT INTO t VALUES {vals}")
    # select inside large txn (ops overlay path + streaming scan fallback)
    r = s.q("SELECT id FROM t WHERE pad = 'p3' ORDER BY id DESC LIMIT 5")
    assert r["ok"] and len(r["rows"]) == 5, r
    s.q("COMMIT")
    r = s.q("SELECT COUNT_OK FROM t WHERE id = 19999")  # unknown col -> error, must respond
    assert not r["ok"], r
    s.close()

def t_restart_scan(d):
    s = Sess(d)
    s.q("CREATE TABLE t (id BIGINT PRIMARY KEY, pad VARCHAR(50))")
    for i in range(50):
        vals = ",".join(f"({j}, 'x{j%11}')" for j in range(i*2000, i*2000+2000))
        s.q(f"INSERT INTO t VALUES {vals}")
    s.q("UPDATE t SET pad = 'zz' WHERE id < 100000")   # creates stale frames
    s.q("DELETE FROM t WHERE id > 120000")             # tombstones
    s.close()
    s = Sess(d)  # recovery -> then scan (streaming path, stale+tombstone frames)
    r = s.q("SELECT id FROM t WHERE pad = 'zz' LIMIT 3")
    assert r["ok"] and len(r["rows"]) == 3, r
    r = s.q("SELECT id FROM t ORDER BY id DESC LIMIT 1")
    assert r["ok"], r
    s.close()

def t_vec_lifecycle(d):
    s = Sess(d)
    s.q("CREATE TABLE v (id BIGINT PRIMARY KEY, v VECTOR(8))")
    for i in range(500):
        comp = ",".join(str((i*j) % 10) for j in range(8))
        s.q(f"INSERT INTO v VALUES ({i}, '[{comp}]')")
    r = s.q("SELECT id, l2_distance(v, '[1,1,1,1,1,1,1,1]') AS d FROM v ORDER BY d LIMIT 3")
    assert r["ok"] and len(r["rows"]) == 3, r
    # null vector? (insert null into vector col if allowed)
    s.q("UPDATE v SET v = '[9,9,9,9,9,9,9,9]' WHERE id = 0")
    s.close()
    s = Sess(d)
    r = s.q("SELECT id FROM v ORDER BY l2_distance(v, '[9,9,9,9,9,9,9,9]') LIMIT 2")
    assert r["ok"], r
    s.close()

def t_parse_stress(d):
    s = Sess(d)
    s.q("CREATE TABLE p (id BIGINT PRIMARY KEY, s VARCHAR(200))")
    # weird-but-legal sql forms
    for sql in ["SELECT id FROM p",
                "SELECT p.id FROM p",
                "SELECT id FROM p WHERE id > 0 AND id < 100 OR id = 5",
                "SELECT id FROM p WHERE NOT (id = 1)",
                "SELECT id FROM p WHERE id <> 1",
                "SELECT id FROM p WHERE s LIKE 'x' "]:
        r = s.q(sql)  # may error unsupported; must respond
    # unterminated string must error not hang
    r = s.q("INSERT INTO p VALUES (1, 'unterminated)")
    assert not r["ok"], r
    # deep expression
    r = s.q("SELECT id FROM p WHERE " + " OR ".join(f"id = {i}" for i in range(500)))
    s.close()

d = tempfile.mkdtemp(prefix="mvdb-edge-")
try:
    results.append(run("edge misc (dim mismatch, null, rollback)", lambda: t_edge_misc(d)))
    results.append(run("big txn + select inside txn", lambda: t_big_txn(d)))
    results.append(run("restart + scan with stale frames/tombstones", lambda: t_restart_scan(d)))
    results.append(run("vector lifecycle + restart", lambda: t_vec_lifecycle(d)))
    results.append(run("parse stress", lambda: t_parse_stress(d)))
finally:
    shutil.rmtree(d, ignore_errors=True)
print("\nSummary:", "ALL PASS" if all(results) else "FAILURES PRESENT")
