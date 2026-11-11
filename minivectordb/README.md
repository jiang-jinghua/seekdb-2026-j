# MiniVectorDB

2026 OceanBase 数据库大赛赛题一《完成一个轻量向量数据库》的 C++ 实现。

单机、单进程、单客户端向量数据库，符合官方 `2026-competition-round2` 的 **JSONL stdio 协议**（见 `oceanbase/seekdb` 仓库根目录 `STDIO_PROTOCOL.md`）：stdin 逐行接收 JSON 请求，stdout 逐行返回 JSON 响应（每行立即 flush），日志写 stderr，不监听网络。核心存储、索引、查询引擎自行实现，无第三方依赖。

## 构建

```bash
make                    # 或 cmake -B build && cmake --build build
./minivectordb --data-dir /data
```

## 协议实现

- 启动完成恢复后，先输出 `{"type":"ready"}` 再等第一条请求（每次启动都会输出）
- 请求：`{"id":1,"op":"execute","sql":"...","search_mode":"exact"|"ann"}`，`id` 按其 JSON 类型原样回传
- 查询成功：`{"id":..,"ok":true,"columns":[..],"rows":[[..]]}`（空结果也返回列名）
- DML 成功：`{"id":..,"ok":true,"affected_rows":N}`
- 失败：`{"id":..,"ok":false,"error":{"code":"...","message":"..."}}`，预期错误不终止进程
- `op":"shutdown"`：回滚未提交事务、落盘 fsync、响应后退出码 0，之后 stdout 无任何输出
- BIGINT 以 JSON 整数精确输出（>2^53 不丢精度）；向量 float32 语义；浮点比较容差 1e-5

## 功能

- **SQL**：`CREATE TABLE`/`DROP TABLE`/`INSERT`（多行）/`DELETE`/`UPDATE`/`SELECT`；`BIGINT` 主键（有且仅有一个，可为负/超 2^53）、`VARCHAR(n)`（UTF-8）、`VECTOR(n)`
- **条件查询**：谓词过滤（`AND/OR/NOT`、`= != <> < <= > >=`）、`ORDER BY ... [ASC|DESC]`（支持投影别名 `AS distance`）、`LIMIT`
- **向量检索**：`L2_DISTANCE(col, '[..]')` 返回欧氏距离（sqrt、float32 语义）；`ORDER BY distance LIMIT k` 精确 TopK（partial_sort，AVX2/SSE2 SIMD 分派），先 WHERE 过滤再取 TopK；**距离并列按主键升序**；带 `AS` 别名投影
- **向量索引**：`CREATE VECTOR INDEX name ON t(col)` / `DROP VECTOR INDEX name [ON t]`；索引定义持久化（重启保留）；检索语义与精确扫描一致
- **事务**：`BEGIN`/`COMMIT`/`ROLLBACK`，单语句自动提交；**语句级原子性**（失败语句不留部分效果，事务可继续）；未提交事务不生效
- **恢复**：每次提交先 write() 进 WAL（进程 SIGKILL 不丢），fsync 批量化；启动重放 WAL（CRC32 校验、损坏尾部截断）；`--data-dir` 指定数据目录，不无条件清空

## 错误码

已对齐公开用例的语义名：`duplicate_key`、`unsupported_sql`、`dimension_mismatch`。其余为合理猜测（`syntax_error`、`table_not_found`、`table_already_exists`、`unknown_column`、`invalid_vector`、`invalid_value`、`index_already_exists`、`index_not_found`），正式错误码映射以题包发布为准，改 `src/common.h` 的 `errcode` 一处即可。

## 本地自测

```bash
cd official_cases
python run_local.py                 # 跑全部 20 个官方公开用例（模拟官方 protocol_check/run.py）
python run_local.py sql_crud        # 单个用例
python kill9_test.py                # kill -9 崩溃恢复测试
```

`official_cases/` 内含按官方 `mysqltest/cases` 转录的 20 个公开用例（`make_cases.py` 生成）。正式评测须以官方仓库实际文件为准。

当前状态：**公开用例 20/20 通过，kill -9 崩溃恢复通过。**

## 已知边界（后续优化方向）

1. **全内存行存储**：正式要求"能处理超过内存限额的数据"，当前实现全量常驻内存，数据集大于内存配额时需改为页式/磁盘驻留存储
2. **ANN 模式**：`search_mode: ann` 目前仍走精确扫描（结果正确但不提速）；正式评测有召回率要求，可引入 IVF-Flat + 精确 rerank 保证结果一致
3. **性能优化**：benchmark 维度（写入/过滤 TopK 的耗时、I/O、内存）——可用紧凑列式存储降内存、批量 INSERT 合并编码、WAL 压缩
4. 错误码映射待官方冻结后校正

## 环境变量（调优用，非协议）

- `MVDB_FSYNC=1`：每条语句提交都 fsync（最严格，慢）
- `MVDB_SYNC_EVERY=N`：每 N 条语句 fsync 一次（默认 64）
- `MVDB_PROFILE=1`：stderr 输出解析/执行耗时统计
