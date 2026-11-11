#!/usr/bin/env python3
# generate official_cases/*.test and *.result from transcribed official content
import os

D5 = "CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), content VARCHAR(16), embedding VECTOR(2)); "
BASE = "CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(16)); "
SEED = "INSERT INTO docs VALUES (3, '乙', '三', '[0, 2]'), (1, '甲', '一', '[0, 0]'), (2, '甲', '二', '[1, 0]'); "

CASES = {
"sql_crud": (
    BASE + "INSERT INTO docs VALUES (1, '初始'); SELECT id, content FROM docs; UPDATE docs SET content = '更新' WHERE id = 1; SELECT content FROM docs; DELETE FROM docs WHERE id = 1; SELECT id FROM docs;",
    '[{},{"affected_rows":1},{"columns":["id","content"],"rows":[[1,"初始"]]},{"affected_rows":1},{"columns":["content"],"rows":[["更新"]]},{"affected_rows":1},{"columns":["id"],"rows":[]}]'),
"sql_comparisons": (
    "CREATE TABLE docs (id BIGINT PRIMARY KEY, value BIGINT); INSERT INTO docs VALUES (1, 10), (2, 20), (3, 30); SELECT id FROM docs WHERE value >= 20 AND value < 30; SELECT id FROM docs WHERE value = 10;",
    '[{},{"affected_rows":3},{"columns":["id"],"rows":[[2]]},{"columns":["id"],"rows":[[1]]}]'),
"sql_ordering": (
    "CREATE TABLE docs (id BIGINT PRIMARY KEY, value BIGINT); INSERT INTO docs VALUES (3, 30), (1, 10), (2, 20); SELECT id FROM docs ORDER BY value; SELECT id FROM docs ORDER BY value DESC LIMIT 2;",
    '[{},{"affected_rows":3},{"columns":["id"],"rows":[[1],[2],[3]]},{"columns":["id"],"rows":[[3],[2]]}]'),
"sql_empty": (
    BASE + "SELECT id FROM docs; DELETE FROM docs WHERE id = 1;",
    '[{},{"columns":["id"],"rows":[]},{"affected_rows":0}]'),
"sql_primary_key": (
    BASE + "INSERT INTO docs VALUES (1, '初始'); INSERT INTO docs VALUES (1, '重复'); SELECT id, content FROM docs;",
    '[{},{"affected_rows":1},{"error":"duplicate_key"},{"columns":["id","content"],"rows":[[1,"初始"]]}]'),
"sql_unicode": (
    BASE + "INSERT INTO docs VALUES (1, '中文'); SELECT content FROM docs WHERE id = 1;",
    '[{},{"affected_rows":1},{"columns":["content"],"rows":[["中文"]]}]'),
"sql_bigint": (
    "CREATE TABLE docs (id BIGINT PRIMARY KEY, value BIGINT); INSERT INTO docs VALUES (9007199254740993, -9007199254740993); SELECT id, value FROM docs;",
    '[{},{"affected_rows":1},{"columns":["id","value"],"rows":[[9007199254740993,-9007199254740993]]}]'),
"sql_unsupported": (
    BASE + "ALTER TABLE docs ADD extra BIGINT; SELECT id FROM docs;",
    '[{},{"error":"unsupported_sql"},{"columns":["id"],"rows":[]}]'),
"storage_restart": (
    BASE + "INSERT INTO docs VALUES (1, '初始'); --restart SELECT id, content FROM docs;",
    '[{},{"affected_rows":1},{"columns":["id","content"],"rows":[[1,"初始"]]}]'),
"vector_euclidean": (
    "CREATE TABLE docs (id BIGINT PRIMARY KEY, embedding VECTOR(2)); INSERT INTO docs VALUES (1, '[3,4]'); SELECT id, L2_DISTANCE(embedding, '[0,0]') AS distance FROM docs ORDER BY distance LIMIT 1;",
    '[{},{"affected_rows":1},{"columns":["id","distance"],"rows":[[1,5.0]]}]'),
"transaction_commit": (
    BASE + "INSERT INTO docs VALUES (1, '初始'); BEGIN; UPDATE docs SET content = '修改' WHERE id = 1; COMMIT; SELECT id, content FROM docs;",
    '[{},{"affected_rows":1},{},{"affected_rows":1},{},{"columns":["id","content"],"rows":[[1,"修改"]]}]'),
"transaction_rollback": (
    BASE + "INSERT INTO docs VALUES (1, '初始'); BEGIN; UPDATE docs SET content = '修改' WHERE id = 1; ROLLBACK; SELECT id, content FROM docs;",
    '[{},{"affected_rows":1},{},{"affected_rows":1},{},{"columns":["id","content"],"rows":[[1,"初始"]]}]'),
"storage_mutations": (
    D5 + SEED + "UPDATE docs SET embedding = '[3,4]', content = '修改' WHERE id = 1; DELETE FROM docs WHERE id = 2; --restart SELECT id, content FROM docs ORDER BY id; SELECT id, L2_DISTANCE(embedding, '[0, 0]') AS distance FROM docs ORDER BY distance LIMIT 10;",
    '[{},{"affected_rows":3},{"affected_rows":1},{"affected_rows":1},{"columns":["id","content"],"rows":[[1,"修改"],[3,"三"]]},{"columns":["id","distance"],"rows":[[3,2.0],[1,5.0]]}]'),
"storage_multiple_tables": (
    "CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), content VARCHAR(16)); INSERT INTO docs VALUES (3, '乙', '三'), (1, '甲', '一'), (2, '甲', '二'); CREATE TABLE tags (id BIGINT PRIMARY KEY, tag VARCHAR(8)); INSERT INTO tags VALUES (1, '独立'); --restart SELECT id, tag FROM tags; SELECT id FROM docs ORDER BY id;",
    '[{},{"affected_rows":3},{},{"affected_rows":1},{"columns":["id","tag"],"rows":[[1,"独立"]]},{"columns":["id"],"rows":[[1],[2],[3]]}]'),
"vector_ties_and_filter": (
    D5 + "INSERT INTO docs VALUES (8,'keep','八','[1,0]'), (3,'keep','三','[-1,0]'), (1,'skip','一','[0,0]'), (2,'skip','二','[0,0]'); SELECT id, L2_DISTANCE(embedding, '[0,0]') AS distance FROM docs WHERE category = 'keep' ORDER BY distance LIMIT 1; SELECT id, L2_DISTANCE(embedding, '[0,0]') AS distance FROM docs WHERE category = 'keep' ORDER BY distance LIMIT 9; SELECT id, L2_DISTANCE(embedding, '[0,0]') AS distance FROM docs WHERE category = 'absent' ORDER BY distance LIMIT 9;",
    '[{},{"affected_rows":4},{"columns":["id","distance"],"rows":[[3,1.0]]},{"columns":["id","distance"],"rows":[[3,1.0],[8,1.0]]},{"columns":["id","distance"],"rows":[]}]'),
"vector_float32": (
    D5 + "INSERT INTO docs VALUES (1,'x','a','[16777217,0]'),(2,'x','b','[16777216,0]'); SELECT id, L2_DISTANCE(embedding, '[16777216,0]') AS distance FROM docs ORDER BY distance LIMIT 2;",
    '[{},{"affected_rows":2},{"columns":["id","distance"],"rows":[[1,0.0],[2,0.0]]}]'),
"vector_index_maintenance": (
    D5 + SEED + "CREATE VECTOR INDEX docs_vec ON docs(embedding); UPDATE docs SET embedding = '[3,4]' WHERE id = 1; DELETE FROM docs WHERE id = 2; INSERT INTO docs VALUES (4,'甲','四','[0,0]'); SELECT id, L2_DISTANCE(embedding, '[0, 0]') AS distance FROM docs ORDER BY distance LIMIT 10; --restart SELECT id, L2_DISTANCE(embedding, '[0, 0]') AS distance FROM docs ORDER BY distance LIMIT 10; --mode ann SELECT id, L2_DISTANCE(embedding, '[0, 0]') AS distance FROM docs ORDER BY distance LIMIT 10;",
    '[{},{"affected_rows":3},{},{"affected_rows":1},{"affected_rows":1},{"affected_rows":1},{"columns":["id","distance"],"rows":[[4,0.0],[3,2.0],[1,5.0]]},{"columns":["id","distance"],"rows":[[4,0.0],[3,2.0],[1,5.0]]},{"columns":["id","distance"],"rows":[[4,0.0],[3,2.0],[1,5.0]]}]'),
"transaction_failed_insert": (
    D5 + SEED + "BEGIN; UPDATE docs SET content = '保留' WHERE id = 1; INSERT INTO docs VALUES (4,'新','四','[0,0]'),(2,'新','重复','[0,0]'); SELECT id FROM docs ORDER BY id; COMMIT; --restart SELECT id, content FROM docs ORDER BY id;",
    '[{},{"affected_rows":3},{},{"affected_rows":1},{"error":"duplicate_key"},{"columns":["id"],"rows":[[1],[2],[3]]},{},{"columns":["id","content"],"rows":[[1,"保留"],[2,"二"],[3,"三"]]}]'),
"transaction_autocommit_atomic": (
    D5 + SEED + "INSERT INTO docs VALUES (4,'新','四','[0,0]'),(5,'新','错误','[0]'); --restart SELECT id FROM docs ORDER BY id;",
    '[{},{"affected_rows":3},{"error":"dimension_mismatch"},{"columns":["id"],"rows":[[1],[2],[3]]}]'),
"transaction_shutdown_rollback": (
    "CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), content VARCHAR(16)); INSERT INTO docs VALUES (3, '乙', '三'), (1, '甲', '一'), (2, '甲', '二'); BEGIN; DELETE FROM docs WHERE id >= 1; --restart SELECT id FROM docs ORDER BY id;",
    '[{},{"affected_rows":3},{},{"affected_rows":3},{"columns":["id"],"rows":[[1],[2],[3]]}]'),
}

here = os.path.dirname(os.path.abspath(__file__))
n = 0
for cid, (test, result) in CASES.items():
    with open(os.path.join(here, cid + ".test"), "w", encoding="utf-8") as f:
        f.write(test + "\n")
    with open(os.path.join(here, cid + ".result"), "w", encoding="utf-8") as f:
        f.write(result + "\n")
    n += 2
print("wrote", n, "files for", len(CASES), "cases")
