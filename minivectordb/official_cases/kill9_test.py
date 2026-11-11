#!/usr/bin/env python3
# kill -9 crash recovery test: committed data must survive, uncommitted must vanish
import json, os, subprocess, sys, tempfile, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(os.path.dirname(HERE), "minivectordb.exe")
if not os.path.exists(BIN):
    BIN = os.path.join(os.path.dirname(HERE), "minivectordb")

class Proc:
    def __init__(self, data_dir):
        self.data_dir = data_dir
        self.start()
    def start(self):
        self.p = subprocess.Popen([BIN, "--data-dir", self.data_dir],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, encoding="utf-8")
        line = self.p.stdout.readline().rstrip("\n")
        assert line == '{"type":"ready"}', repr(line)
    def req(self, i, sql):
        self.p.stdin.write(json.dumps({"id": i, "op": "execute", "sql": sql, "search_mode": "exact"}) + "\n")
        self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())
    def kill9(self):
        self.p.kill()          # Windows TerminateProcess ~ SIGKILL
        self.p.wait()
    def stop(self):
        self.req(0, None) if False else None
        self.p.stdin.write(json.dumps({"id": 999999, "op": "shutdown"}) + "\n")
        self.p.stdin.flush()
        self.p.stdout.readline()
        self.p.wait()

tmp = tempfile.mkdtemp(prefix="mvdb-kill-")
data = os.path.join(tmp, "data"); os.makedirs(data)
p = Proc(data)
p.req(1, "CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(4), tag VARCHAR(16))")
for i in range(200):
    r = p.req(10 + i, "INSERT INTO t VALUES (%d, '[%d,1,2,3]', 'committed')" % (i, i))
    assert r.get("ok") is True, r
p.req(500, "BEGIN")
p.req(501, "INSERT INTO t VALUES (1000, '[0,0,0,0]', 'uncommitted')")
p.kill9()  # SIGKILL with an open transaction

p = Proc(data)  # restart from same dir
r = p.req(1, "SELECT COUNT_TAG FROM t")  # expect unknown_column error (sanity of error path)
assert r.get("ok") is False and r["error"]["code"], r
r = p.req(2, "SELECT id FROM t WHERE id = 1000")
assert r["ok"] and r["rows"] == [], "uncommitted row survived kill: %r" % r
r = p.req(3, "SELECT id FROM t WHERE id = 199")
assert r["ok"] and r["rows"] == [[199]], r
r = p.req(4, "SELECT id FROM t WHERE id = 0")
assert r["ok"] and r["rows"] == [[0]], r
# commit something new, kill again mid-transaction
p.req(5, "BEGIN")
p.req(6, "DELETE FROM t WHERE id < 50")
p.kill9()
p = Proc(data)
r = p.req(7, "SELECT id FROM t WHERE id = 49")
assert r["ok"] and r["rows"] == [[49]], "uncommitted delete survived: %r" % r
r = p.req(8, "SELECT id, tag FROM t WHERE id = 100")
assert r["ok"] and r["rows"] == [[100, "committed"]], r
# now commit and hard-kill immediately after ack
p.req(9, "BEGIN")
p.req(10, "DELETE FROM t WHERE id < 50")
p.req(11, "COMMIT")
p.kill9()
p = Proc(data)
r = p.req(12, "SELECT id FROM t WHERE id = 49")
assert r["ok"] and r["rows"] == [], "committed delete lost after kill: %r" % r
r = p.req(13, "SELECT id FROM t WHERE id = 199")
assert r["ok"] and r["rows"] == [[199]], r
print("kill -9 crash recovery: ALL OK (200 committed rows, 2 kills, open-tx kills)")
