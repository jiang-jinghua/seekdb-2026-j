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
#include "sql/engine/basic/file_sql_scan.h"

namespace oceanbase {
namespace sql {
namespace file_sql {

// Compact-protocol metadata only; data pages remain page-bounded and streaming.
struct CompactValue {
  int64_t integer = 0;
  std::string binary;
  std::vector<CompactValue> list;
  std::map<int, CompactValue> fields;
  const CompactValue &field(int id) const {
    const auto i = fields.find(id);
    require(i != fields.end(),
            "missing Parquet metadata field " + std::to_string(id));
    return i->second;
  }
  int64_t number(int id, int64_t fallback = 0) const {
    auto i = fields.find(id);
    return i == fields.end() ? fallback : i->second.integer;
  }
};
class CompactReader {
public:
  explicit CompactReader(const std::string &bytes) : bytes_(bytes) {}
  std::size_t position() const { return offset_; }
  CompactValue structure(int depth = 0) {
    require(depth < 32, "Parquet metadata nesting limit");
    CompactValue result;
    int last = 0;
    while (true) {
      const auto header = byte();
      if (header == 0) {
        break;
      }
      const int type = header & 15;
      const int delta = header >> 4;
      const int id = delta ? last + delta : static_cast<int>(zigzag());
      require(id > 0 && result.fields.count(id) == 0,
              "invalid Parquet field id");
      last = id;
      require(++nodes_ < 200000, "Parquet metadata resource limit");
      result.fields.emplace(id, value(type, depth + 1));
    }
    return result;
  }

private:
  uint8_t byte() {
    require(offset_ < bytes_.size(), "truncated Parquet metadata");
    return static_cast<uint8_t>(bytes_[offset_++]);
  }
  uint64_t variable() {
    uint64_t result = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
      const auto b = byte();
      require(shift != 63 || (b & 0xfe) == 0, "Parquet integer overflow");
      result |= uint64_t(b & 127) << shift;
      if (!(b & 128)) {
        return result;
      }
    }
    require(false, "Parquet varint overflow");
    return 0;
  }
  int64_t zigzag() {
    const uint64_t x = variable();
    return static_cast<int64_t>((x >> 1) ^ uint64_t(-int64_t(x & 1)));
  }
  CompactValue value(int type, int depth) {
    CompactValue result;
    switch (type) {
    case 1:
      result.integer = 1;
      break;
    case 2:
      result.integer = 0;
      break;
    case 3:
      result.integer = static_cast<int8_t>(byte());
      break;
    case 4:
    case 5:
    case 6:
      result.integer = zigzag();
      break;
    case 7:
      for (int i = 0; i < 8; ++i) {
        byte();
      }
      break;
    case 8: {
      const auto n = variable();
      require(n <= bytes_.size() - offset_, "truncated Parquet string");
      result.binary = bytes_.substr(offset_, n);
      offset_ += n;
      break;
    }
    case 9:
    case 10: {
      const auto header = byte();
      const auto count = (header >> 4) == 15 ? variable() : header >> 4;
      require(count <= 100000 && nodes_ + count <= 200000,
              "Parquet list limit");
      nodes_ += count;
      const int child = header & 15;
      for (uint64_t i = 0; i < count; ++i) {
        result.list.push_back(child == 12 ? structure(depth)
                                          : value(child, depth + 1));
      }
      break;
    }
    case 11: {
      const auto count = variable();
      require(count < 10000, "Parquet map limit");
      if (count) {
        const auto types = byte();
        for (uint64_t i = 0; i < count; ++i) {
          value(types >> 4, depth + 1);
          value(types & 15, depth + 1);
        }
      }
      break;
    }
    case 12:
      result = structure(depth);
      break;
    default:
      require(false, "invalid Parquet compact type");
    }
    require(depth < 32, "Parquet metadata nesting limit");
    return result;
  }
  const std::string &bytes_;
  std::size_t offset_ = 0, nodes_ = 0;
};
inline uint64_t little(const std::string &s, std::size_t &pos, unsigned n) {
  require(n <= 8 && pos <= s.size() && n <= s.size() - pos,
          "truncated Parquet value");
  uint64_t v = 0;
  for (unsigned i = 0; i < n; ++i) {
    v |= uint64_t(static_cast<unsigned char>(s[pos++])) << (i * 8);
  }
  return v;
}
inline uint64_t varint(const std::string &s, std::size_t &pos) {
  uint64_t v = 0;
  for (unsigned i = 0; i < 64; i += 7) {
    const auto b = little(s, pos, 1);
    require(i != 63 || (b & 0xfe) == 0, "Parquet varint overflow");
    v |= (b & 127) << i;
    if (!(b & 128)) {
      return v;
    }
  }
  require(false, "invalid Parquet varint");
  return 0;
}
inline std::string snappy(const std::string &encoded, std::size_t expected) {
  std::size_t pos = 0;
  const auto length = varint(encoded, pos);
  require(length == expected && length <= 64 * 1024 * 1024,
          "invalid Snappy length");
  std::string out;
  out.reserve(length);
  while (pos < encoded.size() && out.size() < length) {
    const auto tag = little(encoded, pos, 1);
    const unsigned kind = tag & 3;
    uint64_t count, offset;
    if (kind == 0) {
      count = tag >> 2;
      count = count < 60 ? count + 1 : little(encoded, pos, count - 59) + 1;
      require(count <= encoded.size() - pos && count <= length - out.size(),
              "invalid Snappy literal");
      out.append(encoded, pos, count);
      pos += count;
    } else {
      if (kind == 1) {
        count = 4 + ((tag >> 2) & 7);
        offset = ((tag & 0xe0) << 3) | little(encoded, pos, 1);
      } else {
        count = 1 + (tag >> 2);
        offset = little(encoded, pos, kind == 2 ? 2 : 4);
      }
      require(offset > 0 && offset <= out.size() &&
                  count <= length - out.size(),
              "invalid Snappy copy");
      for (uint64_t i = 0; i < count; ++i) {
        out += out[out.size() - offset];
      }
    }
  }
  require(pos == encoded.size() && out.size() == length,
          "truncated Snappy block");
  return out;
}
inline std::vector<uint32_t> hybrid(const std::string &bytes, std::size_t &pos,
                                    unsigned bits, std::size_t count,
                                    std::size_t end) {
  require(bits <= 32 && end <= bytes.size(), "invalid Parquet bit width");
  std::vector<uint32_t> out;
  require(count <= 2 * 1024 * 1024, "Parquet page row limit");
  out.reserve(count);
  while (out.size() < count) {
    require(pos < end, "truncated Parquet RLE");
    const auto header = varint(bytes, pos);
    require(header != 0, "zero-length Parquet RLE run");
    if (!(header & 1)) {
      const auto n = header >> 1;
      const auto value = little(bytes, pos, (bits + 7) / 8);
      require(n <= count - out.size(), "invalid Parquet RLE count");
      out.insert(out.end(), n, value);
    } else {
      const auto groups = header >> 1;
      require(groups <= (count - out.size() + 7) / 8 &&
                  (bits == 0 || groups <= (end - pos) / bits),
              "truncated Parquet bit pack");
      const auto byte_count = groups * bits;
      require(byte_count <= end - pos, "truncated Parquet bit pack");
      const auto base = pos;
      uint64_t bit = 0;
      for (uint64_t i = 0; i < groups * 8; ++i) {
        uint32_t value = 0;
        for (unsigned b = 0; b < bits; ++b, ++bit) {
          value |= ((static_cast<unsigned char>(bytes[base + bit / 8]) >>
                     (bit % 8)) &
                    1u)
                   << b;
        }
        if (out.size() < count) {
          out.push_back(value);
        }
      }
      pos += byte_count;
    }
    require(pos <= end, "Parquet RLE exceeds section");
  }
  return out;
}
struct ParquetColumn {
  Column column;
  int physical = 0;
  int width = 0;
  bool optional = false;
  bool unsigned_integer = false;
  bool date = false;
  int timestamp_unit = 0;
};
class ParquetFile {
public:
  explicit ParquetFile(Input &input) : input_(input) {
    require(input.size() >= 12 && input.at(0, 4) == "PAR1",
            "not a Parquet file");
    const auto tail = input.at(input.size() - 8, 8);
    require(tail.substr(4) == "PAR1", "invalid Parquet footer magic");
    std::size_t p = 0;
    const auto size = little(tail, p, 4);
    require(size > 0 && size <= MAX_RECORD &&
                size <= uint64_t(input.size() - 12),
            "invalid Parquet footer length");
    footer_start_ = input.size() - 8 - size;
    const auto bytes = input.at(footer_start_, size);
    CompactReader reader(bytes);
    metadata_ = reader.structure();
    require(reader.position() == bytes.size(),
            "invalid Parquet footer trailing bytes");
    const auto &schema = metadata_.field(2).list;
    require(schema.size() > 1 && schema.size() <= MAX_COLUMNS + 1,
            "invalid Parquet schema");
    require(schema[0].number(5) == int64_t(schema.size() - 1),
            "nested Parquet schemas are unsupported");
    for (std::size_t i = 1; i < schema.size(); ++i) {
      const auto &element = schema[i];
      ParquetColumn c;
      c.column.source = element.field(4).binary;
      utf8(c.column.source);
      c.physical = element.field(1).integer;
      c.width = element.number(2);
      const auto repetition = element.number(3);
      require(repetition == 0 || repetition == 1,
              "repeated Parquet fields are unsupported");
      c.optional = repetition == 1;
      c.column.nullable = c.optional;
      switch (c.physical) {
      case 0:
        c.column.type = Type::BOOLEAN;
        break;
      case 1:
      case 2:
        c.column.type = Type::BIGINT;
        break;
      case 3:
        c.column.type = Type::DATETIME;
        break;
      case 4:
      case 5:
        c.column.type = Type::DOUBLE;
        break;
      case 6:
      case 7:
        c.column.type = Type::VARCHAR;
        break;
      default:
        require(false, "unsupported Parquet physical type");
      }
      const auto converted = element.number(6, -1);
      c.unsigned_integer = converted >= 11 && converted <= 14;
      if (converted == 6) {
        c.column.type = Type::DATE;
        c.date = true;
      } else if (converted == 9 || converted == 10) {
        c.column.type = Type::DATETIME;
        c.timestamp_unit = converted == 9 ? 1000 : 1;
      } else {
        require(converted == -1 || converted == 0 || converted == 4 ||
                    converted == 11 || converted == 12 || converted == 13 ||
                    converted == 14 || converted == 15 || converted == 16 ||
                    converted == 17 || converted == 18,
                "unsupported Parquet converted type");
      }
      auto logical = element.fields.find(10);
      if (logical != element.fields.end()) {
        const auto &fields = logical->second.fields;
        if (fields.count(10)) {
          c.unsigned_integer = fields.at(10).number(2, 1) == 0;
        }
        if (fields.count(6)) {
          c.column.type = Type::DATE;
          c.date = true;
        }
        if (fields.count(8)) {
          c.column.type = Type::DATETIME;
          const auto &unit = fields.at(8).field(2).fields;
          c.timestamp_unit = unit.count(1)   ? 1000
                             : unit.count(2) ? 1
                             : unit.count(3) ? -1000
                                             : 0;
          require(c.timestamp_unit != 0, "invalid Parquet timestamp unit");
        }
      }
      if (c.unsigned_integer && c.physical == 2) {
        c.column.type = Type::VARCHAR;
      }
      if (c.column.type == Type::DATETIME) {
        c.column.scale = 6;
      }
      columns_.push_back(c);
    }
    Schema names;
    for (const auto &c : columns_) {
      names.push_back(c.column);
    }
    unique_names(names);
    for (std::size_t i = 0; i < names.size(); ++i) {
      columns_[i].column.name = names[i].name;
    }
    for (const auto &group : metadata_.field(4).list) {
      require(group.field(1).list.size() == columns_.size(),
              "invalid Parquet row-group columns");
      require(group.number(3) >= 0, "negative Parquet row-group count");
      require(group.number(3) <= INT64_MAX - rows_,
              "Parquet row count overflow");
      rows_ += group.number(3);
    }
    require(rows_ == metadata_.field(3).integer, "Parquet row count mismatch");
  }
  Schema schema() const {
    Schema result;
    for (const auto &column : columns_) {
      result.push_back(column.column);
    }
    return result;
  }
  int64_t rows() const { return rows_; }
  class ColumnCursor {
  public:
    ColumnCursor(Input &input, const ParquetColumn &column,
                 const CompactValue &chunk)
        : input_(input), column_(column), metadata_(chunk.field(3)) {
      codec_ = metadata_.field(4).integer;
      require(codec_ == 0 || codec_ == 1,
              "unsupported Parquet compression (only plain/Snappy supported)");
      offset_ = metadata_.field(9).integer;
      const auto dict = metadata_.number(11, -1);
      if (dict >= 0) {
        offset_ = std::min(offset_, dict);
      }
      const auto length = metadata_.field(7).integer;
      require(offset_ >= 4 && offset_ <= input.size() - 8 && length >= 0 &&
                  length <= input.size() - 8 - offset_,
              "invalid Parquet column extent");
      end_ = offset_ + length;
      require(offset_ >= 4 && end_ >= offset_ && end_ <= input.size() - 8,
              "invalid Parquet column extent");
      require(metadata_.field(1).integer == column.physical,
              "Parquet column type mismatch");
      require(!chunk.fields.count(1),
              "external Parquet column files are unsupported");
    }
    Cell next() {
      while (position_ == page_.size()) {
        require(offset_ < end_, "missing Parquet data page");
        load();
      }
      return page_[position_++];
    }
    std::size_t memory() const {
      std::size_t n = page_.capacity() * sizeof(Cell) +
                      dictionary_.capacity() * sizeof(Cell);
      for (const auto &c : page_) {
        n += c.text.size();
      }
      for (const auto &c : dictionary_) {
        n += c.text.size();
      }
      return n;
    }

