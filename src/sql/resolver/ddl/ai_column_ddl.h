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
#include "share/schema/ai_column_definition.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_table_schema.h"
#include "sql/parser/parse_node.h"

namespace oceanbase {
namespace sql {
namespace ai_column {
using Definition = share::ai_column::Definition;
inline std::string text(const common::ObString &s) {
  return std::string(s.ptr(), s.length());
}
inline std::string node_text(const ParseNode *node) {
  Definition::check(node != nullptr, "missing AI option");
  if (node->type_ == T_COLUMN_REF && node->num_child_ == 3) {
    node = node->children_[2];
  }
  Definition::check(node && node->str_value_, "invalid AI option name");
  return std::string(node->str_value_, node->str_len_);
}
inline void dependencies(const ParseNode &node,
                         std::vector<std::string> &result) {
  Definition::check(
      node.type_ != T_SELECT && node.type_ != T_OP_GET_USER_VAR,
      "AI column expressions cannot contain subqueries or session variables");
  if (node.type_ == T_FUN_SYS && node.num_child_ >= 1 && node.children_[0]) {
    const auto name = Definition::lower(node_text(node.children_[0]));
    Definition::check(
        name != "ai_complete" && name != "ai_embed" && name != "ai_rerank" &&
            name != "load_file" && name != "sleep" && name != "get_lock",
        "AI inputs cannot invoke external or nested model operations");
  }
  if (node.type_ == T_COLUMN_REF) {
    Definition::check(node.num_child_ == 3 && node.children_[0] == nullptr &&
                          node.children_[1] == nullptr &&
                          node.children_[2] != nullptr,
                      "AI dependencies must be unqualified columns");
    const auto name = node_text(node.children_[2]);
    if (std::find(result.begin(), result.end(), name) == result.end()) {
      result.push_back(name);
    }
  } else {
    for (int64_t i = 0; i < node.num_child_; ++i) {
      if (node.children_[i]) {
        dependencies(*node.children_[i], result);
      }
    }
  }
}
inline std::string hidden_name(uint64_t id, const char *kind) {
  return "__ai_" + std::string(kind) + "_" + std::to_string(id);
}
inline bool is_ai(const share::schema::ObColumnSchemaV2 &column) {
  return (column.get_column_flags() & AI_COLUMN_FLAG) != 0;
}
inline Definition resolve(const ParseNode &node,
                          const share::schema::ObColumnSchemaV2 &column) {
  Definition d;
  Definition::check(node.num_child_ == 6 && node.children_[4],
                    "invalid AI column definition");
  d.parse_expression(node_text(node.children_[4]));
  const auto *call = node.children_[4];
  Definition::check(call->type_ == T_FUN_SYS && call->num_child_ == 2 &&
                        call->children_[1],
                    "AI column requires a scalar AI function call");
  const auto *arguments = call->children_[1];
  Definition::check(arguments->num_child_ >= 2 && arguments->num_child_ <= 3 &&
                        arguments->children_[0] &&
                        arguments->children_[0]->type_ == T_VARCHAR,
                    "AI column model name must be a constant string");
  d.arguments[0] =
      Definition::quote_literal(node_text(arguments->children_[0]));
  for (int64_t i = 1; i < arguments->num_child_; ++i) {
    dependencies(*arguments->children_[i], d.dependencies);
  }
  const auto *options = node.children_[5];
  std::set<std::string> seen;
  for (int64_t i = 0; options && i < options->num_child_; ++i) {
    const auto *option = options->children_[i];
    Definition::check(option && option->type_ == T_OP_EQ &&
                          option->num_child_ == 2,
                      "AI options require name=value");
    const auto name = Definition::lower(node_text(option->children_[0]));
    Definition::check(seen.insert(name).second, "duplicate AI column option");
    const auto *value = option->children_[1];
    if (name == "sync_mode") {
      Definition::check(value && value->type_ == T_VARCHAR &&
                            Definition::lower(node_text(value)) == "async",
                        "only asynchronous AI columns are supported");
    } else if (name == "max_retries") {
      Definition::check(value && value->type_ == T_INT,
                        "AI max_retries must be an integer");
      d.max_retries = value->value_;
    } else {
      Definition::check(false, "unknown AI column option");
    }
  }
  const auto type = column.get_data_type();
  if (column.is_json()) {
    d.result_type = "JSON";
    d.system_prompt = "Return exactly one valid JSON value. Do not include "
                      "Markdown or explanations.";
  } else if (common::ob_is_integer_type(type)) {
    d.result_type = "SIGNED";
    d.system_prompt = "Return exactly one base-10 integer within the SQL "
                      "result type range. Do not include other text.";
  } else if (common::ob_is_numeric_type(type)) {
    d.result_type = "DECIMAL";
    d.system_prompt =
        "Return exactly one finite base-10 number. Do not include other text.";
  } else if (common::ob_is_string_type(type)) {
    d.result_type = "CHAR";
  } else if (column.is_collection() &&
             column.get_extended_type_info().count() == 1) {
    const auto type_name =
        Definition::lower(text(column.get_extended_type_info().at(0)));
    Definition::check(type_name.compare(0, 7, "vector(") == 0 &&
                          type_name.back() == ')',
                      "AI_EMBED requires a VECTOR result");
    d.dimension = Definition::number(type_name.substr(7, type_name.size() - 8));
    d.result_type = "VECTOR";
  } else {
    Definition::check(false, "unsupported AI column result type");
  }
  Definition::check((d.function == "ai_embed") == (d.result_type == "VECTOR"),
                    "AI function and result type mismatch");
  d.validate();
  if (!d.system_prompt.empty()) {
    d.expression += " /* " + d.system_prompt + " */";
  }
  return d;
}
inline int add_hidden_columns(share::schema::ObTableSchema &schema,
                              uint64_t result_id,
                              const Definition &definition) {
  int ret = common::OB_SUCCESS;
  const char *kinds[] = {"state", "request", "spec"};
  for (int i = 0; OB_SUCC(ret) && i < 3; ++i) {
    share::schema::ObColumnSchemaV2 column;
    const uint64_t id = schema.get_max_used_column_id() + 1;
    column.set_column_id(id);
    column.set_table_id(schema.get_table_id());
    column.set_is_hidden(true);
    column.set_nullable(false);
    column.set_data_type(i == 1 ? common::ObUInt64Type : common::ObVarcharType);
    column.set_charset_type(common::CHARSET_UTF8MB4);
    column.set_collation_type(common::CS_TYPE_UTF8MB4_BIN);
    column.set_data_length(
        static_cast<int64_t>(i == 2 ? Definition::MAX_DEFINITION_BYTES : 32));
    const auto name = hidden_name(result_id, kinds[i]);
    common::ObObj value;
    const auto encoded = i == 2 ? definition.encode() : std::string("PENDING");
    if (i == 1) {
      value.set_uint64(0);
      column.set_charset_type(common::CHARSET_BINARY);
      column.set_collation_type(common::CS_TYPE_BINARY);
    } else {
      value.set_varchar(common::ObString(encoded.size(), encoded.data()));
      value.set_collation_type(common::CS_TYPE_UTF8MB4_BIN);
    }
    if (OB_FAIL(column.set_column_name(
            common::ObString(name.size(), name.data())))) {
    } else if (OB_FAIL(column.set_cur_default_value(value, false))) {
    } else if (OB_FAIL(column.set_orig_default_value(value))) {
    } else if (OB_FAIL(schema.add_column(column))) {
    } else {
      schema.set_max_used_column_id(id);
    }
  }
  return ret;
}
inline std::vector<std::pair<uint64_t, Definition>>
definitions(const share::schema::ObTableSchema &schema) {
  std::vector<std::pair<uint64_t, Definition>> result;
  for (int64_t i = 0; i < schema.get_column_count(); ++i) {
    const auto *column = schema.get_column_schema_by_idx(i);
    if (column && is_ai(*column)) {
      const auto name = hidden_name(column->get_column_id(), "spec");
      const auto *spec =
          schema.get_column_schema(common::ObString(name.size(), name.data()));
      Definition::check(spec != nullptr &&
                            spec->get_cur_default_value().is_string_type(),
                        "missing AI schema result specification");
      result.emplace_back(
          column->get_column_id(),
          Definition::decode(text(spec->get_cur_default_value().get_string())));
    }
  }
  return result;
}
} // namespace ai_column
} // namespace sql
} // namespace oceanbase
