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

#include <cstdint>

namespace oceanbase {
namespace share {
namespace ai_column {

// Identity captured when an AI task is created in the source DML transaction.
// These values must be compared with the locked current row during writeback.
struct ResultIdentity {
  uint64_t table_id = 0;
  uint64_t column_id = 0;
  uint64_t row_generation = 0;
  uint64_t request_id = 0;
  int64_t definition_version = 0;
  int64_t result_spec_version = 0;

  bool valid() const {
    return table_id != 0 && column_id != 0 && row_generation != 0
        && request_id != 0 && definition_version > 0 && result_spec_version > 0;
  }
};

enum class ColumnState { PENDING, READY, FAILED };
enum class WritebackDecision { APPLY, DISCARD_STALE, INVALID_IDENTITY };

// A missing row includes DELETE and primary-key moves. Row generation prevents
// DELETE followed by INSERT with the same key from resurrecting old results.
// Transaction visibility and row locking belong to the caller; this predicate
// intentionally performs no network I/O and must run inside writeback's txn.
inline WritebackDecision check_writeback(const ResultIdentity &task,
                                        const ResultIdentity &current,
                                        bool row_exists,
                                        ColumnState current_state)
{
  if (!task.valid()) {
    return WritebackDecision::INVALID_IDENTITY;
  }
  if (!row_exists) {
    return WritebackDecision::DISCARD_STALE;
  }
  if (!current.valid()) {
    return WritebackDecision::INVALID_IDENTITY;
  }
  if (current_state != ColumnState::PENDING
      || task.table_id != current.table_id
      || task.column_id != current.column_id
      || task.row_generation != current.row_generation
      || task.request_id != current.request_id
      || task.definition_version != current.definition_version
      || task.result_spec_version != current.result_spec_version) {
    return WritebackDecision::DISCARD_STALE;
  }
  return WritebackDecision::APPLY;
}

// max_retries counts additional attempts after the initial request. Once a
// request is replaced, it must never retry or publish, regardless of its budget.
inline bool can_retry(const ResultIdentity &task, const ResultIdentity &current,
                      bool row_exists, ColumnState current_state,
                      int64_t retry_count, int64_t max_retries)
{
  return retry_count >= 0 && max_retries >= 0 && retry_count < max_retries
      && check_writeback(task, current, row_exists, current_state)
          == WritebackDecision::APPLY;
}

} // namespace ai_column
} // namespace share
} // namespace oceanbase
