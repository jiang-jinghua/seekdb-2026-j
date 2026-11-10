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
#include "share/schema/ai_column_result_guard.h"
#include <cassert>
using namespace oceanbase::share::ai_column;
int main()
{
  const ResultIdentity original{100, 16, 1, 10, 1, 1};
  const auto pending = ColumnState::PENDING;
  assert(check_writeback(original, original, true, pending) == WritebackDecision::APPLY);
  auto current = original;
  ++current.request_id;
  assert(check_writeback(original, current, true, pending) == WritebackDecision::DISCARD_STALE);
  assert(!can_retry(original, current, true, pending, 0, 2));
  current = original;
  ++current.row_generation;
  assert(check_writeback(original, current, true, pending) == WritebackDecision::DISCARD_STALE);
  current = original;
  ++current.definition_version;
  assert(check_writeback(original, current, true, pending) == WritebackDecision::DISCARD_STALE);
  current = original;
  ++current.result_spec_version;
  assert(check_writeback(original, current, true, pending) == WritebackDecision::DISCARD_STALE);
  assert(check_writeback(original, original, false, pending) == WritebackDecision::DISCARD_STALE);
  assert(check_writeback(original, original, true, ColumnState::READY) == WritebackDecision::DISCARD_STALE);
  assert(check_writeback(original, original, true, ColumnState::FAILED) == WritebackDecision::DISCARD_STALE);
  assert(can_retry(original, original, true, pending, 0, 2));
  assert(can_retry(original, original, true, pending, 1, 2));
  assert(!can_retry(original, original, true, pending, 2, 2));
  assert(!can_retry(original, original, true, pending, -1, 2));
  assert(!can_retry(original, original, true, pending, 0, 0));
  assert(check_writeback(ResultIdentity{}, original, true, pending) == WritebackDecision::INVALID_IDENTITY);
}
