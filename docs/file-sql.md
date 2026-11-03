# 文件即数据库

文件表函数将本机文件映射为动态列，过滤、聚合、排序与 JOIN 仍由 seekdb 原生 SQL 引擎执行，不创建中间业务表。

## 使用

文件路径指 **seekdb 服务器上的路径**。功能默认关闭：

```sql
SET SESSION enable_file_sql = ON;
SELECT * FROM read_csv('/allowed/data.csv', header = true, sample_size = -1);
SELECT * FROM read_json('/allowed/data.jsonl', format = 'newline_delimited');
SELECT * FROM read_ndjson('/allowed/data.jsonl');
SELECT * FROM read_parquet('/allowed/data.parquet');
DESCRIBE SELECT * FROM read_csv('/allowed/data.csv');
SELECT file FROM glob('/allowed/*.csv') ORDER BY file;
```

CSV/JSONL 完整扫描以推断确定的列结构，再以流式方式执行查询；`sample_size` 仅接受 `-1`。CSV 支持 UTF-8/BOM、LF/CRLF、引号、转义、多行字段和重名表头。JSONL 支持扁平对象的字段并集，缺失字段补 NULL，拒绝嵌套对象与数组。可推断 BIGINT、DOUBLE、BOOLEAN、VARCHAR、DATE、DATETIME；Parquet UINT64 保留为 VARCHAR 避免有符号溢出；全 NULL 列使用可空 VARCHAR，超出 BIGINT 范围的整数保留为文本。

Parquet 支持扁平基础类型、可空列、多个 row group、PLAIN/RLE/字典编码，以及未压缩或 Snappy 压缩的数据页（V1/V2）。只解码查询引用的列，按数据页读取；不支持的类型、编码或压缩方式会报错。

路径与选项必须是常量，支持 `delimiter`、`quote`、`escape`、`nullstr` 等 CSV 选项。`glob` 只发现文件并返回 `file` 列，不自动合并多个文件。动态文件查询不缓存执行计划，以便下次执行重新绑定文件结构和快照。

## 导出 CSV

```sql
COPY (SELECT id, name FROM read_csv('/allowed/data.csv') ORDER BY id)
  TO '/allowed/export.csv' (FORMAT CSV, HEADER, NULLSTR 'NULL');
COPY test.my_table TO '/allowed/table.csv' (HEADER, DELIMITER ';');
```

导出逐行/逐批消费原生执行计划。NULL 与空字符串分别编码，目标已存在时报错。先写同目录临时文件，成功后以不覆盖目标的原子操作发布；查询失败或算子销毁时清理临时文件。TEXT/LONGTEXT 使用数据库的 LOB 读取接口获取真实内容。

## 路径与资源限制

读写都必须通过运行时 `secure_file_priv` 校验；空值或 NULL 禁止访问。修改该全局变量遵循 seekdb 已有的 Unix socket 管理限制。输入仅允许本机普通文件，拒绝远程 URL、目录、FIFO 与越界路径；通过安全打开目录/文件和文件快照检查避免错误数据或查询期间文件变化。输入文件始终只读。

每条文本记录最多 16 MiB、最多 4096 列。Parquet 元数据、数据页、字典和扫描缓冲均有上限；超限会返回错误，不将整文件物化到内存。glob 最多发现 10000 个文件。

## 验证

- 官方 mysqltest：`file_sql.file_sql_basic`。
- 关系与导出回归：`file_sql.file_sql_relational`（跨格式 JOIN、GROUP BY/HAVING/LIMIT、COPY NULL/空字符串/引号/换行、禁止覆盖）。
- 读取层独立回归：`unittest/sql/engine/basic/test_file_scan_utils.cpp`。
- Parquet 变体：`make_file_sql_parquet_cases.py OUTPUT_DIR` 使用 PyArrow 生成 8 组 V1/V2、压缩/未压缩、字典/非字典文件及无符号整数边界文件；`test_file_scan_parquet.cpp` 校验所有行、NULL、Unicode、日期、时间和多 row group。

独立 C++ 测试使用 `c++ -std=c++20 -Isrc` 编译，可加 `-fsanitize=address,undefined` 检查内存与未定义行为。mysqltest 继续使用仓库的数据库测试框架和 file_sql 数据准备脚本。
