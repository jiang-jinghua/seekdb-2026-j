import json, subprocess, tempfile, shutil, threading, queue
BIN = "../minivectordb.exe"
d = tempfile.mkdtemp(prefix="mvdb-l2-")
try:
    p = subprocess.Popen([BIN, "--data-dir", d], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    q_ = queue.Queue()
    threading.Thread(target=lambda: [q_.put(l) for l in p.stdout] or q_.put(None), daemon=True).start()
    line = q_.get(timeout=8); assert '"ready"' in line
    def q(sql):
        p.stdin.write(json.dumps({"id":1,"op":"execute","sql":sql})+"\n"); p.stdin.flush()
        return json.loads(q_.get(timeout=8))
    q("CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(8))")
    q("INSERT INTO t VALUES (1, '[3e38,0,0,0,0,0,0,0]'), (2, '[-3e38,0,0,0,0,0,0,0]')")
    r = q("SELECT id, l2_distance(v, '[0,0,0,0,0,0,0,0]') AS d FROM t ORDER BY id")
    print("l2 results:", r["rows"], "ok:", r["ok"])
    p.stdin.write(json.dumps({"id":2,"op":"shutdown"})+"\n"); p.stdin.flush(); q_.get(timeout=8); p.wait(timeout=8)
    print("l2 overflow check done")
finally:
    p.kill()
    shutil.rmtree(d, ignore_errors=True)
