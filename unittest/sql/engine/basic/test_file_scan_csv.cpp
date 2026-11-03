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
// Standalone regression executable: c++ -std=c++17 -Isrc this_file.cpp -o test_file_scan_csv
#include "sql/engine/basic/file_scan_csv.h"
#include <cassert>
#include <sstream>
using namespace oceanbase::sql::file_sql;
int main() {
  std::istringstream input("id,note,empty\r\n1,\"hello,\r\nworld\",\r\n2,\"say \"\"hi\"\"\",x");
  CsvReader reader(input);
  std::vector<CsvField> row;
  assert(reader.next(row) && row.size() == 3 && row[0].text == "id");
  assert(reader.next(row) && row.size() == 3);
  assert(row[1].quoted && row[1].text == "hello,\r\nworld" && row[2].text.empty());
  assert(reader.next(row) && row[1].text == "say \"hi\"");
  assert(!reader.next(row));
  std::istringstream broken("a,\"unterminated");
  CsvReader bad(broken);
  bool rejected = false;
  try { bad.next(row); } catch (const std::runtime_error &) { rejected = true; }
  assert(rejected);
  std::istringstream huge("123456");
  CsvOptions options;
  options.max_record_bytes = 3;
  CsvReader bounded(huge, options);
  rejected = false;
  try { bounded.next(row); } catch (const std::runtime_error &) { rejected = true; }
  assert(rejected);
  std::istringstream empty("");
  CsvReader empty_reader(empty);
  assert(!empty_reader.next(row));
  std::istringstream trailing("a,b,\n");
  CsvReader trailing_reader(trailing);
  assert(trailing_reader.next(row) && row.size() == 3 && row[2].text.empty());
  assert(!trailing_reader.next(row));
  std::istringstream invalid("\"a\"suffix,b");
  CsvReader invalid_reader(invalid);
  rejected = false;
  try { invalid_reader.next(row); } catch (const std::runtime_error &) { rejected = true; }
  assert(rejected);
  std::istringstream lazy("a,b\n\"unterminated");
  CsvReader lazy_reader(lazy);
  assert(lazy_reader.next(row) && row[0].text == "a");
  // First-row consumers must not parse the rest of the file.
  assert(lazy.peek() == '"');
}
