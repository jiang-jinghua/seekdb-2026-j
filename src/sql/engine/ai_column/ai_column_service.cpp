/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/ai_column/ai_column_service.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "lib/thread/thread_pool.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/resolver/ddl/ai_column_ddl.h"
#include "sql/session/ob_inner_sql_connection.h"
#include <atomic>
#include <mutex>
#include <optional>
#include <unistd.h>

namespace oceanbase {
namespace sql {
namespace ai_column {
using namespace common;
using namespace share::schema;
using Connection = common::sqlclient::ObISQLConnection;
using ConnectionGuard = common::sqlclient::ObISQLConnectionGuard;
using Value = std::optional<std::string>;
using Row = std::vector<Value>;
using Rows = std::vector<Row>;
namespace {
thread_local bool internal = false;
struct InternalGuard {
  bool saved = internal;
  InternalGuard() { internal = true; }
  ~InternalGuard() { internal = saved; }
};
std::mutex metadata_mutex;
std::atomic<bool> initialized{false};
const std::string META = "oceanbase.__all_ai_column";
const std::string TASKS = "oceanbase.__all_ai_column_task";
std::string ident(const std::string &s) {
  return Definition::quote_identifier(s);
}
std::string literal(const std::string &s) {
  return Definition::quote_literal(s);
}
std::string decimal(uint64_t n) { return std::to_string(n); }
int fail(const std::exception &error) {
  const int ret = OB_INVALID_ARGUMENT;
  LOG_WARN("AI column operation failed", "reason", error.what());
  LOG_USER_ERROR(OB_INVALID_ARGUMENT, error.what());
  return OB_INVALID_ARGUMENT;
}
std::string str(const Row &row, std::size_t i) {
  Definition::check(i < row.size() && row[i].has_value(),
                    "missing AI task field");
  return *row[i];
}
int write(Connection &connection, const std::string &sql,
          int64_t *count = nullptr) {
  int64_t affected = 0;
  const int ret =
      connection.execute_write(ObString(sql.size(), sql.data()), affected);
  if (count) {
    *count = affected;
  }
  return ret;
}
int read(Connection &connection, const std::string &sql, Rows &rows) {
  int ret = OB_SUCCESS;
  ObISQLClient::ReadResult holder;
  if (OB_FAIL(
          connection.execute_read(ObString(sql.size(), sql.data()), holder))) {
  } else if (!holder.get_result()) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    auto &result = *holder.get_result();
    while (OB_SUCC(ret) && OB_SUCC(ret = result.next())) {
      Row row;
      for (int64_t i = 0; OB_SUCC(ret) && i < result.get_column_count(); ++i) {
        ObString value;
        ret = result.get_varchar(i, value);
        if (ret == OB_ERR_NULL_VALUE) {
          row.emplace_back(std::nullopt);
          ret = OB_SUCCESS;
        } else if (OB_SUCC(ret)) {
          row.emplace_back(std::string(value.ptr(), value.length()));
        }
      }
      if (OB_SUCC(ret)) {
        rows.push_back(std::move(row));
      }
      if (rows.size() > 10000) {
        ret = OB_SIZE_OVERFLOW;
      }
    }
    if (ret == OB_ITER_END) {
      ret = OB_SUCCESS;
    }
  }
  return ret;
}
int owned(ConnectionGuard &guard) {
  return GCTX.sql_proxy_ ? GCTX.sql_proxy_->acquire_connection(guard, 0)
                         : OB_NOT_INIT;
}
struct Nested {
  ConnectionGuard guard;
  ObIInnerSQLConnection *connection = nullptr;
  ObSQLSessionInfo::StmtSavedValue session_saved;
  ObIInnerSQLConnection::SavedValue connection_saved;
  bool begun = false;
  int open(ObSQLSessionInfo &session) {
    int ret = OB_SUCCESS;
    if (OB_FAIL(query::ObInnerSQLConnectionAccess::
                    create_connection_with_external_session(&session, guard))) {
    } else if (!(connection = as_inner_sql_connection(guard.get_ptr()))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(connection->begin_nested_session(
                   session_saved, connection_saved, true))) {
    } else {
      begun = true;
    }
    return ret;
  }
  ~Nested() {
    if (begun) {
      connection->end_nested_session(session_saved, connection_saved);
    }
  }
};
int ensure_metadata(Connection &connection) {
  if (initialized.load()) {
    return OB_SUCCESS;
  }
  std::lock_guard<std::mutex> lock(metadata_mutex);
  if (initialized.load()) {
    return OB_SUCCESS;
  }
  int ret = OB_SUCCESS;
  if (OB_FAIL(write(
          connection,
          "CREATE TABLE IF NOT EXISTS " + META +
              " (table_id BIGINT UNSIGNED NOT NULL,column_id BIGINT UNSIGNED "
              "NOT NULL,column_name VARCHAR(128) NOT NULL,normalized_expr "
              "LONGTEXT NOT NULL,definition LONGTEXT NOT NULL,max_retries "
              "BIGINT NOT NULL,definition_version BIGINT NOT "
              "NULL,state_column_id BIGINT UNSIGNED NOT NULL,request_column_id "
              "BIGINT UNSIGNED NOT NULL,result_spec_column_id BIGINT UNSIGNED "
              "NOT NULL,PRIMARY KEY(table_id,column_id))"))) {
  } else if (OB_FAIL(write(
                 connection,
                 "CREATE TABLE IF NOT EXISTS " + TASKS +
                     " (table_id BIGINT UNSIGNED NOT NULL,column_id BIGINT "
                     "UNSIGNED NOT NULL,request_id BIGINT UNSIGNED NOT "
                     "NULL,rowkey_predicate VARCHAR(16384) NOT NULL,state INT "
                     "NOT NULL DEFAULT 1,retry_count BIGINT NOT NULL DEFAULT "
                     "0,eligible_ts DATETIME(6) NOT NULL,input_text LONGTEXT "
                     "NULL,config_text LONGTEXT NULL,result_text LONGTEXT "
                     "NULL,result_null TINYINT NOT NULL DEFAULT "
                     "0,definition_version BIGINT NOT NULL DEFAULT 1,PRIMARY "
                     "KEY(table_id,column_id,request_id))"))) {
  } else {
    initialized.store(true);
  }
  return ret;
}
struct Column {
  uint64_t id = 0;
  std::string name, state, request, spec, hash;
  Definition definition;
};
std::vector<Column> columns(const ObTableSchema &schema) {
  std::vector<Column> result;
  for (const auto &entry : definitions(schema)) {
    Column c;
    c.id = entry.first;
    c.definition = entry.second;
    c.name = ident(text(schema.get_column_schema(c.id)->get_column_name_str()));
    c.state = ident(hidden_name(c.id, "state"));
    c.request = ident(hidden_name(c.id, "request"));
    c.spec = ident(hidden_name(c.id, "spec"));
    c.hash = "MD5(CAST(JSON_ARRAY(" + literal(c.definition.encode());
    std::set<std::string> names;
    for (const auto &name : c.definition.dependencies) {
      names.insert(name);
    }
    for (int64_t i = 0; i < schema.get_rowkey_info().get_size(); ++i) {
      const auto *key = schema.get_rowkey_info().get_column(i);
      if (key) {
        names.insert(text(
            schema.get_column_schema(key->column_id_)->get_column_name_str()));
      }
    }
    for (const auto &name : names) {
      c.hash += ',';
      c.hash += ident(name);
    }
    c.hash += ") AS CHAR))";
    result.push_back(std::move(c));
  }
  return result;
}
std::string rowkey(const ObTableSchema &schema) {
  std::string expression = "CONCAT(";
  bool first = true;
  for (int64_t i = 0; i < schema.get_rowkey_info().get_size(); ++i) {
    const auto *key = schema.get_rowkey_info().get_column(i);
    Definition::check(key != nullptr, "AI rowkey is missing");
    const auto *column = schema.get_column_schema(key->column_id_);
    Definition::check(column != nullptr, "AI rowkey column is missing");
    const auto name = ident(text(column->get_column_name_str()));
    if (!first) {
      expression += "," + literal(" AND ") + ",";
    }
    first = false;
    expression += literal(name + " = ") + ",";
    if (ob_is_numeric_type(column->get_data_type())) {
      expression += "CAST(" + name + " AS CHAR)";
    } else {
      expression += "CONCAT(" + literal("CONVERT(X'") + ",HEX(" + name + ")," +
                    literal("' USING utf8mb4)") + ")";
    }
  }
  Definition::check(!first, "AI columns require a rowkey");
  return expression + ")";
}
int get_schema(uint64_t id, ObSchemaGetterGuard &guard,
               const ObTableSchema *&schema) {
  int ret = OB_SUCCESS;
  if (!GCTX.schema_service_) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
  } else {
    ret = guard.get_table_schema(id, schema);
  }
  return ret;
}
int table_name(const ObTableSchema &schema, ObSchemaGetterGuard &guard,
               std::string &name) {
  int ret = OB_SUCCESS;
  const ObDatabaseSchema *database = nullptr;
  if (OB_FAIL(guard.get_database_schema(schema.get_database_id(), database))) {
  } else if (!database) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    name = ident(text(database->get_database_name_str())) + "." +
           ident(text(schema.get_table_name_str()));
  }
  return ret;
}
int maintain(Connection &connection, const ObTableSchema &schema,
             const std::string &table) {
  int ret = OB_SUCCESS;
  const auto cols = columns(schema);
  if (cols.empty()) {
    return ret;
  }
  const auto key = rowkey(schema);
  std::vector<std::string> dirty;
  std::string condition;
  for (const auto &c : cols) {
    dirty.push_back("NOT(" + c.spec + " <=> " + c.hash + ")");
    if (!condition.empty()) {
      condition += " OR ";
    }
    condition += dirty.back();
  }
  std::string update = "UPDATE " + table + " SET ";
  for (std::size_t i = 0; i < cols.size(); ++i) {
    const auto &c = cols[i];
    if (i) {
      update += ",";
    }
    std::string group = "UUID_SHORT()";
    for (std::size_t j = i; j > 0; --j) {
      group =
          "IF(" + dirty[j - 1] + "," + cols[j - 1].request + "," + group + ")";
    }
    update += c.request + "=IF(" + dirty[i] + "," + group + "," + c.request +
              ")," + c.name + "=IF(" + dirty[i] + ",NULL," + c.name + ")," +
              c.state + "=IF(" + dirty[i] + ",'PENDING'," + c.state + ")";
  }
  for (std::size_t i = 0; i < cols.size(); ++i) {
    update += "," + cols[i].spec + "=IF(" + dirty[i] + "," + cols[i].hash +
              "," + cols[i].spec + ")";
  }
  update += " WHERE " + condition;
  if (OB_FAIL(write(connection, update))) {
    return ret;
  }
  for (const auto &c : cols) {
    const auto &d = c.definition;
    std::string input = "CAST((" + d.arguments[1] + ") AS CHAR)";
    const auto config = d.function == "ai_complete" && d.arguments.size() == 3
                            ? "CAST((" + d.arguments[2] + ") AS CHAR)"
                            : "NULL";
    const auto sql =
        "INSERT INTO " + TASKS +
        " (table_id,column_id,request_id,rowkey_predicate,state,retry_count,"
        "eligible_ts,input_text,config_text,definition_version) SELECT " +
        decimal(schema.get_table_id()) + "," + decimal(c.id) + "," + c.request +
        "," + key + ",1,0,NOW(6)," + input + "," + config + "," +
        decimal(d.definition_version) + " FROM " + table + " x WHERE " +
        c.state + "='PENDING' AND " + c.request +
        "<>0 AND NOT EXISTS(SELECT 1 FROM " + TASKS +
        " q WHERE q.table_id=" + decimal(schema.get_table_id()) +
        " AND q.column_id=" + decimal(c.id) + " AND q.request_id=x." +
        c.request + ")";
    if (OB_FAIL(write(connection, sql))) {
      return ret;
    }
    const auto cleanup =
        "DELETE q FROM " + TASKS +
        " q WHERE q.table_id=" + decimal(schema.get_table_id()) +
        " AND q.column_id=" + decimal(c.id) + " AND NOT EXISTS(SELECT 1 FROM " +
        table + " x WHERE q.request_id=x." + c.request +
        " AND q.rowkey_predicate=" + key + ")";
    if (OB_FAIL(write(connection, cleanup))) {
      return ret;
    }
  }
  return ret;
}
class Worker : public lib::ThreadPool {
public:
  void run1() override;
  int work();
  bool recovered = false;
};
Worker worker;
std::mutex worker_mutex;
bool worker_started = false;
} // namespace

