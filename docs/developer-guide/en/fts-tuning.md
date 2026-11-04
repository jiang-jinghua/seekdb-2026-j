# Full-text parser hot-path tuning

This change reduces repeated character work in native IK and basic-English
tokenization. It preserves the dictionaries, token boundaries, category
precedence, stopword rules, token frequencies, and MATCH execution semantics.

## Bottlenecks and changes

- IK character classification previously decoded the same code point for each
  category predicate. Decode once, then apply the same predicates in the same
  order. UTF-16 surrogate handling and decoder errors retain their existing paths.
- The IK parser already obtains the current character, length, and type before
  visiting its four segmenters. Pass that state to `do_process` rather than
  retrieving it again in each processor. None of those processors advances the
  input cursor; their invocation order is unchanged.
- Dictionary lookup previously scanned every cached range. Validate monotonic
  starts and ends while constructing the range view, then use a lower bound on
  inclusive upper endpoints. An unordered range view retains the original
  linear scan. Shared endpoints continue selecting the first matching range.
- The English whitespace tokenizer uses length one for ASCII in charsets whose
  minimum character length is one. Non-ASCII and fixed-width encodings retain
  the multibyte decoder. Delimiter classification is unchanged.

No input/result cache, benchmark-specific branch, reduced input, or skipped
necessary index/query work was introduced. Benchmark scripts, SQL, data generator, score
calculator and checked-in CI baseline are unchanged.

## Local measurement

The original executable was preserved before rebuilding. Both versions ran on
the same Ubuntu x86-64 server using the same clang-17 release configuration and
the same dedicated instance, with no concurrent benchmark/regression process.
The repository-pinned VSAG dependency was used for both versions.

Every run used the unmodified `tools/benchmark/fts_large_bench.sh` with
`ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`.
Each complete run used a separate report file and rebuilt all three indexes.
The table averages two subsequent baseline runs and two runs of the final
optimized version. The initial cold-start measurement is excluded from this
comparison because its index construction times were substantially higher.

| Metric | Before | After | Time reduction |
| --- | ---: | ---: | ---: |
| `build_ik_all_sec` | 2.84250 | 2.75600 | 3.04% |
| `build_ik_content_sec` | 2.15750 | 2.10650 | 2.36% |
| `build_beng_en_sec` | 1.25200 | 1.25350 | -0.12% |
| `tokenize_ik_avg_ms` | 0.05540 | 0.05125 | 7.49% |
| `tokenize_beng_avg_ms` | 0.03875 | 0.03965 | -2.32% |
| `query_cn_avg_ms` | 0.92960 | 0.93555 | -0.64% |
| `query_beng_avg_ms` | 1.42695 | 1.39385 | 2.32% |
| `query_mixed_avg_ms` | 1.05675 | 1.07330 | -1.57% |
| `query_limit_avg_ms` | 1.22920 | 1.23795 | -0.71% |

Raw reports: [before 2](fts-tuning-results/before2.txt),
[before 3](fts-tuning-results/before3.txt),
[after 2](fts-tuning-results/after2.txt),
[after 3](fts-tuning-results/after3.txt).

The clearest local improvement is IK tokenization. English and query differences
are small relative to observed run/sample variation; no stable improvement is
claimed for those categories. This work does not demonstrate a 50% overall time
reduction. Local hardware differs from CI, so these measurements do not predict
the official competition score. Hardware profiling events were unavailable to
the test account; no shared system profiling settings were changed.

## Correctness validation

The native build passed all 16 discovered `fts_index` mysqltest cases, including
`basic_dml`, `basic_dml_sequel`, `cn_word`, `create_fts_index_afterward`,
`create_table_with_fts_index`, `domain_id_merge_with_compare_null`, `drop_index`,
`ik_hot_path`, `index_view`, `load_data`, `offline_ddl_with_fts_index`,
`online_ddl_on_table_with_fts_index`, `parser_properties`,
`partitioned_simple_query`, `simple_query`, and `tokenize_function`.

