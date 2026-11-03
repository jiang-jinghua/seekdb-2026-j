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

#include "sql/engine/basic/file_scan_csv.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace oceanbase {
namespace sql {
namespace file_sql {

constexpr std::size_t MAX_RECORD = 16 * 1024 * 1024;
constexpr std::size_t MAX_COLUMNS = 4096;
inline void require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error("file SQL: " + message);
  }
}
inline std::string lower(std::string value) {
  for (char &ch : value) {
    if (ch >= 'A' && ch <= 'Z') {
      ch += 'a' - 'A';
    }
  }
  return value;
}
inline void utf8(const std::string &s) {
  for (std::size_t i = 0; i < s.size();) {
    uint32_t c = static_cast<unsigned char>(s[i++]);
    if (c < 0x80) {
      continue;
    }
    int n = c >= 0xc2 && c <= 0xdf   ? 1
            : c >= 0xe0 && c <= 0xef ? 2
            : c >= 0xf0 && c <= 0xf4 ? 3
                                     : -1;
    require(n >= 0 && i + n <= s.size(), "invalid UTF-8");
    uint32_t point = c & ((1u << (6 - n)) - 1);
    for (int k = 0; k < n; ++k) {
      c = static_cast<unsigned char>(s[i++]);
      require((c & 0xc0) == 0x80, "invalid UTF-8 continuation");
      point = (point << 6) | (c & 63);
    }
    require((n != 1 || point >= 0x80) && (n != 2 || point >= 0x800) &&
                (n != 3 || point >= 0x10000) && point <= 0x10ffff &&
                !(point >= 0xd800 && point <= 0xdfff),
            "invalid UTF-8 codepoint");
  }
}

