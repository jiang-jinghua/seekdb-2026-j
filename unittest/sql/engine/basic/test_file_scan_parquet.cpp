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
#include "sql/engine/basic/file_sql_cursor.h"
#include <cassert>
#include <filesystem>
#include <iostream>
using namespace oceanbase::sql::file_sql;
int main(int argc, char **argv) {
  for (int arg = 1; arg < argc; ++arg) {
    try {
      Options options;
      options.path = argv[arg];
      options.format = "parquet";
      const auto root = std::filesystem::path(argv[arg]).parent_path().string();
      auto d = oceanbase::sql::file_sql::bind(options, root);
      if (d.schema.size() == 2 && d.schema[0].name == "u32") {
        Cursor cursor(d, root, {0, 1});
        Row row;
        assert(cursor.next(row));
        assert(row[0].text == "4294967295");
        assert(row[1].text == "18446744073709551615");
        assert(d.schema[1].type == Type::VARCHAR);
        assert(!cursor.next(row));
        std::cout << argv[arg] << ": PASS\n";
        continue;
      }
      assert(d.schema.size() == 6);
      Cursor cursor(d, root, {0, 1, 2, 3, 4, 5});
      Row row;
      int i = 0;
      int64_t start, micro;
      calendar("2024-01-01", start, micro);
      while (cursor.next(row)) {
        assert(row.size() == 6);
        assert(row[0].null() == (i % 7 == 0));
        if (i % 7) {
          assert(std::stoll(row[0].text) == i);
        }
        assert(row[1].null() == (i % 5 == 0));
        if (i % 5) {
          assert(std::stod(row[1].text) == i / 4.0);
        }
        assert(row[2].null() == (i % 3 == 0));
        if (i % 3) {
          assert(row[2].text == (i % 2 == 0 ? "true" : "false"));
        }
        assert(row[3].null() == (i % 4 == 0));
        if (i % 4) {
          assert(std::stoll(row[3].text) == start + i);
        }
        assert(row[4].null() == (i % 6 == 0));
        if (i % 6) {
          assert(std::stoll(row[4].text) == micro + i);
        }
        assert(row[5].null() == (i % 8 == 0));
        if (i % 8) {
          assert(row[5].text == "文字" + std::to_string(i % 3));
        }
        ++i;
      }
      assert(i == 257);
      std::cout << argv[arg] << ": PASS\n";
    } catch (const std::exception &e) {
      std::cerr << argv[arg] << ": " << e.what() << "\n";
      return 1;
    }
  }
}
