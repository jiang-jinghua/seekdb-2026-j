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
#include "lib/container/ob_iarray.h"
#include "lib/string/ob_string.h"
namespace oceanbase {
namespace share {
namespace schema {
class ObTableSchema;
}
} // namespace share
namespace sql {
class ObExecContext;
class ObSQLSessionInfo;
namespace ai_column {
bool internal_execution();
bool exchange_internal_execution(bool value);
class InternalExecutionScope {
public:
  InternalExecutionScope() : saved_(exchange_internal_execution(true)) {}
  ~InternalExecutionScope() { exchange_internal_execution(saved_); }

private:
  bool saved_;
};
int register_table(ObExecContext &ctx,
                   const share::schema::ObTableSchema &schema,
                   const common::ObString &database, uint64_t table_id);
int before_execute(ObExecContext &ctx);
int after_dml(ObExecContext &ctx, const common::ObIArray<uint64_t> &tables);
int strict_read(ObSQLSessionInfo &session,
                const share::schema::ObTableSchema &schema,
                const common::ObString &database, uint64_t column_id);
int start_worker();
void stop_worker();
} // namespace ai_column
} // namespace sql
} // namespace oceanbase