enum class Type { NIL, BIGINT, DOUBLE, BOOLEAN, VARCHAR, DATE, DATETIME };
enum class Kind { AUTO, STRING, NUMBER, BOOLEAN, NIL };
struct Cell {
  std::string text;
  Kind kind = Kind::NIL;
  Cell() = default;
  Cell(std::string value, Kind value_kind = Kind::AUTO)
      : text(std::move(value)), kind(value_kind) {}
  bool null() const { return kind == Kind::NIL; }
};
struct Column {
  std::string name;
  std::string source;
  Type type = Type::NIL;
  bool nullable = false;
  int scale = 0;
};
using Row = std::vector<Cell>;
using Schema = std::vector<Column>;
struct Options {
  std::string format;
  std::string path;
  bool header = true;
  char delimiter = ',';
  char quote = '"';
  char escape = '"';
  std::string nullstr;
  bool describe = false;
};
inline bool integer(const std::string &s, int64_t &value) {
  if (s.empty()) {
    return false;
  }
  const char *begin = s.data();
  if (*begin == '+') {
    if (s.size() == 1) {
      return false;
    }
    ++begin;
  }
  const auto result = std::from_chars(begin, s.data() + s.size(), value);
  return result.ec == std::errc{} && result.ptr == s.data() + s.size();
}
inline bool real(const std::string &s, double &value) {
  if (s.empty() ||
      s.find_first_not_of("0123456789+-.eE") != std::string::npos) {
    return false;
  }
  char *end = nullptr;
  errno = 0;
  value = std::strtod(s.c_str(), &end);
  return end == s.c_str() + s.size() && errno != ERANGE && std::isfinite(value);
}
inline bool calendar(const std::string &s, int64_t &days, int64_t &micros) {
  if (s.size() < 10 || s[4] != '-' || s[7] != '-') {
    return false;
  }
  auto digits = [&](std::size_t start, std::size_t len, int &value) {
    value = 0;
    if (start + len > s.size()) {
      return false;
    }
    for (std::size_t i = start; i < start + len; ++i) {
      if (s[i] < '0' || s[i] > '9') {
        return false;
      }
      value = value * 10 + s[i] - '0';
    }
    return true;
  };
  int y, m, d;
  if (!digits(0, 4, y) || !digits(5, 2, m) || !digits(8, 2, d) || y < 1 ||
      m < 1 || m > 12) {
    return false;
  }
  const bool leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
  const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (d < 1 || d > month_days[m - 1] + (m == 2 && leap)) {
    return false;
  }
  y -= m <= 2;
  const int era = y / 400;
  const unsigned yy = y - era * 400;
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  days = era * 146097 + yy * 365 + yy / 4 - yy / 100 + doy - 719468;
  micros = days * 86400000000LL;
  if (s.size() == 10) {
    return true;
  }
  if (s.size() < 19 || (s[10] != 'T' && s[10] != ' ') || s[13] != ':' ||
      s[16] != ':') {
    return false;
  }
  int hour, minute, second;
  if (!digits(11, 2, hour) || !digits(14, 2, minute) ||
      !digits(17, 2, second) || hour > 23 || minute > 59 || second > 59) {
    return false;
  }
  micros += ((hour * 60 + minute) * 60 + second) * 1000000LL;
  if (s.size() > 19) {
    if (s[19] != '.' || s.size() < 21 || s.size() > 26) {
      return false;
    }
    int fraction;
    if (!digits(20, s.size() - 20, fraction)) {
      return false;
    }
    for (std::size_t i = s.size() - 20; i < 6; ++i) {
      fraction *= 10;
    }
    micros += fraction;
  }
  return true;
}
inline Type infer(const Cell &cell) {
  if (cell.null()) {
    return Type::NIL;
  }
  if (cell.kind == Kind::BOOLEAN) {
    return Type::BOOLEAN;
  }
  int64_t number, days, micros;
  double dbl;
  if (cell.kind == Kind::AUTO || cell.kind == Kind::NUMBER) {
    const std::string normalized = lower(cell.text);
    if (cell.kind == Kind::AUTO &&
        (normalized == "true" || normalized == "false")) {
      return Type::BOOLEAN;
    }
    if (integer(cell.text, number)) {
      return Type::BIGINT;
    }
    // Preserve out-of-range integers as text rather than silently rounding.
    if (cell.text.find_first_not_of("0123456789+-") != std::string::npos &&
        real(cell.text, dbl)) {
      return Type::DOUBLE;
    }
  }
  if (cell.kind != Kind::NUMBER && calendar(cell.text, days, micros)) {
    return cell.text.size() == 10 ? Type::DATE : Type::DATETIME;
  }
  return Type::VARCHAR;
}
inline Type widen(Type a, Type b) {
  if (a == Type::NIL) {
    return b;
  }
  if (b == Type::NIL || a == b) {
    return a;
  }
  if ((a == Type::DATE && b == Type::DATETIME) ||
      (b == Type::DATE && a == Type::DATETIME)) {
    return Type::DATETIME;
  }
  auto numeric = [](Type t) {
    return t == Type::BIGINT || t == Type::DOUBLE || t == Type::BOOLEAN;
  };
  if (numeric(a) && numeric(b)) {
    return a == Type::DOUBLE || b == Type::DOUBLE ? Type::DOUBLE : Type::BIGINT;
  }
  return Type::VARCHAR;
}
inline const char *type_name(Type t) {
  switch (t) {
  case Type::BIGINT:
    return "BIGINT";
  case Type::DOUBLE:
    return "DOUBLE";
  case Type::BOOLEAN:
    return "BOOLEAN";
  case Type::DATE:
    return "DATE";
  case Type::DATETIME:
    return "DATETIME";
  default:
    return "VARCHAR";
  }
}
inline void put(std::string &out, const std::string &s) {
  out += std::to_string(s.size()) + ":" + s;
}
inline std::string take(const std::string &s, std::size_t &offset) {
  const auto colon = s.find(':', offset);
  require(colon != std::string::npos && colon - offset < 20,
          "invalid scan descriptor");
  uint64_t length = 0;
  const auto parsed =
      std::from_chars(s.data() + offset, s.data() + colon, length);
  require(parsed.ec == std::errc{} && parsed.ptr == s.data() + colon &&
              length <= s.size() - colon - 1,
          "invalid scan descriptor length");
  offset = colon + 1;
  std::string value = s.substr(offset, length);
  offset += length;
  return value;
}

