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
#pragma once
#include "share/ob_server_struct.h"
#include "share/schema/ob_schema_utils.h"
#include "sql/engine/basic/file_sql_cursor.h"
#include "sql/parser/parse_node.h"
#include "sql/session/ob_sql_session_info.h"
namespace oceanbase {
namespace sql {
namespace file_sql {
struct SqlError : std::runtime_error {
  int code;
  SqlError(int value, const char *message)
      : std::runtime_error(message), code(value) {}
};
inline std::string string(const common::ObString &s) {
  return std::string(s.ptr(), s.length());
}
inline std::string node_text(const ParseNode *node) {
  require(node != nullptr, "missing option value");
  if (node->type_ == T_COLUMN_REF && node->num_child_ == 3) {
    node = node->children_[2];
  }
  require(node != nullptr, "invalid option name");
  return node->str_value_ == nullptr
             ? std::string()
             : std::string(node->str_value_, node->str_len_);
}
inline std::string literal(const ParseNode *node) {
  if (!node) {
    return "true";
  }
  if (node->type_ == T_INT || node->type_ == T_BOOL) {
    return std::to_string(node->value_);
  }
  if (node->type_ == T_OP_NEG && node->num_child_ == 1) {
    return "-" + literal(node->children_[0]);
  }
  require(node->type_ == T_VARCHAR || node->type_ == T_CHAR ||
              node->type_ == T_IDENT,
          "file options must be constant literals");
  return node_text(node);
}
inline void apply(Options &options, const std::string &key,
                  const std::string &value, bool copy) {
  if (key == "header") {
    require(value == "1" || value == "0" || lower(value) == "true" ||
                lower(value) == "false",
            "header must be boolean");
    options.header = value == "1" || lower(value) == "true";
  } else if (key == "sample_size" && !copy) {
    if (value != "-1") {
      throw SqlError(common::OB_NOT_SUPPORTED,
                     "only sample_size=-1 is supported");
    }
  } else if (key == "format") {
    require((copy && lower(value) == "csv") ||
                (!copy && options.format == "json" &&
                 lower(value) == "newline_delimited"),
            "unsupported format option");
  } else if (key == "delimiter" || key == "delim" || key == "quote" ||
             key == "escape") {
    require(value.size() == 1 && value[0] != '\0' && value[0] != '\n' &&
                value[0] != '\r',
            "CSV character option requires one byte");
    if (key == "quote") {
      options.quote = value[0];
    } else if (key == "escape") {
      options.escape = value[0];
    } else {
      options.delimiter = value[0];
    }
  } else if (key == "nullstr") {
    require(value.size() <= MAX_RECORD, "NULL marker too large");
    options.nullstr = value;
  } else {
    throw SqlError(common::OB_NOT_SUPPORTED, "unsupported file option");
  }
}
inline Options options(const ParseNode &node, bool copy) {
  Options result;
  const ParseNode *args = node.children_[1];
  if (copy) {
    result.format = "csv";
    result.header = false;
    result.path = literal(node.children_[0]);
  } else {
    auto name = lower(node_text(node.children_[0]));
    if (name == "read_csv") {
      result.format = "csv";
    } else if (name == "read_json" || name == "read_ndjson") {
      result.format = "json";
    } else if (name == "read_parquet") {
      result.format = "parquet";
    } else if (name == "glob") {
      result.format = "glob";
    } else {
      throw SqlError(common::OB_ERR_PARSE_SQL, "unknown file table function");
    }
    require(args && args->num_child_ >= 1, "file path required");
    result.path = literal(args->children_[0]);
    result.describe = node.value_ == 1;
  }
  std::set<std::string> seen;
  for (int i = copy ? 0 : 1; args && i < args->num_child_; ++i) {
    auto *option = args->children_[i];
    if (option->type_ != T_OP_EQ || option->num_child_ != 2) {
      throw SqlError(common::OB_ERR_PARSE_SQL, "named file option required");
    }
    const auto key = lower(node_text(option->children_[0]));
    require(seen.insert(key).second, "duplicate file option");
    apply(result, key, literal(option->children_[1]), copy);
  }
  require(result.delimiter != result.quote, "delimiter and quote must differ");
  return result;
}
inline int policy(ObSQLSessionInfo &session, common::ObIAllocator &allocator,
                  std::string &root) {
  int ret = common::OB_SUCCESS;
  common::ObObj enabled;
  common::ObString secure;
  if (OB_FAIL(
          session.get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
  } else if (enabled.get_int() == 0) {
    ret = common::OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED,
                   "file SQL is disabled; set enable_file_sql=ON");
  } else if (OB_ISNULL(GCTX.schema_service_)) {
    ret = common::OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(share::schema::ObSchemaUtils::get_runtime_varchar_variable(
                 *GCTX.schema_service_, share::SYS_VAR_SECURE_FILE_PRIV,
                 allocator, secure))) {
  } else {
    root = string(secure);
  }
  return ret;
}
inline int failure(const std::exception &error) {
  const auto *sql_error = dynamic_cast<const SqlError *>(&error);
  const int ret = sql_error ? sql_error->code
                  : dynamic_cast<const std::bad_alloc *>(&error)
                      ? common::OB_ALLOCATE_MEMORY_FAILED
                      : common::OB_INVALID_ARGUMENT;
  SQL_ENG_LOG(WARN, "file SQL failed", K(ret), "reason", error.what());
  if (ret == common::OB_NOT_SUPPORTED) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED, error.what());
  } else if (ret == common::OB_INVALID_ARGUMENT) {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, error.what());
  }
  return ret;
}
} // namespace file_sql
} // namespace sql
} // namespace oceanbase
