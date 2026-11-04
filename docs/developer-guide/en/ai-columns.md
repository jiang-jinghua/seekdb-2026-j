# Asynchronous AI columns

AI columns run the existing `AI_COMPLETE` and `AI_EMBED` functions after the
transaction that changes their inputs commits. They are available in MySQL mode.

```sql
CREATE TABLE documents (
  id BIGINT PRIMARY KEY,
  content VARCHAR(2000),
  tags JSON AI COLUMN (
    AI_COMPLETE('text_model', CONCAT('Classify: ', content))
  ) WITH (sync_mode='async', max_retries=2),
  embedding VECTOR(4) AI COLUMN (
    AI_EMBED('embedding_model', content)
  ) WITH (sync_mode='async', max_retries=2)
);
```

Register the models and endpoints with `DBMS_AI_SERVICE` before using them.
Pending and failed results read as SQL NULL by default. Status is maintained
separately, so a successful NULL response does not imply an unfinished task.

```sql
SHOW AI COLUMNS FROM documents;
SET SESSION ai_column_strict_read = ON;
ALTER TABLE documents RETRY AI COLUMN tags WHERE status = 'FAILED';
```

Strict reads reject a statement if an AI column it references has any unfinished
or failed rows, including when the statement's predicate selects only READY rows.
An unquoted `status` in RETRY addresses the target AI column's state; use backticks
to address an ordinary business column named `status`.

CREATE, ADD and MODIFY preserve the expression, dependencies, retry options and
definition version in the native schema. MODIFY increments the version and
invalidates existing results. Dependencies must reference ordinary columns in
the same table; chained AI columns and synchronous generation are unsupported.
Applications cannot assign AI results directly.

Task insertion, invalidation and source DML use the same native transaction.
The background worker persists retries, recovers interrupted tasks, and publishes
all terminal results for a row's request group in one transaction. Row-key and
request guards prevent deleted or superseded rows from receiving stale results.
Result writeback uses native DML, including vector index maintenance.

The current worker processes one model request at a time. Dependency maintenance
uses set-based SQL over the affected table; large-table throughput and concurrent
worker scaling require further performance work.

## Validation

The Ubuntu build passed these mysqltest cases:

- `ai_column.ai_column_ddl`
- `ai_column.ai_column_t1_dml`
- `ai_column.ai_column_worker`
- `ai_column.ai_column_retry_strict`
- `ai_column.ai_column_vector_index`
- `ai_column.ai_column_definition_lifecycle`

The definition serialization regression also passed AddressSanitizer and
UndefinedBehaviorSanitizer. A separate forced-process-stop check verified that a
committed task survives restart, completes, and is removed from the task table.
These checks use mock endpoints and do not establish hidden-test scores or
production performance guarantees.

Use the VSAG dependency pinned by `deps/init/oceanbase.*.deps`. An older VSAG
archive can compile against mismatched headers and fail during index creation.