bool internal_execution() { return internal; }
bool exchange_internal_execution(bool value) {
  const bool old = internal;
  internal = value;
  return old;
}
int register_table(ObExecContext &ctx, const ObTableSchema &schema,
                   const ObString &database, uint64_t id) {
  int ret = OB_SUCCESS;
  InternalGuard execution;
  try {
    const auto cols = columns(schema);
    if (cols.empty()) {
      return ret;
    }
    ConnectionGuard guard;
    if (OB_FAIL(owned(guard))) {
      return ret;
    }
    auto &connection = *guard.get_ptr();
    if (OB_FAIL(ensure_metadata(connection))) {
      return ret;
    }
    for (const auto &c : cols) {
      const auto *state = schema.get_column_schema(
          ObString::make_string(hidden_name(c.id, "state").c_str()));
      const auto *request = schema.get_column_schema(
          ObString::make_string(hidden_name(c.id, "request").c_str()));
      const auto *spec = schema.get_column_schema(
          ObString::make_string(hidden_name(c.id, "spec").c_str()));
      Definition::check(state && request && spec,
                        "AI hidden columns are missing");
      const auto sql =
          "REPLACE INTO " + META + " VALUES(" + decimal(id) + "," +
          decimal(c.id) + "," +
          literal(text(schema.get_column_schema(c.id)->get_column_name_str())) +
          "," + literal(c.definition.expression) + "," +
          literal(c.definition.encode()) + "," +
          decimal(c.definition.max_retries) + "," +
          decimal(c.definition.definition_version) + "," +
          decimal(state->get_column_id()) + "," +
          decimal(request->get_column_id()) + "," +
          decimal(spec->get_column_id()) + ")";
      if (OB_FAIL(write(connection, sql))) {
        break;
      }
    }
    if (OB_SUCC(ret)) {
      const auto name = ident(text(database)) + "." +
                        ident(text(schema.get_table_name_str()));
      if (OB_FAIL(connection.start_transaction())) {
      } else {
        ret = maintain(connection, schema, name);
        if (OB_SUCC(ret)) {
          ret = connection.commit();
        } else {
          connection.rollback();
        }
      }
    }
    if (OB_SUCC(ret)) {
      ret = start_worker();
    }
  } catch (const std::exception &error) {
    ret = fail(error);
  }
  return ret;
}
int after_dml(ObExecContext &ctx, const ObIArray<uint64_t> &tables) {
  int ret = OB_SUCCESS;
  if (internal || tables.empty() || !ctx.get_my_session()->is_user_session()) {
    return ret;
  }
  InternalGuard execution;
  try {
    Nested nested;
    if (OB_FAIL(nested.open(*ctx.get_my_session()))) {
      return ret;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < tables.count(); ++i) {
      ObSchemaGetterGuard guard;
      const ObTableSchema *schema = nullptr;
      std::string name;
      if (OB_FAIL(get_schema(tables.at(i), guard, schema))) {
      } else if (!schema) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(table_name(*schema, guard, name))) {
      } else {
        ret = maintain(*nested.connection, *schema, name);
      }
    }
  } catch (const std::exception &error) {
    ret = fail(error);
  }
  return ret;
}
int before_execute(ObExecContext &ctx) {
  int ret = OB_SUCCESS;
  if (internal || !ctx.get_stmt_factory() ||
      !ctx.get_stmt_factory()->get_query_ctx()) {
    return ret;
  }
  const auto &ids = ctx.get_stmt_factory()->get_query_ctx()->ai_column_reads_;
  for (int64_t i = 0; OB_SUCC(ret) && i + 1 < ids.count(); i += 2) {
    ObSchemaGetterGuard guard;
    const ObTableSchema *schema = nullptr;
    const ObDatabaseSchema *database = nullptr;
    if (OB_FAIL(get_schema(ids.at(i), guard, schema))) {
    } else if (!schema) {
      ret = OB_TABLE_NOT_EXIST;
    } else if (OB_FAIL(guard.get_database_schema(schema->get_database_id(),
                                                 database))) {
    } else if (!database) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ret = strict_read(*ctx.get_my_session(), *schema,
                        database->get_database_name_str(), ids.at(i + 1));
    }
  }
  return ret;
}
int strict_read(ObSQLSessionInfo &session, const ObTableSchema &schema,
                const ObString &database, uint64_t column) {
  int ret = OB_SUCCESS;
  if (internal) {
    return ret;
  }
  InternalGuard execution;
  try {
    ObObj strict;
    if (OB_FAIL(session.get_sys_variable(share::SYS_VAR_AI_COLUMN_STRICT_READ,
                                         strict))) {
      return ret;
    }
    if (!strict.get_int()) {
      return ret;
    }
    Nested nested;
    if (OB_FAIL(nested.open(session))) {
      LOG_INFO("AI strict nested open failed", K(ret));
      return ret;
    }
    Rows rows;
    const auto sql = "SELECT CAST(COUNT(*) AS CHAR) FROM " +
                     ident(text(database)) + "." +
                     ident(text(schema.get_table_name_str())) + " WHERE " +
                     ident(hidden_name(column, "state")) + "<>'READY'";
    if (OB_FAIL(read(*nested.connection, sql, rows))) {
      LOG_INFO("AI strict query failed", K(ret), "sql", sql.c_str());
    } else if (!rows.empty() && str(rows[0], 0) != "0") {
      ret = OB_AI_COLUMN_NOT_READY;
      LOG_USER_ERROR(OB_AI_COLUMN_NOT_READY, schema.get_table_id(), column);
    }
  } catch (const std::exception &error) {
    ret = fail(error);
  }
  return ret;
}