  private:
    std::vector<Cell> plain(const std::string &bytes, std::size_t &pos,
                            std::size_t count) {
      require(count <= 2 * 1024 * 1024, "Parquet page row limit");
      std::vector<Cell> result;
      result.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
        Cell cell;
        cell.kind = Kind::NUMBER;
        int64_t integer_value = 0;
        switch (column_.physical) {
        case 0: {
          require(pos + i / 8 < bytes.size(), "truncated Parquet boolean");
          cell = Cell(
              (static_cast<unsigned char>(bytes[pos + i / 8]) >> (i % 8)) & 1
                  ? "true"
                  : "false",
              Kind::BOOLEAN);
          break;
        }
        case 1:
          integer_value = column_.unsigned_integer
                              ? int64_t(uint32_t(little(bytes, pos, 4)))
                              : int64_t(int32_t(little(bytes, pos, 4)));
          cell.text = std::to_string(integer_value);
          break;
        case 2:
          integer_value = static_cast<int64_t>(little(bytes, pos, 8));
          cell.text = column_.unsigned_integer
                          ? std::to_string(uint64_t(integer_value))
                          : std::to_string(integer_value);
          break;
        case 3: {
          const auto nanos = little(bytes, pos, 8);
          const auto julian = little(bytes, pos, 4);
          require(julian >= 1721426 && julian <= 5373484,
                  "Parquet INT96 date out of range");
          require(nanos < 86400000000000ULL, "invalid Parquet INT96 time");
          integer_value =
              (int64_t(julian) - 2440588) * 86400000000LL + nanos / 1000;
          cell.text = std::to_string(integer_value);
          break;
        }
        case 4: {
          uint32_t bits = little(bytes, pos, 4);
          float value;
          std::memcpy(&value, &bits, 4);
          require(std::isfinite(value), "non-finite Parquet float");
          std::ostringstream s;
          s.precision(std::numeric_limits<float>::max_digits10);
          s << value;
          cell.text = s.str();
          break;
        }
        case 5: {
          uint64_t bits = little(bytes, pos, 8);
          double value;
          std::memcpy(&value, &bits, 8);
          require(std::isfinite(value), "non-finite Parquet double");
          std::ostringstream s;
          s.precision(std::numeric_limits<double>::max_digits10);
          s << value;
          cell.text = s.str();
          break;
        }
        case 6:
        case 7: {
          const auto length = column_.physical == 6 ? little(bytes, pos, 4)
                                                    : uint64_t(column_.width);
          require(length <= MAX_RECORD && length <= bytes.size() - pos,
                  "invalid Parquet string length");
          cell = Cell(bytes.substr(pos, length), Kind::STRING);
          pos += length;
          utf8(cell.text);
          break;
        }
        default:
          require(false, "unsupported Parquet type");
        }
        if (column_.timestamp_unit > 0) {
          require(integer_value <= INT64_MAX / column_.timestamp_unit &&
                      integer_value >= INT64_MIN / column_.timestamp_unit,
                  "Parquet timestamp overflow");
          cell.text = std::to_string(integer_value * column_.timestamp_unit);
        } else if (column_.timestamp_unit < 0) {
          cell.text = std::to_string(integer_value / (-column_.timestamp_unit));
        }
        result.push_back(std::move(cell));
      }
      if (column_.physical == 0) {
        pos += (count + 7) / 8;
      }
      return result;
    }
    void load() {
      page_.clear();
      position_ = 0;
      const auto header_bytes =
          input_.at(offset_, std::min<int64_t>(65536, end_ - offset_));
      CompactReader compact(header_bytes);
      const auto header = compact.structure();
      const auto uncompressed = header.field(2).integer;
      const auto compressed = header.field(3).integer;
      require(uncompressed >= 0 && uncompressed <= 32 * 1024 * 1024 &&
                  compressed >= 0 && compressed <= 32 * 1024 * 1024,
              "Parquet page exceeds memory limit");
      offset_ += compact.position();
      require(compressed <= end_ - offset_, "truncated Parquet page");
      const auto encoded = input_.at(offset_, compressed);
      offset_ += compressed;
      const auto page_type = header.field(1).integer;
      std::string data;
      if (page_type == 3) {
        const auto &v2 = header.field(8);
        require(v2.number(6) == 0, "repeated Parquet data unsupported");
        const auto levels = v2.field(5).integer;
        require(levels >= 0 && levels <= compressed && levels <= uncompressed,
                "invalid Parquet level size");
        data = encoded.substr(0, levels);
        const auto rest = encoded.substr(levels);
        data += (codec_ == 1 && v2.number(7, 1))
                    ? snappy(rest, uncompressed - levels)
                    : rest;
        require(data.size() == uint64_t(uncompressed),
                "Parquet page size mismatch");
      } else {
        data = codec_ == 1 ? snappy(encoded, uncompressed) : encoded;
        require(data.size() == uint64_t(uncompressed),
                "Parquet page size mismatch");
      }
      std::size_t pos = 0;
      if (page_type == 2) {
        const auto &dict = header.field(7);
        require(dict.number(2) == 0, "unsupported Parquet dictionary encoding");
        dictionary_ = plain(data, pos, dict.field(1).integer);
        require(memory() <= 64 * 1024 * 1024,
                "Parquet dictionary exceeds memory limit");
        return;
      }
      if (page_type == 1) {
        return;
      }
      require(page_type == 0 || page_type == 3,
              "unsupported Parquet page kind");
      const auto &page = header.field(page_type == 0 ? 5 : 8);
      const auto count = page.field(1).integer;
      require(count >= 0 && count <= 2 * 1024 * 1024,
              "invalid Parquet page value count");
      const auto encoding = page.field(page_type == 0 ? 2 : 4).integer;
      std::vector<uint32_t> defined(count, 1);
      if (column_.optional) {
        std::size_t end;
        if (page_type == 0) {
          require(page.number(3) == 3,
                  "unsupported Parquet definition encoding");
          const auto n = little(data, pos, 4);
          require(n <= data.size() - pos, "invalid Parquet definitions size");
          end = pos + n;
        } else {
          end = page.field(5).integer;
          require(end <= data.size(), "invalid Parquet definitions size");
        }
        defined = hybrid(data, pos, 1, count, end);
        pos = end;
      } else if (page_type == 3) {
        pos = page.field(5).integer;
      }
      std::size_t present = 0;
      for (auto level : defined) {
        require(level <= 1, "invalid Parquet definition level");
        present += level;
      }
      std::vector<Cell> values;
      if (encoding == 0) {
        values = plain(data, pos, present);
      } else if (encoding == 3 && column_.physical == 0) {
        const auto length = little(data, pos, 4);
        require(length <= data.size() - pos,
                "invalid Parquet boolean RLE size");
        const auto booleans = hybrid(data, pos, 1, present, pos + length);
        for (const auto value : booleans) {
          require(value <= 1, "invalid Parquet boolean");
          values.emplace_back(value ? "true" : "false", Kind::BOOLEAN);
        }
      } else if ((encoding == 2 || encoding == 8) && present != 0) {
        require(!dictionary_.empty() || present == 0,
                "missing Parquet dictionary");
        const auto bits = little(data, pos, 1);
        const auto indexes = hybrid(data, pos, bits, present, data.size());
        for (auto index : indexes) {
          require(index < dictionary_.size(),
                  "invalid Parquet dictionary index");
          values.push_back(dictionary_[index]);
        }
      } else {
        require(present == 0 && (encoding == 2 || encoding == 8),
                "unsupported Parquet value encoding");
      }
      page_.reserve(count);
      std::size_t index = 0;
      for (auto level : defined) {
        page_.push_back(level ? std::move(values[index++]) : Cell());
      }
      require(memory() <= 64 * 1024 * 1024,
              "Parquet decoded page exceeds memory limit");
    }
    Input &input_;
    const ParquetColumn &column_;
    const CompactValue &metadata_;
    int codec_ = 0;
    int64_t offset_ = 0, end_ = 0;
    std::vector<Cell> dictionary_, page_;
    std::size_t position_ = 0;
  };
  void project(const std::vector<std::size_t> &columns) {
    projected_ = columns;
    group_ = 0;
    row_ = 0;
    cursors_.clear();
  }
  bool next(Row &row) {
    input_.unchanged();
    const auto &groups = metadata_.field(4).list;
    while (group_ < groups.size() && row_ == groups[group_].number(3)) {
      ++group_;
      row_ = 0;
      cursors_.clear();
    }
    if (group_ == groups.size()) {
      return false;
    }
    if (cursors_.empty() && !projected_.empty()) {
      for (auto index : projected_) {
        require(index < columns_.size(), "invalid Parquet projected column");
        cursors_.emplace_back(new ColumnCursor(
            input_, columns_[index], groups[group_].field(1).list[index]));
      }
    }
    row.assign(columns_.size(), Cell());
    std::size_t memory = 0;
    for (std::size_t i = 0; i < cursors_.size(); ++i) {
      row[projected_[i]] = cursors_[i]->next();
      memory += cursors_[i]->memory();
      require(memory <= 128 * 1024 * 1024,
              "Parquet scan exceeds memory budget");
    }
    ++row_;
    return true;
  }

private:
  Input &input_;
  CompactValue metadata_;
  std::vector<ParquetColumn> columns_;
  int64_t footer_start_ = 0, rows_ = 0, row_ = 0;
  std::size_t group_ = 0;
  std::vector<std::size_t> projected_;
  std::vector<std::unique_ptr<ColumnCursor>> cursors_;
};

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