#ifndef _WIN32
class Input {
public:
  Input(const std::string &path, const std::string &root) : fd_(-1) {
    require(path.find('\0') == std::string::npos &&
                root.find('\0') == std::string::npos,
            "NUL in path");
    require(path.find("://") == std::string::npos,
            "remote files are not supported");
    require(!root.empty() && lower(root) != "null",
            "secure_file_priv disallows file access");
    char allowed[PATH_MAX], actual[PATH_MAX];
    require(::realpath(root.c_str(), allowed) != nullptr,
            "invalid secure_file_priv directory");
    require(::realpath(path.c_str(), actual) != nullptr,
            "file does not exist: " + path);
    root_ = allowed;
    path_ = actual;
    require(root_ == "/" || (path_.size() > root_.size() &&
                             path_.compare(0, root_.size(), root_) == 0 &&
                             path_[root_.size()] == '/'),
            "path escapes secure_file_priv");
    int parent =
        ::open(allowed, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    require(parent >= 0, "cannot open secure directory");
    std::string relative =
        root_ == "/" ? path_.substr(1) : path_.substr(root_.size() + 1);
    std::size_t start = 0;
    while (true) {
      const auto end = relative.find('/', start);
      const auto component = relative.substr(start, end - start);
      const int next =
          ::openat(parent, component.c_str(),
                   O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
                       (end == std::string::npos ? O_NONBLOCK : O_DIRECTORY));
      ::close(parent);
      require(next >= 0, "cannot safely open file: " + path_);
      if (end == std::string::npos) {
        fd_ = next;
        break;
      }
      parent = next;
      start = end + 1;
    }
    if (::fstat(fd_, &snapshot_) != 0 || !S_ISREG(snapshot_.st_mode)) {
      ::close(fd_);
      fd_ = -1;
      require(false, "input must be a regular file");
    }
  }
  ~Input() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }
  Input(const Input &) = delete;
  Input &operator=(const Input &) = delete;
  int64_t size() const { return snapshot_.st_size; }
  std::string fingerprint() const {
    return path_ + ":" + std::to_string(snapshot_.st_dev) + ":" +
           std::to_string(snapshot_.st_ino) + ":" +
           std::to_string(snapshot_.st_size) + ":" +
           std::to_string(snapshot_.st_mtim.tv_sec) + ":" +
           std::to_string(snapshot_.st_mtim.tv_nsec) + ":" +
           std::to_string(snapshot_.st_ctim.tv_sec) + ":" +
           std::to_string(snapshot_.st_ctim.tv_nsec);
  }
  void unchanged() const {
    struct stat current {
    }, named{};
    require(::fstat(fd_, &current) == 0 && ::stat(path_.c_str(), &named) == 0 &&
                current.st_dev == snapshot_.st_dev &&
                current.st_ino == snapshot_.st_ino &&
                named.st_dev == snapshot_.st_dev &&
                named.st_ino == snapshot_.st_ino &&
                current.st_size == snapshot_.st_size &&
                current.st_mtim.tv_sec == snapshot_.st_mtim.tv_sec &&
                current.st_mtim.tv_nsec == snapshot_.st_mtim.tv_nsec &&
                current.st_ctim.tv_sec == snapshot_.st_ctim.tv_sec &&
                current.st_ctim.tv_nsec == snapshot_.st_ctim.tv_nsec,
            "input file changed during query");
  }
  std::size_t read(char *buffer, std::size_t count) {
    ssize_t n;
    do {
      n = ::read(fd_, buffer, count);
    } while (n < 0 && errno == EINTR);
    require(n >= 0, "file read failed");
    return n;
  }
  std::string at(int64_t offset, std::size_t count) {
    require(offset >= 0 && offset <= size() &&
                count <= static_cast<uint64_t>(size() - offset),
            "truncated file");
    require(count <= 64 * 1024 * 1024, "file block exceeds memory limit");
    std::string result(count, '\0');
    std::size_t done = 0;
    while (done < count) {
      const auto n = ::pread(fd_, &result[done], count - done, offset + done);
      if (n < 0 && errno == EINTR) {
        continue;
      }
      require(n > 0, "file read failed");
      done += n;
    }
    return result;
  }
  const std::string &path() const { return path_; }

private:
  int fd_;
  struct stat snapshot_ {};
  std::string root_, path_;
};
class StreamBuffer : public std::streambuf {
public:
  explicit StreamBuffer(Input &file) : file_(file) {
    setg(buffer_.data(), buffer_.data(), buffer_.data());
  }

protected:
  int_type underflow() override {
    const auto n = file_.read(buffer_.data(), buffer_.size());
    if (n == 0) {
      return traits_type::eof();
    }
    setg(buffer_.data(), buffer_.data(), buffer_.data() + n);
    return traits_type::to_int_type(*gptr());
  }

private:
  Input &file_;
  std::array<char, 65536> buffer_;
};
#else
class Input {
public:
  Input(const std::string &, const std::string &) {
    require(false, "file SQL currently requires Linux");
  }
  int64_t size() const { return 0; }
  std::string fingerprint() const { return ""; }
  void unchanged() const {}
  std::string at(int64_t, std::size_t) { return ""; }
  const std::string &path() const { return path_; }

private:
  std::string path_;
};
class StreamBuffer : public std::stringbuf {
public:
  explicit StreamBuffer(Input &) {}
};
#endif

