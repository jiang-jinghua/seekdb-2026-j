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

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace oceanbase {
namespace share {
namespace ai_column {

// Persisted in the default value of the AI column's hidden result-spec column.
// This uses the existing schema deep-copy, RPC and inner-table persistence
// paths.
struct Definition {
  std::string expression;
  std::string function;
  std::vector<std::string> arguments;
  std::vector<std::string> dependencies;
  std::string system_prompt;
  std::string result_type;
  int64_t max_retries = 2;
  int64_t definition_version = 1;
  int64_t dimension = 0;

  static constexpr std::size_t MAX_DEFINITION_BYTES = 65535;
  static void check(bool valid, const char *message) {
    if (!valid) {
      throw std::invalid_argument(message);
    }
  }
  static std::string lower(std::string text) {
    for (auto &c : text) {
      if (c >= 'A' && c <= 'Z') {
        c += 'a' - 'A';
      }
    }
    return text;
  }
  static std::string trim(const std::string &s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    return begin == std::string::npos
               ? std::string()
               : s.substr(begin, s.find_last_not_of(" \t\r\n") - begin + 1);
  }
  static std::string quote_identifier(const std::string &value) {
    std::string result = "`";
    for (char c : value) {
      if (c == '`') {
        result += '`';
      }
      result += c;
    }
    return result + '`';
  }
  static std::string quote_literal(const std::string &value) {
    // Hex literals are independent of NO_BACKSLASH_ESCAPES and connection
    // quoting.
    static const char hex[] = "0123456789abcdef";
    std::string result = "CONVERT(X'";
    for (unsigned char c : value) {
      result += hex[c >> 4];
      result += hex[c & 15];
    }
    return result + "' USING utf8mb4)";
  }
  // The SQL parser validates the expression before this splitter is used.
  // Quoted commas and nested calls must remain part of their original argument.
  void parse_expression(const std::string &sql) {
    expression = trim(sql);
    check(!expression.empty() && expression.size() <= MAX_DEFINITION_BYTES,
          "invalid AI column expression size");
    const auto open = expression.find('(');
    check(open != std::string::npos && expression.back() == ')',
          "AI column requires one AI function call");
    function = lower(trim(expression.substr(0, open)));
    check(function == "ai_complete" || function == "ai_embed",
          "AI column supports AI_COMPLETE or AI_EMBED");
    arguments.clear();
    int depth = 0;
    char quote = 0;
    std::size_t begin = open + 1;
    for (std::size_t i = begin; i + 1 < expression.size(); ++i) {
      const char c = expression[i];
      if (quote) {
        if (c == quote) {
          if (i + 1 < expression.size() && expression[i + 1] == quote) {
            ++i;
          } else {
            quote = 0;
          }
        } else if (c == '\\' && quote != '`' && i + 1 < expression.size()) {
          ++i;
        }
      } else if (c == '\'' || c == '"' || c == '`') {
        quote = c;
      } else if (c == '(') {
        ++depth;
      } else if (c == ')') {
        check(depth > 0, "unbalanced AI expression");
        --depth;
      } else if (c == ',' && depth == 0) {
        arguments.push_back(trim(expression.substr(begin, i - begin)));
        begin = i + 1;
      }
    }
    check(quote == 0 && depth == 0, "unbalanced AI expression");
    arguments.push_back(
        trim(expression.substr(begin, expression.size() - 1 - begin)));
    check(arguments.size() >= 2 && arguments.size() <= 3,
          "AI function requires two or three arguments");
    for (const auto &argument : arguments) {
      check(!argument.empty(), "empty AI function argument");
    }
  }
  void validate() const {
    check(function == "ai_complete" || function == "ai_embed",
          "unknown AI function");
    check(arguments.size() >= 2 && arguments.size() <= 3,
          "invalid AI function arity");
    check(max_retries >= 0 && max_retries <= 100,
          "AI max_retries must be between 0 and 100");
    check(definition_version > 0, "invalid AI definition version");
    check(dependencies.size() <= 4096, "too many AI dependencies");
    check(function != "ai_embed" || (dimension > 0 && dimension <= 65535),
          "invalid AI embedding dimension");
  }
  static void put(std::string &out, const std::string &field) {
    check(field.size() <= MAX_DEFINITION_BYTES,
          "AI definition field too large");
    out += std::to_string(field.size()) + ':' + field;
    check(out.size() <= MAX_DEFINITION_BYTES, "AI definition too large");
  }
  static std::string take(const std::string &data, std::size_t &position) {
    const auto end = data.find(':', position);
    check(end != std::string::npos && end > position,
          "invalid AI definition length");
    uint64_t size = 0;
    const auto parsed =
        std::from_chars(data.data() + position, data.data() + end, size);
    check(parsed.ec == std::errc() && parsed.ptr == data.data() + end &&
              size <= data.size() - end - 1,
          "truncated AI definition");
    position = end + 1 + size;
    return data.substr(end + 1, size);
  }
  static int64_t number(const std::string &data) {
    int64_t value = 0;
    const auto parsed =
        std::from_chars(data.data(), data.data() + data.size(), value);
    check(!data.empty() && parsed.ec == std::errc() &&
              parsed.ptr == data.data() + data.size(),
          "invalid AI integer option");
    return value;
  }
  std::string encode() const {
    validate();
    std::string out = "AIC1";
    for (const auto &field :
         {expression, function, system_prompt, result_type,
          std::to_string(max_retries), std::to_string(definition_version),
          std::to_string(dimension), std::to_string(arguments.size())}) {
      put(out, field);
    }
    for (const auto &argument : arguments) {
      put(out, argument);
    }
    put(out, std::to_string(dependencies.size()));
    for (const auto &dependency : dependencies) {
      put(out, dependency);
    }
    return out;
  }
  static Definition decode(const std::string &data) {
    check(data.size() <= MAX_DEFINITION_BYTES &&
              data.compare(0, 4, "AIC1") == 0,
          "invalid AI definition version");
    std::size_t position = 4;
    Definition d;
    d.expression = take(data, position);
    d.function = take(data, position);
    d.system_prompt = take(data, position);
    d.result_type = take(data, position);
    d.max_retries = number(take(data, position));
    d.definition_version = number(take(data, position));
    d.dimension = number(take(data, position));
    const auto count = number(take(data, position));
    check(count >= 2 && count <= 3, "invalid AI argument count");
    for (int64_t i = 0; i < count; ++i) {
      d.arguments.push_back(take(data, position));
    }
    const auto dependencies = number(take(data, position));
    check(dependencies >= 0 && dependencies <= 4096,
          "invalid AI dependency count");
    for (int64_t i = 0; i < dependencies; ++i) {
      d.dependencies.push_back(take(data, position));
    }
    check(position == data.size(), "trailing AI definition data");
    d.validate();
    return d;
  }
};

} // namespace ai_column
} // namespace share
} // namespace oceanbase
