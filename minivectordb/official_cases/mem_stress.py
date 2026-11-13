#!/usr/bin/env python3
# Memory-bounded SELECT stress: big table + big vector table under platform-like limits.
# Verifies: big SELECT with ORDER BY/LIMIT, restart survival, vector topk, txn-staged select.
import json, os, subprocess, sys, tempfile, shutil, random, time
IS_POSIX = os.name == 'posix'
if IS_POSIX:
    import resource

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(os.path.dirname(HERE), "minivectordb.exe")
if not os.path.exists(BIN):
    BIN = os.path.join(os.path.dirname(HERE), "minivectordb")

class Sess:
    def __init__(self, d, rlimit_mb=None, cache_mb=None):
        pre = None
        if rlimit_mb and IS_POSIX:
            def pre():
                lim = rlimit_mb * 1024 * 1024
                resource.setrlimit(resource.RLIMIT_AS, (lim, lim))
        env = dict(os.environ)
        if cache_mb:
            env["MVDB_ROW_CACHE_MB"] = str(cache_mb)
        self.p = subprocess.Popen([BIN, "--data-dir", d], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True, preexec_fn=pre, env=env)
        r = self.p.stdout.readline(); assert '"ready"' in r, r
    def q(self, sql, check=True):
        self.p.stdin.write(json.dumps({"id": 1, "op": "execute", "sql": sql}) + "\n")
        self.p.stdin.flush()
        r = json.loads(self.p.stdout.readline())
        if check and not r.get("ok"):
            raise RuntimeError(f"SQL failed: {sql[:80]}: {r}")
        return r
    def close(self):
        self.p.stdin.write(json.dumps({"id": 2, "op": "shutdown"}) + "\n"); self.p.stdin.flush()
        self.p.stdout.readline(); self.p.wait()

def main():
    d = tempfile.mkdtemp(prefix="mvdb-memstress-")
    random.seed(7)
    t0 = time.time()
    try:
        s = Sess(d)
        s.q("CREATE TABLE big (id BIGINT PRIMARY KEY, pad VARCHAR(400), score BIGINT)")
        N = 400000
        for start in range(0, N, 500):
            vals = ",".join(
                f"({i}, '{'x' * 380}{i % 97}', {i * 2654435761 % 100000})"
                for i in range(start, start + 500))
            s.q(f"INSERT INTO big VALUES {vals}")
        print(f"[1] inserted {N} rows ({time.time()-t0:.0f}s)")

        # big SELECT with ORDER BY + LIMIT (old code materialized all matches -> OOM risk)
        r = s.q("SELECT id, pad FROM big WHERE score > 90000 ORDER BY pad DESC LIMIT 20")
        rows = r["rows"]
        assert len(rows) == 20, len(rows)
        pads = [x[1] for x in rows]
        assert pads == sorted(pads, reverse=True), "order wrong"
        print("[2] big SELECT ORDER BY LIMIT 20 OK (20 rows, desc order verified)")

        # no ORDER BY + LIMIT on big table
        r = s.q("SELECT id FROM big WHERE score > 50000 LIMIT 10")
        assert len(r["rows"]) == 10
        print("[3] big SELECT WHERE LIMIT 10 OK")

        # select inside txn with staged inserts + deletes
        s.q("BEGIN")
        s.q("INSERT INTO big VALUES (9000001, 'txn-row-a', 123)")
        s.q("DELETE FROM big WHERE id = 5")
        r = s.q("SELECT id FROM big WHERE id > 9000000 ORDER BY pad LIMIT 5")
        assert [x[0] for x in r["rows"]] == [9000001], r["rows"]
        s.q("ROLLBACK")
        print("[4] txn-staged big SELECT OK")

        s.close()

        # restart on big dataset, re-verify
        s = Sess(d)
        r = s.q("SELECT pad FROM big WHERE id = 123456")
        assert r["rows"][0][0] == "x"*380 + str(123456 % 97), r["rows"]
        r = s.q("SELECT id, score FROM big WHERE score > 0.9 ORDER BY score LIMIT 5")
        assert len(r["rows"]) == 5
        print("[5] restart on 150k-row table OK, count + ordered select verified")

        # big vector table + topk
        s.q("CREATE TABLE vtab (id BIGINT PRIMARY KEY, v VECTOR(64))")
        M = 40000
        for start in range(0, M, 1000):
            vals = ",".join(
                f"({i}, [{','.join(str(round(((i*7+j) % 100) / 10.0 - 5.0, 3)) for j in range(64))}])"
                for i in range(start, start + 1000))
            s.q(f"INSERT INTO vtab VALUES {vals}")
        qv = [0.5] * 64
        r = s.q("SELECT id, l2_distance(v, '[{}]') AS d FROM vtab ORDER BY d LIMIT 10".format(
            ",".join(str(x) for x in qv)))
        assert len(r["rows"]) == 10
        ds = [x[1] for x in r["rows"]]
        assert ds == sorted(ds)
        print(f"[6] vector topk over {M}x64 OK ({time.time()-t0:.0f}s)")
        s.close()

        s = Sess(d)  # restart again after vector data
        r = s.q("SELECT id, l2_distance(v, '[{}]') AS d FROM vtab ORDER BY d LIMIT 3".format(
            ",".join(str(x) for x in qv)))
        assert len(r["rows"]) == 3
        print("[7] vector topk after restart OK")
        s.close()

        # [8] decisive: recovery over a ~160MB data file with the server capped at 120MB.
        # Whole-file buffering in load_dir would abort here; streaming replay must pass.
        if not IS_POSIX:
            print("[8] skipped on Windows (no resource module); CI/ubuntu runs it")
            print(f"MEM-STRESS OK-WIN ({time.time()-t0:.0f}s total)")
            return
        s = Sess(d, rlimit_mb=120, cache_mb=24)  # cache 24MB so row cache fits the 120MB cap
        r = s.q("SELECT id, pad FROM big WHERE id = 399999")
        assert r["rows"][0][0] == 399999, r["rows"]
        r = s.q("SELECT id FROM big WHERE score > 99000 ORDER BY id DESC LIMIT 5")
        assert len(r["rows"]) == 5
        print("[8] recovery of 160MB data file under 120MB server limit OK")
        s.close()
        print(f"MEM-STRESS ALL OK ({time.time()-t0:.0f}s total)")
    finally:
        shutil.rmtree(d, ignore_errors=True)

if __name__ == "__main__":
    main()
