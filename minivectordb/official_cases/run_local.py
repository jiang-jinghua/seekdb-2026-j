#!/usr/bin/env python3
# Local runner mimicking official protocol_check/run.py over the 20 public cases.
# Usage: python run_local.py [case_id ...]
import json, os, subprocess, sys, tempfile, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(os.path.dirname(HERE), "minivectordb.exe")
if not os.path.exists(BIN):
    BIN = os.path.join(os.path.dirname(HERE), "minivectordb")

TOL = 1e-5

def split_statements(text):
    """Split .test content into ('directive', x) / ('sql', s) items."""
    items, buf, inq, i = [], [], False, 0
    def flush():
        s = "".join(buf).strip()
        buf.clear()
        while True:
            m = None
            if s.startswith("--restart"): m, rest = "--restart", s[len("--restart"):]
            elif s.startswith("--mode"):
                # consume mode argument
                parts = s.split(None, 2)
                if len(parts) >= 2:
                    items.append(("directive", "--mode " + parts[1]))
                    s = parts[2] if len(parts) > 2 else ""
                    continue
                break
            elif s.startswith("--"):
                # comment to end of segment
                nl = s.find("\n")
                s = s[nl+1:] if nl >= 0 else ""
                continue
            else:
                break
            items.append(("directive", m))
            s = rest.strip()
            if not s: break
        if s: items.append(("sql", s))
    while i < len(text):
        c = text[i]
        if inq:
            buf.append(c)
            if c == "'":
                if i + 1 < len(text) and text[i+1] == "'":
                    buf.append("'"); i += 1
                else:
                    inq = False
        else:
            if c == "'":
                inq = True; buf.append(c)
            elif c == ';':
                flush(); buf.append(' ')  # separator
            else:
                buf.append(c)
        i += 1
    flush()
    return items

def cmp_val(a, b):
    if isinstance(b, (int, float)) and not isinstance(b, bool):
        if a is None or isinstance(a, bool) or isinstance(a, str): return False
        if isinstance(a, int) and isinstance(b, int): return a == b  # BIGINT exact
        fa, fb = float(a), float(b)
        return abs(fa - fb) <= TOL * max(1.0, abs(fb)) or abs(fa - fb) <= TOL
    if isinstance(b, list):
        if not isinstance(a, list) or len(a) != len(b): return False
        return all(cmp_val(x, y) for x, y in zip(a, b))
    return a == b  # str exact, None, bool

def check(resp, exp):
    if "error" in exp:
        if resp.get("ok") is not False: return "expected error, got ok=%r" % (resp.get("ok"),)
        code = (resp.get("error") or {}).get("code", "")
        if not code: return "empty error code"
        return None
    if resp.get("ok") is not True: return "expected success, got %r" % resp
    if "affected_rows" in exp and resp.get("affected_rows") != exp["affected_rows"]:
        return "affected_rows: got %r want %r" % (resp.get("affected_rows"), exp["affected_rows"])
    if "columns" in exp:
        if resp.get("columns") != exp["columns"]:
            return "columns: got %r want %r" % (resp.get("columns"), exp["columns"])
        got, want = resp.get("rows", []), exp.get("rows", [])
        if len(got) != len(want):
            return "row count: got %d want %d" % (len(got), len(want))
        for gi, wi in zip(got, want):
            if len(gi) != len(wi): return "row width mismatch: %r vs %r" % (gi, wi)
            for g, w in zip(gi, wi):
                if not cmp_val(g, w): return "value mismatch: got %r want %r" % (g, w)
    return None

class Proc:
    def __init__(self, data_dir):
        self.data_dir = data_dir
        self.p = None
        self.start()
    def start(self):
        self.p = subprocess.Popen([BIN, "--data-dir", self.data_dir],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, encoding="utf-8")
        line = self.p.stdout.readline().rstrip("\n")
        if line != '{"type":"ready"}':
            raise RuntimeError("bad ready line: %r" % line)
    def request(self, obj):
        self.p.stdin.write(json.dumps(obj, ensure_ascii=False) + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline().rstrip("\n")
        return json.loads(line)
    def stop(self):
        if self.p and self.p.poll() is None:
            try:
                self.request({"id": 0, "op": "shutdown"})
            except Exception:
                self.p.kill()
            try:
                self.p.wait(timeout=10)
            except Exception:
                self.p.kill()
        self.p = None

def run_case(cid):
    with open(os.path.join(HERE, cid + ".test"), encoding="utf-8") as f:
        items = split_statements(f.read())
    with open(os.path.join(HERE, cid + ".result"), encoding="utf-8") as f:
        expected = json.load(f)
    tmp = tempfile.mkdtemp(prefix="mvdb-")
    data_dir = os.path.join(tmp, "data")
    os.makedirs(data_dir)
    proc = Proc(data_dir)
    mode = "exact"
    exp_idx, fails = 0, []
    rid = 0
    try:
        for kind, val in items:
            if kind == "directive":
                if val == "--restart":
                    proc.stop()
                    proc = Proc(data_dir)
                elif val.startswith("--mode"):
                    mode = val.split()[1]
                continue
            rid += 1
            resp = proc.request({"id": rid, "op": "execute", "sql": val, "search_mode": mode})
            exp = expected[exp_idx]; exp_idx += 1
            err = check(resp, exp)
            if err:
                fails.append("stmt %d: %s\n  sql: %s\n  resp: %s" % (exp_idx, err, val[:120], json.dumps(resp, ensure_ascii=False)[:300]))
    finally:
        proc.stop()
        shutil.rmtree(tmp, ignore_errors=True)
    return fails

def main():
    ids = sys.argv[1:]
    if not ids:
        ids = [l.strip() for l in open(os.path.join(HERE, "case_list.txt"), encoding="utf-8") if l.strip()]
    passed = 0
    for cid in ids:
        try:
            fails = run_case(cid)
        except Exception as e:
            print("ERROR  %s: %r" % (cid, e)); continue
        if fails:
            print("FAIL   %s" % cid)
            for f in fails: print("   " + f.replace("\n", "\n   "))
        else:
            passed += 1
            print("PASS   %s" % cid)
    print("\n%d/%d passed" % (passed, len(ids)))
    sys.exit(0 if passed == len(ids) else 1)

if __name__ == "__main__":
    main()