class FlatJson {
public:
  explicit FlatJson(const std::string &line) : text_(line) { utf8(line); }
  std::map<std::string, Cell> object(std::vector<std::string> &order) {
    std::map<std::string, Cell> result;
    space();
    expect('{');
    space();
    if (peek() == '}') {
      ++pos_;
    } else {
      while (true) {
        const auto key = string();
        space();
        expect(':');
        space();
        require(result.count(key) == 0, "duplicate JSON field: " + key);
        require(result.size() < MAX_COLUMNS, "JSON field limit exceeded");
        result.emplace(key, value());
        order.push_back(key);
        space();
        if (peek() == '}') {
          ++pos_;
          break;
        }
        expect(',');
        space();
      }
    }
    space();
    require(pos_ == text_.size(), "trailing data after JSON object");
    return result;
  }

private:
  void space() {
    while (pos_ < text_.size() &&
           (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\r' ||
            text_[pos_] == '\n')) {
      ++pos_;
    }
  }
  char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }
  void expect(char c) {
    require(peek() == c, "invalid JSON token at byte " + std::to_string(pos_));
    ++pos_;
  }
  uint32_t hex() {
    uint32_t cp = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = peek();
      ++pos_;
      require((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'),
              "invalid JSON Unicode escape");
      cp = cp * 16 + (c <= '9'   ? c - '0'
                      : c <= 'F' ? c - 'A' + 10
                                 : c - 'a' + 10);
    }
    return cp;
  }
  static void codepoint(std::string &s, uint32_t cp) {
    if (cp < 0x80) {
      s += static_cast<char>(cp);
    } else if (cp < 0x800) {
      s += static_cast<char>(0xc0 | (cp >> 6));
      s += static_cast<char>(0x80 | (cp & 63));
    } else if (cp < 0x10000) {
      s += static_cast<char>(0xe0 | (cp >> 12));
      s += static_cast<char>(0x80 | ((cp >> 6) & 63));
      s += static_cast<char>(0x80 | (cp & 63));
    } else {
      s += static_cast<char>(0xf0 | (cp >> 18));
      s += static_cast<char>(0x80 | ((cp >> 12) & 63));
      s += static_cast<char>(0x80 | ((cp >> 6) & 63));
      s += static_cast<char>(0x80 | (cp & 63));
    }
  }
  std::string string() {
    space();
    expect('"');
    std::string out;
    while (peek() != '"') {
      const unsigned char c = peek();
      require(c >= 32, "unterminated or invalid JSON string");
      ++pos_;
      if (c != '\\') {
        out += static_cast<char>(c);
        continue;
      }
      const char e = peek();
      ++pos_;
      switch (e) {
      case '"':
        out += '"';
        break;
      case '\\':
        out += '\\';
        break;
      case '/':
        out += '/';
        break;
      case 'b':
        out += '\b';
        break;
      case 'f':
        out += '\f';
        break;
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      case 'u': {
        uint32_t cp = hex();
        if (cp >= 0xd800 && cp <= 0xdbff) {
          expect('\\');
          expect('u');
          const auto low = hex();
          require(low >= 0xdc00 && low <= 0xdfff, "invalid JSON surrogate");
          cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
        } else {
          require(cp < 0xdc00 || cp > 0xdfff, "invalid JSON surrogate");
        }
        codepoint(out, cp);
        break;
      }
      default:
        require(false, "invalid JSON escape");
      }
    }
    ++pos_;
    return out;
  }
  Cell value() {
    if (peek() == '"') {
      return Cell(string(), Kind::STRING);
    }
    require(peek() != '{' && peek() != '[',
            "nested JSON objects and arrays are unsupported");
    if (text_.compare(pos_, 4, "null") == 0) {
      pos_ += 4;
      return Cell();
    }
    if (text_.compare(pos_, 4, "true") == 0) {
      pos_ += 4;
      return Cell("true", Kind::BOOLEAN);
    }
    if (text_.compare(pos_, 5, "false") == 0) {
      pos_ += 5;
      return Cell("false", Kind::BOOLEAN);
    }
    const auto begin = pos_;
    if (peek() == '-') {
      ++pos_;
    }
    require(peek() >= '0' && peek() <= '9', "invalid JSON value");
    if (peek() == '0') {
      ++pos_;
    } else {
      while (peek() >= '0' && peek() <= '9') {
        ++pos_;
      }
    }
    if (peek() == '.') {
      ++pos_;
      require(peek() >= '0' && peek() <= '9', "invalid JSON fraction");
      while (peek() >= '0' && peek() <= '9') {
        ++pos_;
      }
    }
    if (peek() == 'e' || peek() == 'E') {
      ++pos_;
      if (peek() == '+' || peek() == '-') {
        ++pos_;
      }
      require(peek() >= '0' && peek() <= '9', "invalid JSON exponent");
      while (peek() >= '0' && peek() <= '9') {
        ++pos_;
      }
    }
    return Cell(text_.substr(begin, pos_ - begin), Kind::NUMBER);
  }
  const std::string &text_;
  std::size_t pos_ = 0;
};