namespace {
void Worker::run1() {
  InternalGuard execution;
  while (!has_set_stop()) {
    int ret = OB_SUCCESS;
    try {
      ret = work();
    } catch (const std::exception &error) {
      ret = fail(error);
    }
    if (ret != OB_SUCCESS && ret != OB_TABLE_NOT_EXIST) {
      LOG_WARN("AI column worker iteration failed", K(ret));
    }
    for (int i = 0; i < 10 && !has_set_stop(); ++i) {
      usleep(10000);
    }
  }
}
int Worker::work() {
  int ret = OB_SUCCESS;
  ConnectionGuard connection_guard;
  if (OB_FAIL(owned(connection_guard))) {
    return ret;
  }
  auto &connection = *connection_guard.get_ptr();
  if (!recovered) {
    if (OB_FAIL(write(connection,
                      "UPDATE " + TASKS +
                          " SET state=1,eligible_ts=NOW(6) WHERE state=2"))) {
      return ret;
    }
    recovered = true;
  }
  Rows tasks;
  // A RUNNING lease is reclaimed after restart or a worker interruption.
  const auto query =
      "SELECT CAST(table_id AS CHAR),CAST(column_id AS CHAR),CAST(request_id "
      "AS CHAR),rowkey_predicate,CAST(retry_count AS "
      "CHAR),input_text,config_text FROM " +
      TASKS +
      " WHERE (state IN(1,3) AND eligible_ts<=NOW(6)) OR (state=2 AND "
      "eligible_ts<DATE_SUB(NOW(6),INTERVAL 60 SECOND)) ORDER BY eligible_ts "
      "LIMIT 1";
  if (OB_FAIL(read(connection, query, tasks)) || tasks.empty()) {
    return ret;
  }
  const auto task = tasks[0];
  const auto table_id = std::stoull(str(task, 0)),
             column_id = std::stoull(str(task, 1)),
             request = std::stoull(str(task, 2));
  const auto where = "table_id=" + decimal(table_id) +
                     " AND column_id=" + decimal(column_id) +
                     " AND request_id=" + decimal(request);
  ObSchemaGetterGuard schema_guard;
  const ObTableSchema *schema = nullptr;
  std::string name;
  if (OB_FAIL(get_schema(table_id, schema_guard, schema))) {
    return ret;
  }
  if (!schema || schema->is_in_recyclebin()) {
    return write(connection, "DELETE FROM " + TASKS +
                                 " WHERE table_id=" + decimal(table_id));
  }
  if (OB_FAIL(table_name(*schema, schema_guard, name))) {
    return ret;
  }
  const auto cols = columns(*schema);
  const Column *column = nullptr;
  for (const auto &candidate : cols) {
    if (candidate.id == column_id) {
      column = &candidate;
      break;
    }
  }
  if (!column) {
    return write(connection, "DELETE FROM " + TASKS + " WHERE " + where);
  }
  const auto &d = column->definition;
  // Claim before calling the network; source DML remains free to supersede it.
  int64_t claimed = 0;
  if (OB_FAIL(write(connection,
                    "UPDATE " + TASKS +
                        " SET state=2,eligible_ts=NOW(6) WHERE " + where +
                        " AND state IN(1,2,3)",
                    &claimed)) ||
      claimed == 0) {
    return ret;
  }
  std::string expression;
  if (d.function == "ai_embed") {
    expression = "AI_EMBED(" + d.arguments[0] + "," +
                 (task[5] ? literal(*task[5]) : "NULL") + "," +
                 decimal(d.dimension) + ")";
  } else {
    auto input = task[5] ? literal(*task[5]) : "NULL";
    if (Definition::lower(Definition::trim(d.arguments[1]))
            .compare(0, 10, "ai_prompt(") == 0) {
      input = "CAST(" + input + " AS JSON)";
    }
    std::string config =
        task[6] ? "CAST(" + literal(*task[6]) + " AS JSON)" : "JSON_OBJECT()";
    if (!d.system_prompt.empty()) {
      config = "JSON_SET(COALESCE(" + config +
               ",JSON_OBJECT()),'$.__seekdb_ai_column_system'," +
               literal(d.system_prompt) + ")";
    }
    expression =
        "AI_COMPLETE(" + d.arguments[0] + "," + input + "," + config + ")";
  }
  Rows output;
  int call_ret = read(connection, "SELECT " + expression, output);
  if (call_ret == OB_SUCCESS && output.size() != 1) {
    call_ret = OB_INVALID_DATA;
  }
  if (call_ret == OB_SUCCESS && output[0][0] && d.result_type == "SIGNED") {
    try {
      Definition::number(*output[0][0]);
    } catch (...) {
      call_ret = OB_INVALID_DATA;
    }
  }
  if (call_ret == OB_SUCCESS && output[0][0] && d.result_type == "JSON") {
    Rows checked;
    call_ret = read(connection,
                    "SELECT CAST(CAST(" + literal(*output[0][0]) +
                        " AS JSON) AS CHAR)",
                    checked);
  }
  if (call_ret != OB_SUCCESS) {
    const auto retries = Definition::number(str(task, 4));
    if (retries < d.max_retries) {
      if (OB_FAIL(write(connection,
                        "UPDATE " + TASKS +
                            " SET "
                            "state=3,retry_count=retry_count+1,eligible_ts="
                            "DATE_ADD(NOW(6),INTERVAL 1 SECOND) WHERE " +
                            where))) {
        return ret;
      }
      return OB_SUCCESS;
    }
    if (OB_FAIL(write(connection,
                      "UPDATE " + TASKS + " SET state=5 WHERE " + where))) {
      return ret;
    }
  } else {
    if (OB_FAIL(write(connection,
                      "UPDATE " + TASKS + " SET state=4,result_text=" +
                          (output[0][0] ? literal(*output[0][0]) : "NULL") +
                          ",result_null=" + (output[0][0] ? "0" : "1") +
                          " WHERE " + where))) {
      return ret;
    }
  }
  // Only terminal groups publish. Results, statuses, index entries and task
  // deletion are committed by the same native transaction.
  Rows group;
  const auto group_where =
      "table_id=" + decimal(table_id) + " AND request_id=" + decimal(request);
  if (OB_FAIL(read(connection,
                   "SELECT CAST(column_id AS CHAR),CAST(state AS "
                   "CHAR),result_text FROM " +
                       TASKS + " WHERE " + group_where,
                   group))) {
    return ret;
  }
  for (const auto &entry : group) {
    if (str(entry, 1) != "4" && str(entry, 1) != "5") {
      return OB_SUCCESS;
    }
  }
  if (group.empty()) {
    return OB_SUCCESS;
  }
  if (OB_FAIL(connection.start_transaction())) {
    return ret;
  }
  std::string update = "UPDATE " + name + " SET ", guard;
  bool first = true;
  for (const auto &entry : group) {
    const Column *target = nullptr;
    const auto id = std::stoull(str(entry, 0));
    for (const auto &candidate : cols) {
      if (candidate.id == id) {
        target = &candidate;
        break;
      }
    }
    if (!target) {
      ret = OB_SCHEMA_EAGAIN;
      break;
    }
    if (!first) {
      update += ',';
    }
    first = false;
    auto value = entry[2] ? literal(*entry[2]) : "NULL";
    if (entry[2] && target->definition.result_type == "JSON") {
      value = "CAST(" + value + " AS JSON)";
    }
    update += target->name + "=" + (str(entry, 1) == "4" ? value : "NULL") +
              "," + target->state + "=" +
              (str(entry, 1) == "4" ? "'READY'" : "'FAILED'");
    guard += " AND " + target->request + "=" + decimal(request);
  }
  if (OB_SUCC(ret)) {
    ret = write(connection, update + " WHERE " + str(task, 3) + guard);
  }
  if (OB_SUCC(ret)) {
    ret = write(connection, "DELETE FROM " + TASKS + " WHERE " + group_where);
  }
  if (OB_SUCC(ret)) {
    ret = connection.commit();
  } else {
    connection.rollback();
  }
  return ret;
}
} // namespace
int start_worker() {
  std::lock_guard<std::mutex> lock(worker_mutex);
  if (worker_started) {
    return OB_SUCCESS;
  }
  worker.set_thread_count(1);
  const int ret = worker.start();
  if (ret == OB_SUCCESS) {
    worker_started = true;
  }
  return ret;
}
void stop_worker() {
  std::lock_guard<std::mutex> lock(worker_mutex);
  if (worker_started) {
    worker.stop();
    worker.wait();
    worker_started = false;
  }
}
} // namespace ai_column
} // namespace sql
} // namespace oceanbase
