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
#include "share/schema/ai_column_definition.h"
#include <cassert>
#include <iostream>
using oceanbase::share::ai_column::Definition;
template <class F> void rejected(F f) {
  bool caught = false;
  try {
    f();
  } catch (const std::exception &) {
    caught = true;
  }
  assert(caught);
}
int main() {
  Definition d;
  d.parse_expression(
      "AI_COMPLETE('model', concat('a,b', content), '{\"temperature\":0}')");
  d.result_type = "JSON";
  d.dependencies = {"content"};
  d.system_prompt = "Return exactly one valid JSON value";
  assert(d.arguments.size() == 3 && d.arguments[1] == "concat('a,b', content)");
  const auto encoded = d.encode();
  const auto copy = Definition::decode(encoded);
  assert(copy.expression == d.expression &&
         copy.dependencies == d.dependencies &&
         copy.system_prompt == d.system_prompt);
  for (std::size_t i = 0; i < encoded.size(); ++i) {
    rejected([&] { Definition::decode(encoded.substr(0, i)); });
  }
  rejected([&] { Definition::decode(encoded + "junk"); });
  rejected([&] { Definition::decode("AIC199999999999999999999999999999:"); });
  rejected([&] {
    Definition invalid;
    invalid.parse_expression("AI_COMPLETE('model')");
  });
  rejected([&] {
    Definition invalid;
    invalid.parse_expression("AI_RERANK('model','input')");
  });
  rejected([&] {
    auto invalid = d;
    invalid.max_retries = -1;
    invalid.encode();
  });
  Definition embed;
  embed.parse_expression("AI_EMBED('model', body, 4)");
  embed.dimension = 4;
  embed.result_type = "VECTOR";
  assert(Definition::decode(embed.encode()).dimension == 4);
  assert(Definition::quote_identifier("a`b") == "`a``b`");
  assert(Definition::quote_literal("a'\\") ==
         "CONVERT(X'61275c' USING utf8mb4)");
  std::cout << "AI column definition regressions passed\n";
}