class RawReader {
public:
  RawReader(Input &input, const Options &options)
      : input_(input), options_(options), buffer_(input), stream_(&buffer_),
        csv_(stream_, csv_options(options)) {
    stream_.exceptions(std::ios::badbit);
    if (input.size() >= 3 && input.at(0, 3) == std::string("\xef\xbb\xbf", 3)) {
      stream_.get();
      stream_.get();
      stream_.get();
    }
  }
  bool next(std::vector<std::string> &names, Row &row) {
    input_.unchanged();
    row.clear();
    names.clear();
    if (options_.format == "csv") {
      std::vector<CsvField> fields;
      if (!csv_.next(fields)) {
        return false;
      }
      for (auto &field : fields) {
        utf8(field.text);
        std::string value;
        value.reserve(field.text.size());
        for (std::size_t k = 0; k < field.text.size(); ++k) {
          if (field.text[k] == '\r' && k + 1 < field.text.size() &&
              field.text[k + 1] == '\n') {
            continue;
          }
          value += field.text[k];
        }
        row.emplace_back(!field.quoted && value == options_.nullstr
                             ? Cell()
                             : Cell(std::move(value)));
      }
      return true;
    }
    std::string line;
    char c;
    while (stream_.get(c)) {
      if (c == '\n') {
        break;
      }
      require(line.size() < MAX_RECORD, "JSONL record exceeds size limit");
      line += c;
    }
    if (line.empty() && stream_.eof()) {
      return false;
    }
    ++line_;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    try {
      const auto object = FlatJson(line).object(names);
      for (const auto &name : names) {
        row.push_back(object.at(name));
      }
    } catch (const std::exception &e) {
      throw std::runtime_error("JSONL line " + std::to_string(line_) + ": " +
                               e.what());
    }
    return true;
  }

private:
  static CsvOptions csv_options(const Options &o) {
    CsvOptions c;
    c.delimiter = o.delimiter;
    c.quote = o.quote;
    c.escape = o.escape;
    return c;
  }
  Input &input_;
  const Options &options_;
  StreamBuffer buffer_;
  std::istream stream_;
  CsvReader csv_;
  std::size_t line_ = 0;
};
inline void unique_names(Schema &schema) {
  std::set<std::string> used;
  for (std::size_t i = 0; i < schema.size(); ++i) {
    auto &col = schema[i];
    std::string base =
        col.source.empty() ? "column" + std::to_string(i) : col.source;
    std::string name = base;
    int suffix = 2;
    while (!used.insert(lower(name)).second) {
      name = base + "__" + std::to_string(suffix++);
    }
    col.name = name;
  }
}
inline Schema infer_text(Input &input, const Options &options) {
  RawReader reader(input, options);
  Schema schema;
  Row row;
  std::vector<std::string> names;
  std::size_t rows = 0;
  if (options.format == "csv") {
    if (reader.next(names, row)) {
      for (std::size_t i = 0; i < row.size(); ++i) {
        Column c;
        c.source = options.header ? (row[i].null() ? "" : row[i].text)
                                  : "column" + std::to_string(i);
        schema.push_back(c);
      }
      if (!options.header) {
        for (std::size_t i = 0; i < row.size(); ++i) {
          schema[i].type = infer(row[i]);
          schema[i].scale =
              schema[i].type == Type::DATETIME && row[i].text.size() > 20
                  ? int(row[i].text.size() - 20)
                  : 0;
          schema[i].nullable = row[i].null();
        }
        ++rows;
      }
    }
    while (reader.next(names, row)) {
      require(row.size() == schema.size(), "CSV record " +
                                               std::to_string(rows + 2) +
                                               " has a different field count");
      for (std::size_t i = 0; i < row.size(); ++i) {
        schema[i].type = widen(schema[i].type, infer(row[i]));
        if (infer(row[i]) == Type::DATETIME) {
          schema[i].scale = std::max(
              schema[i].scale,
              row[i].text.size() > 20 ? int(row[i].text.size() - 20) : 0);
        }
        schema[i].nullable |= row[i].null();
      }
      ++rows;
    }
  } else {
    std::map<std::string, std::size_t> indexes;
    while (reader.next(names, row)) {
      std::set<std::size_t> present;
      for (std::size_t i = 0; i < names.size(); ++i) {
        auto found = indexes.find(names[i]);
        std::size_t index;
        if (found == indexes.end()) {
          require(schema.size() < MAX_COLUMNS, "schema column limit exceeded");
          index = schema.size();
          indexes[names[i]] = index;
          Column c;
          c.source = names[i];
          c.nullable = rows != 0;
          schema.push_back(c);
        } else {
          index = found->second;
        }
        schema[index].type = widen(schema[index].type, infer(row[i]));
        if (infer(row[i]) == Type::DATETIME) {
          schema[index].scale = std::max(
              schema[index].scale,
              row[i].text.size() > 20 ? int(row[i].text.size() - 20) : 0);
        }
        schema[index].nullable |= row[i].null();
        present.insert(index);
      }
      for (std::size_t i = 0; i < schema.size(); ++i) {
        if (!present.count(i)) {
          schema[i].nullable = true;
        }
      }
      ++rows;
    }
  }
  require(!schema.empty(), "cannot infer columns from empty input");
  for (auto &column : schema) {
    if (column.type == Type::NIL) {
      column.type = Type::VARCHAR;
      column.nullable = true;
    }
  }
  unique_names(schema);
  input.unchanged();
  return schema;
}

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
