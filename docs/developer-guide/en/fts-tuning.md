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
index/query work was introduced. Benchmark scripts, SQL, data generator, score
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
