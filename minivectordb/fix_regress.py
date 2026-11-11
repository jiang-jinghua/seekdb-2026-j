#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Regression tests for the 2026-11-11 bugfixes (JSONL protocol driver)."""
import json, os, shutil, subprocess, sys, tempfile

BIN = r"D:\seekdb-2026-j\minivectordb\mvd_test.exe"
fails = 0

def run_case(sqls, use_dir=None):
    """sqls: list of str or ('restart',). Returns list of responses."""
    d = use_dir or tempfile.mkdtemp(prefix="mvdreg_")
    if use_dir is None:
        os.makedirs(d, exist_ok=True)
    p = subprocess.Popen([BIN, "--data-dir", d], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ready = p.stdout.readline()
    assert json.loads(ready)["type"] == "ready", ready
    out = []
    for i, s in enumerate(sqls, 1):
        if s == ('restart',):
            p.stdin.write((json.dumps({"id": i, "op": "shutdown"}) + "\n").encode())
            p.stdin.flush()
            r = json.loads(p.stdout.readline())
            assert r.get("ok") is True, r
            p.wait(timeout=10)
            p = subprocess.Popen([BIN, "--data-dir", d], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            ready = p.stdout.readline()
            assert json.loads(ready)["type"] == "ready", ready
            continue
        p.stdin.write((json.dumps({"id": i, "op": "execute", "sql": s, "search_mode": "exact"}) + "\n").encode())
        p.stdin.flush()
        out.append(json.loads(p.stdout.readline()))
    p.stdin.close()
    try:
        p.wait(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
    return out, d

def check(name, cond, detail=""):
    global fails
    if cond:
        print("PASS  " + name)
    else:
        fails += 1
        print("FAIL  " + name + ("  | " + str(detail) if detail else ""))

# ---- T1: PK not in first column + restart recovery (encode_record_insert pk fix) ----
sqls = [
    "CREATE TABLE t1 (name VARCHAR(8), id BIGINT PRIMARY KEY);",
    "INSERT INTO t1 VALUES ('a', 1);",
    "INSERT INTO t1 VALUES ('b', 2);",
    ('restart',),
    "SELECT id, name FROM t1 ORDER BY id;",
]
out, _ = run_case(sqls)
r = out[-1]
check("T1 pk-not-first WAL recovery", r.get("ok") and r.get("rows") == [[1, "a"], [2, "b"]], r)

# ---- T2: UPDATE changing pk + restart (WAL delete-old-pk fix) ----
sqls = [
    "CREATE TABLE t2 (id BIGINT PRIMARY KEY, v VARCHAR(8));",
    "INSERT INTO t2 VALUES (1, 'x');",
    "INSERT INTO t2 VALUES (2, 'y');",
    "UPDATE t2 SET id = 10 WHERE id = 1;",
    ('restart',),
    "SELECT id, v FROM t2 ORDER BY id;",
]
out, _ = run_case(sqls)
r = out[-1]
check("T2 update-pk WAL recovery", r.get("ok") and r.get("rows") == [[2, "y"], [10, "x"]], r)

# ---- T3: WHERE on BIGINT beyond 2^53 (exact int64 comparison) ----
big = 9223372036854775806
sqls = [
    "CREATE TABLE t3 (id BIGINT PRIMARY KEY);",
    f"INSERT INTO t3 VALUES ({big - 1});",
    f"INSERT INTO t3 VALUES ({big});",
    f"SELECT id FROM t3 WHERE id = {big};",
    f"SELECT id FROM t3 WHERE id > {big - 1};",
]
out, _ = run_case(sqls)
check("T3a bigint eq", out[-2].get("rows") == [[big]], out[-2])
check("T3b bigint gt", out[-1].get("rows") == [[big]], out[-1])

# ---- T4: ORDER BY VARCHAR uses lexicographic order ----
sqls = [
    "CREATE TABLE t4 (id BIGINT PRIMARY KEY, name VARCHAR(16));",
    "INSERT INTO t4 VALUES (1, 'bbb');",
    "INSERT INTO t4 VALUES (2, 'a');",
    "INSERT INTO t4 VALUES (3, 'ccc');",
    "INSERT INTO t4 VALUES (4, 'aa');",
    "SELECT name FROM t4 ORDER BY name;",
    "SELECT name FROM t4 ORDER BY name DESC;",
]
out, _ = run_case(sqls)
check("T4a varchar asc lexicographic",
      out[-2].get("rows") == [["a"], ["aa"], ["bbb"], ["ccc"]], out[-2])
check("T4b varchar desc lexicographic",
      out[-1].get("rows") == [["ccc"], ["bbb"], ["aa"], ["a"]], out[-1])

# ---- T5: VARCHAR(n) counts characters, not bytes ----
sqls = [
    "CREATE TABLE t5 (id BIGINT PRIMARY KEY, s VARCHAR(4));",
    "INSERT INTO t5 VALUES (1, '你好世界');",   # 4 chars, 12 bytes -> must succeed
    "INSERT INTO t5 VALUES (2, '你好世界啊');", # 5 chars -> must fail
    "SELECT s FROM t5 ORDER BY id;",
]
out, _ = run_case(sqls)
check("T5a varchar 4 chinese chars ok", out[1].get("ok") is True, out[1])
check("T5b varchar 5 chinese chars rejected", out[2].get("ok") is False, out[2])
check("T5c varchar select", out[3].get("rows") == [["你好世界"]], out[3])

# ---- T6: corrupt WAL tail is truncated, process still starts ----
d = tempfile.mkdtemp(prefix="mvdreg_")
os.makedirs(d, exist_ok=True)
out, _ = run_case([
    "CREATE TABLE t6 (id BIGINT PRIMARY KEY, v VARCHAR(8));",
    "INSERT INTO t6 VALUES (1, 'a');",
    "INSERT INTO t6 VALUES (2, 'b');",
    "INSERT INTO t6 VALUES (3, 'c');",
    "DELETE FROM t6 WHERE id = 3;",
], use_dir=d)
wal = os.path.join(d, "t6.wal")
with open(wal, "ab") as f:
    f.write(b"\x11\x22\x33\x44" + os.urandom(50))  # garbage tail
out, _ = run_case(["SELECT id FROM t6 ORDER BY id;"], use_dir=d)
check("T6 corrupt WAL tail truncated", out[-1].get("rows") == [[1], [2]], out[-1])
shutil.rmtree(d, ignore_errors=True)

# ---- T7: NULL ordering (NULLs first ascending) ----
sqls = [
    "CREATE TABLE t7 (id BIGINT PRIMARY KEY, v VARCHAR(4));",
    "INSERT INTO t7 VALUES (1, 'b');",
    "INSERT INTO t7 VALUES (2, NULL);",
    "INSERT INTO t7 VALUES (3, 'a');",
    "SELECT id FROM t7 ORDER BY v;",
]
out, _ = run_case(sqls)
check("T7 NULL first ascending", out[-1].get("rows") == [[2], [3], [1]], out[-1])

# ---- T8: statement-level atomicity in txn (already-passing public case) ----
sqls = [
    "CREATE TABLE t8 (id BIGINT PRIMARY KEY, v VARCHAR(4));",
    "BEGIN;",
    "INSERT INTO t8 VALUES (1, 'a');",
    "INSERT INTO t8 VALUES (1, 'dup');",   # fails, only this statement rolls back
    "INSERT INTO t8 VALUES (2, 'b');",
    "COMMIT;",
    "SELECT id FROM t8 ORDER BY id;",
]
out, _ = run_case(sqls)
check("T8 txn failed stmt atomic", out[-1].get("rows") == [[1], [2]], out[-1])

# ---- T9: vector ties ordered by pk; filtered topk (regression) ----
sqls = [
    "CREATE TABLE t9 (id BIGINT PRIMARY KEY, tag VARCHAR(4), v VECTOR(2));",
    "INSERT INTO t9 VALUES (1, 'x', '[0,0]');",
    "INSERT INTO t9 VALUES (2, 'y', '[1,0]');",
    "INSERT INTO t9 VALUES (3, 'x', '[2,0]');",
    "INSERT INTO t9 VALUES (4, 'x', '[1,0]');",
    "SELECT id FROM t9 WHERE tag = 'x' ORDER BY l2_distance(v, '[1,0]') LIMIT 2;",
]
out, _ = run_case(sqls)
# distances: id1=1, id4=0, id3=1 -> tie between 1 and 3 at distance 1 -> pk order [4,1,3] -> top2 [4,1]
check("T9 filter topk ties by pk", out[-1].get("rows") == [[4], [1]], out[-1])

print()
print("RESULT:", "ALL PASS" if fails == 0 else f"{fails} FAILED")
sys.exit(1 if fails else 0)