The new `ik_hot_path` expected output was recorded with the original executable
and compared with the optimized executable. It covers default/detail output,
smart and max-word modes, mixed scripts, supplementary characters, full-width letters/digits,
punctuation, long inputs, ASCII whitespace, and embedded NUL for BENG.

A separate deterministic Unicode corpus produced 374 successful UTF-8
tokenization results identical byte for byte before and after. Unsupported
UTF-16/UTF-16LE SQL conversion errors were also unchanged; these errors do not
constitute successful UTF-16 tokenization coverage. All benchmark MATCH hit
counts agreed across the four measured runs. Correctness is established by the
regressions and output comparisons, rather than hit counts alone.

## Revision after platform evaluation: membership-only retrieval

The submitted `6a44170af` received 0.048/50 on the platform, with reported mean
improvement 0.0004813965. The first-stage local parser improvements were not a
meaningful improvement in that evaluation. This revision instead removes
unobservable BM25 work from eligible native retrieval scans.

The optimizer requires a mandatory positive boolean MATCH condition in a
single-table SELECT with no subquery expressions. It excludes boolean/ES modes,
explicit column boosts, visible scores, score thresholds, and references to
ordinary columns outside MATCH. Thus index-only membership queries such as
COUNT(*) can avoid scoring; returning table columns, ranking, and numerical score
uses retain the original path. The proof does not depend on query text, input
data, or benchmark names.

The legacy executor now tolerates absent BM25 aggregate scans, skips initializing
unused estimators and score filters, and allocates its document-ID batch mask
independently of scoring. Membership-only token iteration does not estimate
document frequencies. Score-free batches use a positive membership value rather
than zero; numerical score consumers are excluded from this mode. Long-query
TAAT execution initializes its hash/spill partition without BM25 estimates and
uses the posting scan's range when no score aggregate scan exists.

All 17 native `fts_index` regressions passed. The added `membership_score` test
compares against original-executable output for returned IDs, actual scores,
ranking, common/missing/empty terms, LIMIT, score thresholds, boolean mode, and
prepared threshold changes. An additional prepared-search-term sequence checks
apple/missingword/banana/empty/apple reuse, followed by short/300-term/missing
300-term/short queries to exercise the algorithm switch. Existing golden-result changes are
restricted to four EXPLAIN lines changing `calc_relevance=true` to `false`; query
result expectations are unchanged.

For the comparison below, the exact submitted revision's six affected source
files were restored in the isolated build and compiled with the same compiler
and flags. Separate original and optimized executables were retained. Both ran
the unchanged full benchmark twice on the same instance, with the configuration
above and no concurrent compiler or regression process. Values are run means.

| Metric | Submitted revision | New revision | Time reduction |
| --- | ---: | ---: | ---: |
| `build_ik_all_sec` | 2.84200 | 2.89800 | -1.97% |
| `build_ik_content_sec` | 2.14550 | 2.15250 | -0.33% |
| `build_beng_en_sec` | 1.30100 | 1.23750 | 4.88% |
| `tokenize_ik_avg_ms` | 0.05020 | 0.05220 | -3.98% |
| `tokenize_beng_avg_ms` | 0.03945 | 0.03210 | 18.63% |
| `query_cn_avg_ms` | 0.94500 | 0.68830 | 27.16% |
| `query_beng_avg_ms` | 1.40345 | 1.06210 | 24.32% |
| `query_mixed_avg_ms` | 1.07400 | 0.79270 | 26.19% |
| `query_limit_avg_ms` | 1.23975 | 1.23745 | 0.19% |

Raw reports: [submitted 1](fts-tuning-results/before-score1.txt),
[submitted 2](fts-tuning-results/before-score2.txt),
[optimized 1](fts-tuning-results/after-score1.txt),
[optimized 2](fts-tuning-results/after-score2.txt).

This revision changes query execution, not construction or tokenization. Their
differences above are recorded for completeness and are not attributed to the
new optimization. All four MATCH hit counts agree across all four runs. The
three index-only query cases show the intended reduction; the LIMIT case returns
IDs internally and deliberately retains scoring. These local results do not
establish an official score or a 50% overall improvement.
