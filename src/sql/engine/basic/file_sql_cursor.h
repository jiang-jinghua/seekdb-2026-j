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
#include "sql/engine/basic/file_sql_parquet.h"
#include <atomic>

namespace oceanbase {
namespace sql {
namespace file_sql {

#ifndef _WIN32
inline int directory_fd(const std::string &directory, const std::string &root,
                        std::string &actual) {
  require(!root.empty() && lower(root) != "null" &&
              directory.find('\0') == std::string::npos &&
              root.find('\0') == std::string::npos,
          "invalid or disabled secure_file_priv");
  char root_path[PATH_MAX], directory_path[PATH_MAX];
  require(::realpath(root.c_str(), root_path) != nullptr &&
              ::realpath(directory.c_str(), directory_path) != nullptr,
          "directory does not exist");
  const std::string allowed = root_path;
  actual = directory_path;
  require(allowed == "/" || actual == allowed ||
              (actual.size() > allowed.size() &&
               actual.compare(0, allowed.size(), allowed) == 0 &&
               actual[allowed.size()] == '/'),
          "directory escapes secure_file_priv");
  int fd =
      ::open(allowed.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  require(fd >= 0, "cannot open secure directory");
  const auto relative = actual == allowed ? std::string()
                        : allowed == "/"  ? actual.substr(1)
                                          : actual.substr(allowed.size() + 1);
  std::size_t pos = 0;
  while (pos < relative.size()) {
    const auto end = relative.find('/', pos);
    const auto name = relative.substr(pos, end - pos);
    const int next = ::openat(fd, name.c_str(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    ::close(fd);
    require(next >= 0, "cannot safely open directory");
    fd = next;
    if (end == std::string::npos) {
      break;
    }
    pos = end + 1;
  }
  return fd;
}
inline std::vector<std::string> discover(const std::string &pattern,
                                         const std::string &root) {
  require(pattern.find('\0') == std::string::npos &&
              pattern.find("://") == std::string::npos,
          "invalid glob pattern");
  const auto wildcard = pattern.find_first_of("*?[");
  const auto slash = pattern.rfind(
      '/', wildcard == std::string::npos ? pattern.size() : wildcard);
  const auto prefix = slash == std::string::npos ? "."
                      : slash == 0               ? "/"
                                                 : pattern.substr(0, slash);
  const auto rest =
      slash == std::string::npos ? pattern : pattern.substr(slash + 1);
  std::string actual;
  const int first = directory_fd(prefix, root, actual);
  std::vector<std::string> parts;
  std::size_t pos = 0;
  while (pos < rest.size()) {
    const auto end = rest.find('/', pos);
    auto part = rest.substr(pos, end - pos);
    require(part != "." && part != ".." && !part.empty(),
            "invalid glob component");
    parts.push_back(part);
    if (end == std::string::npos) {
      break;
    }
    pos = end + 1;
  }
  std::vector<std::string> files;
  std::size_t bytes = 0;
  std::function<void(int, const std::string &, std::size_t)> visit;
  visit = [&](int fd, const std::string &path, std::size_t depth) {
    if (depth >= parts.size()) {
      return;
    }
    require(depth < 64, "glob nesting limit");
    DIR *directory = ::fdopendir(::dup(fd));
    require(directory != nullptr, "cannot enumerate directory");
    try {
      while (auto *entry = ::readdir(directory)) {
        if (::fnmatch(parts[depth].c_str(), entry->d_name, FNM_PERIOD) != 0) {
          continue;
        }
        struct stat st {};
        require(::fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0,
                "file changed during discovery");
        const auto next_path =
            (path == "/" ? path : path + "/") + entry->d_name;
        if (depth + 1 == parts.size()) {
          if (S_ISREG(st.st_mode)) {
            bytes += next_path.size();
            require(files.size() < 10000 && bytes < MAX_RECORD,
                    "glob resource limit");
            files.push_back(next_path);
          }
        } else if (S_ISDIR(st.st_mode)) {
          int next = ::openat(fd, entry->d_name,
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
          require(next >= 0, "directory changed during discovery");
          try {
            visit(next, next_path, depth + 1);
          } catch (...) {
            ::close(next);
            throw;
          }
          ::close(next);
        }
      }
    } catch (...) {
      ::closedir(directory);
      throw;
    }
    ::closedir(directory);
  };
  try {
    visit(first, actual, 0);
  } catch (...) {
    ::close(first);
    throw;
  }
  ::close(first);
  std::sort(files.begin(), files.end());
  return files;
}
#else
inline std::vector<std::string> discover(const std::string &,
                                         const std::string &) {
  require(false, "file SQL requires Linux");
  return {};
}
#endif
struct Descriptor {
  Options options;
  Schema schema;
  std::string fingerprint;
  std::string encode() const {
    std::string s = "FSQ1";
    put(s, options.format);
    put(s, options.path);
    put(s, options.header ? "1" : "0");
    put(s, std::string(1, options.delimiter));
    put(s, std::string(1, options.quote));
    put(s, std::string(1, options.escape));
    put(s, options.nullstr);
    put(s, options.describe ? "1" : "0");
    put(s, fingerprint);
    put(s, std::to_string(schema.size()));
    for (const auto &c : schema) {
      put(s, c.name);
      put(s, c.source);
      put(s, std::to_string(int(c.type)));
      put(s, c.nullable ? "1" : "0");
      put(s, std::to_string(c.scale));
    }
    return s;
  }
  static Descriptor decode(const std::string &s) {
    require(s.compare(0, 4, "FSQ1") == 0, "invalid scan descriptor version");
    std::size_t p = 4;
    Descriptor d;
    d.options.format = take(s, p);
    d.options.path = take(s, p);
    d.options.header = take(s, p) == "1";
    auto delim = take(s, p), quote = take(s, p), escape = take(s, p);
    require(delim.size() == 1 && quote.size() == 1 && escape.size() == 1,
            "invalid CSV descriptor options");
    d.options.delimiter = delim[0];
    d.options.quote = quote[0];
    d.options.escape = escape[0];
    d.options.nullstr = take(s, p);
    d.options.describe = take(s, p) == "1";
    d.fingerprint = take(s, p);
    int64_t count;
    require(integer(take(s, p), count) && count >= 0 &&
                count <= int64_t(MAX_COLUMNS),
            "invalid schema column count");
    for (int64_t i = 0; i < count; ++i) {
      Column c;
      c.name = take(s, p);
      c.source = take(s, p);
      int64_t type;
      require(integer(take(s, p), type) && type >= 0 &&
                  type <= int(Type::DATETIME),
              "invalid schema type");
      c.type = Type(type);
      c.nullable = take(s, p) == "1";
      int64_t scale;
      require(integer(take(s, p), scale) && scale >= 0 && scale <= 6,
              "invalid date precision");
      c.scale = scale;
      d.schema.push_back(c);
    }
    require(p == s.size(), "trailing scan descriptor data");
    return d;
  }
  Schema output_schema() const {
    if (!options.describe) {
      return schema;
    }
    Schema result;
    for (const auto *name :
         {"column_name", "column_type", "null", "key", "default", "extra"}) {
      Column c;
      c.name = name;
      c.source = name;
      c.type = Type::VARCHAR;
      c.nullable = result.size() >= 3;
      result.push_back(c);
    }
    return result;
  }
};
inline Descriptor bind(Options options, const std::string &root) {
  Descriptor d;
  d.options = std::move(options);
  if (d.options.format == "glob") {
    discover(d.options.path, root);
    Column c;
    c.name = "file";
    c.source = "file";
    c.type = Type::VARCHAR;
    d.schema.push_back(c);
    return d;
  }
  Input file(d.options.path, root);
  d.options.path = file.path();
  d.fingerprint = file.fingerprint();
  if (d.options.format == "parquet") {
    d.schema = ParquetFile(file).schema();
  } else {
    require(d.options.format == "csv" || d.options.format == "json",
            "unsupported file format");
    d.schema = infer_text(file, d.options);
  }
  for (const auto &column : d.schema) {
    require(column.name.size() <= 128,
            "file column name exceeds identifier length limit");
  }
  return d;
}
class Cursor {
public:
  Cursor(Descriptor descriptor, const std::string &root,
         const std::vector<std::size_t> &projection)
      : descriptor_(std::move(descriptor)) {
    if (descriptor_.options.format == "glob") {
      files_ = discover(descriptor_.options.path, root);
      return;
    }
    file_.reset(new Input(descriptor_.options.path, root));
    require(file_->fingerprint() == descriptor_.fingerprint,
            "file changed since schema binding");
    if (!descriptor_.options.describe) {
      if (descriptor_.options.format == "parquet") {
        parquet_.reset(new ParquetFile(*file_));
        parquet_->project(projection);
      } else {
        reader_.reset(new RawReader(*file_, descriptor_.options));
        if (descriptor_.options.format == "csv" && descriptor_.options.header) {
          Row row;
          std::vector<std::string> names;
          reader_->next(names, row);
        }
      }
    }
  }
  bool next(Row &row) {
    row.clear();
    if (descriptor_.options.describe) {
      if (index_ == descriptor_.schema.size()) {
        return false;
      }
      if (file_) {
        file_->unchanged();
      }
      const auto &c = descriptor_.schema[index_++];
      row = {Cell(c.name),
             Cell(type_name(c.type)),
             Cell(c.nullable ? "YES" : "NO"),
             Cell(),
             Cell(),
             Cell()};
      return true;
    }
    if (descriptor_.options.format == "glob") {
      if (index_ == files_.size()) {
        return false;
      }
      row.emplace_back(files_[index_++]);
      return true;
    }
    if (parquet_) {
      return parquet_->next(row);
    }
    std::vector<std::string> names;
    Row raw;
    if (!reader_->next(names, raw)) {
      return false;
    }
    if (descriptor_.options.format == "csv") {
      require(raw.size() == descriptor_.schema.size(),
              "CSV field count changed");
      row = std::move(raw);
    } else {
      std::map<std::string, std::size_t> indexes;
      for (std::size_t i = 0; i < names.size(); ++i) {
        indexes[names[i]] = i;
      }
      for (const auto &column : descriptor_.schema) {
        const auto i = indexes.find(column.source);
        row.push_back(i == indexes.end() ? Cell() : std::move(raw[i->second]));
      }
    }
    return true;
  }
  const Descriptor &descriptor() const { return descriptor_; }

private:
  Descriptor descriptor_;
  std::unique_ptr<Input> file_;
  std::unique_ptr<RawReader> reader_;
  std::unique_ptr<ParquetFile> parquet_;
  std::vector<std::string> files_;
  std::size_t index_ = 0;
};

class CsvOutput {
public:
  CsvOutput(const Options &options, const std::string &root)
      : options_(options) {
#ifndef _WIN32
    require(options.path.find('\0') == std::string::npos &&
                options.path.find("://") == std::string::npos,
            "invalid COPY target");
    const auto slash = options.path.rfind('/');
    const auto parent = slash == std::string::npos ? "."
                        : slash == 0               ? "/"
                                     : options.path.substr(0, slash);
    target_ = slash == std::string::npos ? options.path
                                         : options.path.substr(slash + 1);
    require(!target_.empty() && target_ != "." && target_ != "..",
            "invalid COPY target name");
    std::string actual;
    parent_ = directory_fd(parent, root, actual);
    struct stat st {};
    if (::fstatat(parent_, target_.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
      ::close(parent_);
      parent_ = -1;
      require(false, "COPY target already exists");
    }
    static std::atomic<uint64_t> counter{0};
    for (int i = 0; i < 32 && fd_ < 0; ++i) {
      temporary_ = ".seekdb-copy-" + std::to_string(::getpid()) + "-" +
                   std::to_string(++counter);
      fd_ =
          ::openat(parent_, temporary_.c_str(),
                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
      if (fd_ < 0 && errno != EEXIST) {
        break;
      }
    }
    if (fd_ < 0) {
      ::close(parent_);
      parent_ = -1;
      require(false, "cannot create COPY temporary file");
    }
#else
    (void)root;
    require(false, "COPY requires Linux");
#endif
  }
  ~CsvOutput() {
#ifndef _WIN32
    if (fd_ >= 0) {
      ::close(fd_);
    }
    if (parent_ >= 0) {
      if (!published_) {
        ::unlinkat(parent_, temporary_.c_str(), 0);
      }
      ::close(parent_);
    }
#endif
  }
  void row(const Row &row) {
    std::string encoded;
    for (std::size_t i = 0; i < row.size(); ++i) {
      if (i) {
        encoded += options_.delimiter;
      }
      const auto &cell = row[i];
      if (cell.null()) {
        encoded += options_.nullstr;
        continue;
      }
      const auto &s = cell.text;
      const bool quote = s.empty() || s == options_.nullstr ||
                         s.find(options_.delimiter) != std::string::npos ||
                         s.find(options_.quote) != std::string::npos ||
                         s.find('\n') != std::string::npos ||
                         s.find('\r') != std::string::npos;
      if (quote) {
        encoded += options_.quote;
      }
      for (char c : s) {
        if (quote && (c == options_.quote || c == options_.escape)) {
          encoded += options_.escape;
        }
        encoded += c;
        require(encoded.size() <= MAX_RECORD, "COPY record exceeds size limit");
      }
      if (quote) {
        encoded += options_.quote;
      }
    }
    require(encoded.size() <= MAX_RECORD, "COPY record exceeds size limit");
    encoded += '\n';
#ifndef _WIN32
    std::size_t written = 0;
    while (written < encoded.size()) {
      const auto n =
          ::write(fd_, encoded.data() + written, encoded.size() - written);
      if (n < 0 && errno == EINTR) {
        continue;
      }
      require(n > 0, "COPY write failed");
      written += n;
    }
#endif
  }
  void commit() {
#ifndef _WIN32
    require(::fsync(fd_) == 0, "COPY flush failed");
    require(
        ::linkat(parent_, temporary_.c_str(), parent_, target_.c_str(), 0) == 0,
        "COPY target exists or cannot be published");
    ::unlinkat(parent_, temporary_.c_str(), 0);
    published_ = true;
#endif
  }

private:
  Options options_;
  int parent_ = -1, fd_ = -1;
  std::string temporary_, target_;
  bool published_ = false;
};

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
