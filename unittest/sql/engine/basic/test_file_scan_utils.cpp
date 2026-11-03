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
// Standalone: c++ -std=c++20 -Isrc test_file_scan_utils.cpp -o
// test_file_scan_utils
#include "sql/engine/basic/file_sql_cursor.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace oceanbase::sql::file_sql;
static void write(const std::string &path, const std::string &data) {
  std::ofstream(path, std::ios::binary) << data;
}
template <class F> static void rejects(F function) {
  bool caught = false;
  try {
    function();
  } catch (const std::exception &) {
    caught = true;
  }
  assert(caught);
}
int main(int argc, char **argv) {
#ifndef _WIN32
  char pattern[] = "/tmp/seekdb-file-sql-XXXXXX";
  const char *created = ::mkdtemp(pattern);
  assert(created);
  const std::string root = created;
  write(root + "/input.csv", "id,note,id,empty,day\r\n1,\"hello,\r\nworld\",11,"
                             ",2024-02-29\r\n2,\"\",12,,2024-03-01\r\n");
  Options options;
  options.format = "csv";
  options.path = root + "/input.csv";
  auto descriptor = oceanbase::sql::file_sql::bind(options, root);
  assert(descriptor.schema.size() == 5);
  assert(descriptor.schema[2].name == "id__2");
  assert(descriptor.schema[4].type == Type::DATE);
  assert(descriptor.schema[3].nullable);
  Cursor csv(Descriptor::decode(descriptor.encode()), root, {0, 1});
  Row row;
  assert(csv.next(row) && row[1].text == "hello,\nworld" && row[3].null());
  assert(csv.next(row) && !row[1].null() && row[1].text.empty());
  assert(!csv.next(row));
  write(root + "/union.jsonl", "{\"id\":1,\"name\":\"中\\uD83D\\uDE00\"}\n{"
                               "\"id\":2.5,\"extra\":true}\n");
  options.format = "json";
  options.path = root + "/union.jsonl";
  descriptor = oceanbase::sql::file_sql::bind(options, root);
  assert(descriptor.schema.size() == 3 &&
         descriptor.schema[0].type == Type::DOUBLE &&
         descriptor.schema[1].nullable && descriptor.schema[2].nullable);
  Cursor json(descriptor, root, {0, 1, 2});
  assert(json.next(row) && row[2].null());
  assert(json.next(row) && row[1].null() && row[2].text == "true");
  write(root + "/bad.jsonl", "{\"x\":{\"nested\":1}}\n");
  options.path = root + "/bad.jsonl";
  rejects([&] { oceanbase::sql::file_sql::bind(options, root); });
  options.path = root + "/union.jsonl";
  descriptor = oceanbase::sql::file_sql::bind(options, root);
  write(options.path, "{}\n");
  rejects([&] { Cursor stale(descriptor, root, {}); });
  rejects([&] { Input outside("/etc/passwd", root); });
  assert(::symlink("/etc/passwd", (root + "/escape.csv").c_str()) == 0);
  rejects([&] { Input symlink(root + "/escape.csv", root); });
  Options output;
  output.path = root + "/output.csv";
  output.nullstr = "NULL";
  {
    CsvOutput writer(output, root);
    writer.row({Cell("name"), Cell("value")});
    writer.row({Cell("a,b"), Cell()});
    writer.row({Cell(""), Cell("NULL")});
    writer.commit();
  }
  options = output;
  options.format = "csv";
  descriptor = oceanbase::sql::file_sql::bind(options, root);
  Cursor copied(descriptor, root, {0, 1});
  assert(copied.next(row) && row[0].text == "a,b" && row[1].null());
  assert(copied.next(row) && !row[0].null() && row[1].text == "NULL");
  rejects([&] { CsvOutput duplicate(output, root); });
  output.path = root + "/aborted.csv";
  {
    CsvOutput aborted(output, root);
    aborted.row({Cell("not published")});
  }
  assert(!std::filesystem::exists(output.path));
  for (const auto &e : std::filesystem::directory_iterator(root)) {
    assert(e.path().filename().string().find(".seekdb-copy-") != 0);
  }
  assert(discover(root + "/*.csv", root).size() == 2);
  assert(snappy(std::string("\x05\x10hello", 7), 5) == "hello");
  rejects([] { snappy(std::string("\x05\x01\x00", 3), 5); });
  std::size_t position = 0;
  auto zeros = hybrid(std::string(1, '\x03'), position, 0, 3, 1);
  assert(zeros == std::vector<uint32_t>({0, 0, 0}));
  if (argc > 1) {
    options.format = "parquet";
    options.path = argv[1];
    descriptor = oceanbase::sql::file_sql::bind(
        options, std::filesystem::path(argv[1]).parent_path().string());
    Cursor parquet(descriptor,
                   std::filesystem::path(argv[1]).parent_path().string(), {0});
    int64_t count = 0, sum = 0;
    while (parquet.next(row)) {
      ++count;
      sum += std::stoll(row[0].text);
    }
    assert(count == 7 && sum == 21);
  }
  std::filesystem::remove_all(root);
#endif
  std::cout << "file scan regressions passed\n";
}
