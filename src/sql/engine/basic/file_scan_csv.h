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

#include <cstddef>
#include <istream>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

namespace oceanbase {
namespace sql {
namespace file_sql {

struct CsvOptions {
  char delimiter = ',';
  char quote = '"';
  char escape = '"';
  std::size_t max_record_bytes = 16 * 1024 * 1024;
  std::size_t max_columns = 4096;
};

struct CsvField {
  std::string text;
  bool quoted = false;
};

// Reads one logical record at a time. Newlines inside quoted fields are data.
// The caller owns the stream and can stop scanning immediately for LIMIT.
class CsvReader {
public:
  explicit CsvReader(std::istream &input, CsvOptions options = {})
      : input_(input), options_(options) {
    if (options.delimiter == options.quote || options.delimiter == '\r'
        || options.delimiter == '\n' || options.quote == '\r'
        || options.quote == '\n' || options.max_record_bytes == 0
        || options.max_columns == 0) {
      throw std::invalid_argument("invalid CSV options");
    }
  }

  bool next(std::vector<CsvField> &record) {
    record.clear();
    CsvField field;
    bool in_quotes = false;
    bool after_quote = false;
    bool have_input = false;
    std::size_t bytes = 0;
    const std::size_t record_line = line_;
    const auto finish_field = [&]() {
      if (record.size() >= options_.max_columns) {
        fail(record_line, "column limit exceeded");
      }
      record.push_back(std::move(field));
      field = CsvField{};
    };
    const auto read = [&]() {
      const int ch = input_.get();
      if (ch != std::char_traits<char>::eof()
          && ++bytes > options_.max_record_bytes) {
        fail(record_line, "record byte limit exceeded");
      }
      return ch;
    };
    for (;;) {
      const int value = read();
      if (value == std::char_traits<char>::eof()) {
        if (input_.bad() || (!input_.eof() && input_.fail())) {
          fail(record_line, "read error");
        }
        if (in_quotes) {
          fail(record_line, "unterminated quoted field");
        }
        if (!have_input) {
          return false;
        }
        finish_field();
        return true;
      }
      have_input = true;
      const char ch = static_cast<char>(value);
      if (in_quotes) {
        if (ch == options_.escape && options_.escape != options_.quote) {
          const int escaped = read();
          if (escaped == std::char_traits<char>::eof()) {
            fail(record_line, "unterminated escape");
          }
          if (escaped == '\n') { ++line_; }
          field.text.push_back(static_cast<char>(escaped));
        } else if (ch == options_.quote) {
          if (input_.peek() == options_.quote) {
            read();
            field.text.push_back(ch);
          } else {
            in_quotes = false;
            after_quote = true;
          }
        } else {
          if (ch == '\n') { ++line_; }
          field.text.push_back(ch);
        }
      } else if (ch == options_.delimiter) {
        finish_field();
        after_quote = false;
      } else if (ch == '\n' || ch == '\r') {
        if (ch == '\r' && input_.peek() == '\n') { read(); }
        ++line_;
        finish_field();
        return true;
      } else if (after_quote) {
        fail(record_line, "unexpected data after closing quote");
      } else if (ch == options_.quote) {
        if (!field.text.empty()) {
          fail(record_line, "quote in unquoted field");
        }
        field.quoted = true;
        in_quotes = true;
      } else {
        field.text.push_back(ch);
      }
    }
  }

  std::size_t line() const { return line_; }

private:
  [[noreturn]] static void fail(std::size_t line, const char *message) {
    throw std::runtime_error("CSV record at line " + std::to_string(line) + ": " + message);
  }
  std::istream &input_;
  CsvOptions options_;
  std::size_t line_ = 1;
};

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
