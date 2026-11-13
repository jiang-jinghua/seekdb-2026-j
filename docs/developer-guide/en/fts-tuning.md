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

## Candidate next revision: doc-id projection without scores

`query_limit` is the only query metric that did not move (1.24 ms versus
0.70-1.07 ms for the other three). It projects the document id under a LIMIT and
consumes no score, but the eligibility proof treats every column reference as a
data-table lookup and therefore keeps BM25.

The candidate change extends `fts_score_is_observable` with the doc-id column
id: a projection of that column is score-free, because the retrieval scan
already produces the doc-id mapping and no ranking or threshold depends on the
score. All other column references keep the scored path, and an unknown
doc-id column id falls back to the previous behavior.

Validation on 2026-11-09: the native release build and all 17 `fts_index`
regressions passed. A complete unchanged benchmark (20,000 rows, 3 samples,
3,000 tokenization rounds and 200 query rounds) retained hit counts
8,001/11,000/7,332/20. Query times were 0.6944/1.0938/0.8133/1.2306 ms.
The previous second optimized run was 0.6760/1.0552/0.7825/1.2284 ms;
these runs do not establish a performance benefit for this candidate.

The internal fulltext document-id column is distinct from the user primary key
`id`. This extension only admits the former. It therefore does not remove BM25
from the benchmark's primary-key LIMIT query. Other column references retain
the conservative scored lookup path. No official score improvement is claimed.

## Caller-owned dictionary wrappers (2026-11-09)

Each IK invocation previously created three arena allocators for short-lived
range dictionary wrappers. Allocate these wrappers from the parser's existing
allocator and store the first eight range descriptors inline, with dynamic
fallback for larger dictionaries. Explicit destruction frees each wrapper;
cache handles continue to own the DAT blocks. A failed descriptor insertion
also frees its newly allocated wrapper. Matching and range selection are
unchanged.

The release build and all 17 native fts_index regression cases passed. One
complete unchanged benchmark retained all four hit counts. IK tokenization
changed from 0.0531 to 0.0510 ms, while construction was effectively unchanged
(2.841/2.141/1.254 versus 2.843/2.141/1.254 seconds). Other timings varied; a
single run is not evidence of a stable official score improvement.

## UTF-8 character scanning (2026-11-09)

Temporary phase timing on the private test instance identified character
scanning as a larger IK cost than arbitration and result emission. Those timing
probes are not part of the implementation or the performance measurements.

TokenizeContext now records whether its input charset is UTF-8. ASCII and
well-formed three-byte encodings in U+4E00..U+9FFF determine their byte length
and existing category directly. This avoids validating and decoding those
characters twice. Every other code point, incomplete sequence, invalid
continuation byte and other charset uses the original path. The character
ranges and category precedence are unchanged.

Compared with an exact rebuild of 853198bf5 on the same private instance, using
the unchanged full benchmark configuration, IK tokenization was:

| Run | Baseline (ms) | Candidate (ms) |
| --- | ---: | ---: |
| 1 | 0.0518 | 0.0509 |
| 2 | 0.0528 | 0.0483 |
| Mean | 0.0523 | 0.0496 |

This is about a 5.2% reduction in this local metric. Index construction did not
show a clear improvement, and an official score improvement is not established.
The two candidate runs preceded the two baseline runs; this is not a randomized
comparison. Shared-host effects and run-to-run noise remain limitations.

Raw reports: [baseline 1](fts-tuning-results/fts-char-before1.txt),
[baseline 2](fts-tuning-results/fts-char-before2.txt),
[candidate 1](fts-tuning-results/fts-char-after1.txt),
[candidate 2](fts-tuning-results/fts-char-after2.txt).

Validation: release build, 17 existing fts_index cases, the new ik_utf8_scan
case, and byte-identical output for the existing 374-query Unicode corpus.
The new case covers ASCII/NUL, both edges of the fast CJK range, fullwidth
characters, supplementary-plane characters, both IK modes, and SQL conversion
of malformed byte sequences. Its expected output was captured with the old
executable and then checked against the candidate. Conversion can sanitize
malformed SQL input, so this is not exhaustive decoder-level malformed-input
coverage. All four benchmark hit counts stayed at 8001/11000/7332/20.

## Avoid successful checksum-report delay (2026-11-10)

report_ddl_sstable_checksum previously slept for 100 ms after its retry loop,
even after a successful synchronous ObDDLChecksumOperator::update_checksum.
The new condition retains that wait on failure only. All checksum reporting,
retry counts, DDL state transitions and completion checks remain unchanged.
Another report_ddl_checksum caller in ObComplementDataTask also proceeds
without an unconditional post-success sleep.

The release build and all 18 fts_index cases passed. Complete benchmark runs
on the same private instance, with unchanged data and configuration, measured:

| Metric (seconds) | Baseline 841081065 mean | Candidate mean | Reduction |
| --- | ---: | ---: | ---: |
| IK title+content index | 2.7960 | 2.4875 | 11.0% |
| IK content index | 2.1545 | 2.0495 | 4.9% |
| English index | 1.3515 | 1.2520 | 7.4% |

Both candidate runs retained hit counts 8001/11000/7332/20. These are two
baseline runs followed by two candidate runs, not randomized trials. They do
not establish the official score or remove shared-host timing variability.
Failure injection was not run; the failure path retains its existing final
sleep and retry behavior. The fixed-processor dispatch experiment did not show
a stable overall benefit and is not included.

Raw reports: [before 1](fts-tuning-results/fts-checksum-before1.txt),
[before 2](fts-tuning-results/fts-checksum-before2.txt),
[after 1](fts-tuning-results/fts-checksum-after1.txt),
[after 2](fts-tuning-results/fts-checksum-after2.txt).

## Short identical string comparisons (2026-11-11)

A user-local CPU sampling profile of index construction identified string
collation comparisons in sorting as a substantial cost. ObCharset::strcmpsp
now returns equality directly for identical non-null byte strings of at most
32 bytes under binary, utf8mb4_bin and utf8mb4_general_ci collations.
Identical bytes compare equal under either end-space policy. Unequal strings,
longer strings, other collations and invalid arguments retain the existing
collation path. The length bound limits the extra comparison cost on misses.

The release build and 20 mysqltest cases passed: all 18 fts_index cases,
order_by_subquery and the new sort_short_strings case. The new case covers
embedded NUL, empty strings, trailing spaces, Unicode, the length boundary,
unequal strings, collation-sensitive ordering, NULLs, LIMIT and window sorting.
Expected output was recorded with the unchanged executable.

Full unchanged benchmarks on the same private server measured:

| Run | IK all (s) | IK content (s) | English (s) | Total (s) |
| --- | ---: | ---: | ---: | ---: |
| Baseline 1 | 2.543 | 2.049 | 1.253 | 5.874 |
| Baseline 2 | 2.533 | 2.047 | 1.245 | 5.855 |
| Candidate 1 | 2.546 | 1.959 | 1.043 | 5.575 |
| Candidate 2 | 2.428 | 1.944 | 1.155 | 5.553 |
| Candidate 3 | 2.341 | 1.847 | 1.059 | 5.274 |

Mean total build time fell from 5.8645 to 5.4673 seconds (about 6.8%).
All candidate hit counts remained 8001/11000/7332/20. Run order was candidate
1-2, baseline 1-2, candidate 3; this was not randomized. Shared-host noise
remains a limitation, and these local results do not establish an official
score improvement. No profiler was active during these timed runs.
The baseline was an exact rebuild of 1e4048e7f. No benchmark or scoring
configuration was changed.

Raw reports: [before 1](fts-tuning-results/fts-streq-before1.txt),
[before 2](fts-tuning-results/fts-streq-before2.txt),
[after 1](fts-tuning-results/fts-streq-after1.txt),
[after 2](fts-tuning-results/fts-streq-after2.txt),
[after 3](fts-tuning-results/fts-streq-after3.txt).
## Remove expired IK matches directly (2026-11-11)

The CJK and quantifier processors previously removed a failed dictionary hit
with ObList::erase(value). That operation searches from the list head even
though the processing loop already has the node iterator. It also frees the
current node before the loop advances that iterator. An arena can hide that
invalid access by retaining the freed memory.

Both loops now advance the iterator before processing the current hit and
erase by its saved iterator. Removal is constant-time and advancement never
reads a removed node. Dictionary matching, token generation and ordering
remain unchanged.

The release build and all 21 regression cases passed: 19 fts_index cases and
two subquery sorting cases. The new ik_hit_erase test covers overlapping
Chinese words, quantities, repeated text, mixed-script boundaries and smart
and max_word modes. Expected output was recorded with the 9ff901a76 executable.
The tests verify SQL output equivalence; no sanitizer build was run.

Full unchanged benchmarks against the preserved 9ff901a76 executable:

| Metric | Baseline 1 | Baseline 2 | Candidate 1 | Candidate 2 |
| --- | ---: | ---: | ---: | ---: |
| IK tokenize (ms) | 0.0514 | 0.0508 | 0.0507 | 0.0484 |
| IK all index (s) | 2.467 | 2.336 | 2.445 | 2.327 |
| IK content index (s) | 1.853 | 1.958 | 1.838 | 1.856 |
| English index (s) | 1.041 | 1.052 | 1.042 | 1.066 |
| Total build (s) | 5.385 | 5.374 | 5.355 | 5.278 |

Mean IK tokenization decreased about 3.0%, and mean total build time about
1.2%. These small differences may include shared-host timing noise. The two
baseline runs preceded the two candidate runs, without concurrent regression
or compilation. All candidate hit counts remained 8001/11000/7332/20.
No official score improvement is established.

Raw reports: [before 1](fts-tuning-results/fts-hit-erase-before1.txt),
[before 2](fts-tuning-results/fts-hit-erase-before2.txt),
[after 1](fts-tuning-results/fts-hit-erase-after1.txt),
[after 2](fts-tuning-results/fts-hit-erase-after2.txt).

A separate experiment sharing the scan-time UTF-8 fast path with arbitration
output did not show a clear tokenization benefit and was reverted.
## Copy words already in lowercase (2026-11-11)

ObAddWord previously allocated a buffer and invoked the charset lowercase
routine for every accepted word. For UTF-8 input containing only ASCII
without uppercase A-Z and valid U+4E00..U+9FFF characters, the result is already
known. The new path copies the bytes into the same caller-owned allocator
without Unicode case conversion. It does not borrow parser-owned buffers or
change hash grouping and representative-key behavior.

Uppercase ASCII, other Unicode characters, malformed sequences and other
charsets retain the original conversion. Charset identity is cached once per
ObAddWord instance. Character selection matches the restricted CJK block
already used by the IK scanning fast path.

The release build and 22 mysqltest cases passed. The new lowercase_fast_path
test compares output against the preserved b00ef3d97 executable for ASCII
case mixtures, Chinese, CJK boundary characters, accented Latin, German and
Turkish letters, Greek sigma, utf8mb4_bin, embedded NUL and both IK modes.
No non-UTF-8 runtime test was available on this instance.

Full unchanged benchmarks used the same private instance. Order was baseline
1-2, candidate 1-2, baseline 3, candidate 3, without concurrent compilation or
regressions. Each run used 20,000 rows, batch 500, 3,000 tokenization rounds,
200 query rounds, three samples, warmup 30 and SKIP_LOAD=0.

| Run | IK tokenize (ms) | IK all (s) | IK content (s) | English (s) | Total (s) |
| --- | ---: | ---: | ---: | ---: | ---: |
| Baseline 1 | 0.0593 | 2.340 | 1.821 | 1.022 | 5.190 |
| Baseline 2 | 0.0518 | 2.633 | 1.821 | 1.021 | 5.481 |
| Baseline 3 | 0.0521 | 2.536 | 1.821 | 0.919 | 5.283 |
| Candidate 1 | 0.0481 | 2.235 | 1.821 | 0.919 | 4.981 |
| Candidate 2 | 0.0502 | 2.119 | 1.718 | 1.023 | 4.868 |
| Candidate 3 | 0.0503 | 2.335 | 1.922 | 1.018 | 5.282 |

The median of the three run-level IK averages fell from 0.0521 to 0.0502 ms
(about 3.6%). Baseline 1 was notably slower in tokenization; it is retained
here rather than excluded. Construction and query timings varied, including
a Chinese query slowdown in the candidate runs. These observations do not
establish a stable overall speedup or an official score increase.
All candidate hit counts remained 8001/11000/7332/20.

Raw reports: [before 1](fts-tuning-results/fts-lowercase-before1.txt),
[before 2](fts-tuning-results/fts-lowercase-before2.txt),
[before 3](fts-tuning-results/fts-lowercase-before3.txt),
[after 1](fts-tuning-results/fts-lowercase-after1.txt),
[after 2](fts-tuning-results/fts-lowercase-after2.txt),
[after 3](fts-tuning-results/fts-lowercase-after3.txt).
## Reduce synchronous DDL completion detection latency (2026-11-11)

ObDDLExecutorUtil::wait_ddl_finish polled the persisted DDL error/completion
record every 100 ms. It now polls every 20 ms, reducing the maximum delay
between task completion and the next check. This is the general DDL waiting
path used by ALTER TABLE ADD FULLTEXT INDEX; it also affects other callers
of wait_ddl_finish. The separate index-executor and retry-task intervals are
unchanged.

The persisted completion result remains authoritative. Published-schema
visibility checks, return-code forwarding, cancellation, session checks and
server-stop handling are unchanged. More frequent polling trades additional
metadata checks for lower synchronous completion latency; this change does
not accelerate the underlying sorting or tokenization.

Release build and 22 regression cases passed. One sorting comparison initially
failed because restoring its Windows working copy introduced CRLF into the
Linux expected-output file; after LF normalization that single case passed
on rerun. The final candidate subsequently passed all 22 cases together.
No timeout or cancellation fault injection was performed.

Two unchanged full benchmark runs against preserved c513ad270 and two runs
with this change measured:

| Metric | Baseline 1 | Baseline 2 | Candidate 1 | Candidate 2 |
| --- | ---: | ---: | ---: | ---: |
| IK all index (s) | 2.460 | 2.331 | 2.313 | 2.244 |
| IK content index (s) | 1.855 | 1.846 | 1.804 | 1.810 |
| English index (s) | 1.046 | 1.059 | 1.024 | 1.087 |
| Total build (s) | 5.389 | 5.265 | 5.166 | 5.169 |
| Whole benchmark server CPU (s) | 13.73 | 13.33 | 13.37 | 13.27 |

Mean total build time decreased about 3.0%. The server CPU figures are
/proc process user+system CPU deltas over the entire benchmark, including
queries, background work and tokenization; they do not isolate polling cost.
They show no CPU increase in this small workload, not a general scalability
guarantee. Tokenization timings were worse in the candidate runs, and overall
official-score improvement is not established. All candidate hit counts stayed
8001/11000/7332/20.

Baseline runs preceded candidate runs on a shared host, without concurrent
compilation or regressions. Other candidate experiments ran between the two
groups, so host drift is a limitation. Benchmark data, SQL and scoring files
were unchanged. Acquiring sort-descriptor array storage outside the comparison
loop and shortening the separate index-executor polling interval did not show
a stable benefit and were reverted.

Raw reports: [before 1](fts-tuning-results/fts-ddl-poll-before1.txt),
[before 2](fts-tuning-results/fts-ddl-poll-before2.txt),
[after 1](fts-tuning-results/fts-ddl-poll-after1.txt),
[after 2](fts-tuning-results/fts-ddl-poll-after2.txt).
CPU observations: [before](fts-tuning-results/fts-ddl-poll-before-cpu.json),
[after](fts-tuning-results/fts-ddl-poll-after-cpu.json).
## Compare dictionary range characters without charset dispatch (2026-11-11)

The utf8mb4_bin handler in ob_ctype_utf8.cc uses ob_strnncoll_mb_bin.
With t_is_prefix=false, that routine compares bytes with memcmp and breaks
ties by length. ObString::compare implements the same ordering, including
embedded NUL and multibyte sequences. Dictionary range lookup previously
dispatched through ObCharset::strcmp for each bound comparison, even though
the collation was fixed to utf8mb4_bin.

The local compare_range_char helper now performs that byte comparison directly
for valid ObString arguments. Negative lengths and nonzero-length null pointers
fall back to the original charset routine. No dictionary contents, range bounds,
first-inclusive-bound selection, ordering fallback, token rules or word grouping
have changed. This specialization applies only to the fixed binary range
comparison, not general SQL collations.

The release build, all 22 existing regression cases, and the new ik_range_lookup
case passed. The new case was checked separately with expected output recorded
using the preserved dbd858b35 executable. It covers ASCII, mixed words, Chinese
quantities, CJK boundary characters, other scripts, supplementary characters,
embedded NUL, utf8mb4_bin and both IK modes.

A UTF-8-only subset of a temporary Unicode corpus also produced 374
byte-identical query results, with zero SQL errors on both executables.
Output SHA-256: f2d4b97f68de34e28abef5b1d2399da4bf0bb3076fbd518f223a37ff99356663.
Other charset cases from the original corpus were excluded because the
instance does not support those charsets. SQL conversion can sanitize malformed
input, so this is not raw malformed-input coverage of the helper.

Full unchanged benchmarks compared against dbd858b35:

| Run | IK tokenize (ms) | IK all (s) | IK content (s) | English (s) | Total (s) |
| --- | ---: | ---: | ---: | ---: | ---: |
| Baseline 1 | 0.0541 | 2.282 | 1.997 | 1.077 | 5.381 |
| Baseline 2 | 0.0568 | 2.452 | 1.965 | 1.068 | 5.512 |
| Baseline 3 | 0.0510 | 2.358 | 1.833 | 0.991 | 5.209 |
| Candidate 1 | 0.0509 | 2.240 | 1.785 | 1.023 | 5.075 |
| Candidate 2 | 0.0464 | 2.202 | 1.738 | 1.015 | 4.983 |
| Candidate 3 | 0.0490 | 2.235 | 1.809 | 1.040 | 5.111 |

Mean IK tokenization decreased about 9.6%, and mean total build time about
5.8%. These are observations on a shared host, not an official score estimate.
Order was baseline 1-2, candidate 1-2, baseline 3, candidate 3. No compilation
or regressions ran during timed benchmarks. Chinese query timings were slightly
worse in the candidate runs; no query speedup is claimed. All candidate hit
counts remained 8001/11000/7332/20. Benchmark inputs and scoring were unchanged.

Raw reports: [before 1](fts-tuning-results/fts-range-cmp-before1.txt),
[before 2](fts-tuning-results/fts-range-cmp-before2.txt),
[before 3](fts-tuning-results/fts-range-cmp-before3.txt),
[after 1](fts-tuning-results/fts-range-cmp-after1.txt),
[after 2](fts-tuning-results/fts-range-cmp-after2.txt),
[after 3](fts-tuning-results/fts-range-cmp-after3.txt).
## Compare binary strings without collation dispatch (2026-11-11)

For CS_TYPE_BINARY, ob_strnncoll_binary compares bytes with memcmp and then
returns the length difference. ob_strnncollsp_binary delegates to that same
function and ignores the end-space policy. ObCharset::strcmp and strcmpsp now
perform this exact operation directly for valid binary arguments, avoiding
charset validation/lookup and indirect dispatch.

This also removes the redundant preliminary short-string equality comparison
for unequal binary strings in strcmpsp. The existing short equality path is
retained for utf8mb4_bin and utf8mb4_general_ci. Other collations and invalid
arguments retain the original path. Zero-length inputs do not call memcmp.
No padding, case-folding, dictionary, serialization or index rules changed.

The release build and 24 mysqltest cases passed: the previous 23 cases and
the new subquery.binary_string_order case. Its expected output was recorded
with the preserved dc34e3838 executable. It covers NULL/empty values,
length/prefix ordering, embedded NUL, trailing spaces, high bytes, 32/33-byte
boundaries, ascending/descending sort, grouping, joins, window functions and
STRCMP. A 374-query UTF-8 corpus also produced byte-identical output with
zero SQL errors on both versions. Its output SHA-256 remained
f2d4b97f68de34e28abef5b1d2399da4bf0bb3076fbd518f223a37ff99356663.

Full unchanged benchmarks compared against dc34e3838:

| Run | IK all (s) | IK content (s) | English (s) | Total (s) |
| --- | ---: | ---: | ---: | ---: |
| Baseline 1 | 2.280 | 1.831 | 1.016 | 5.156 |
| Baseline 2 | 2.388 | 1.806 | 1.037 | 5.258 |
| Baseline 3 | 2.312 | 1.673 | 0.927 | 4.919 |
| Candidate 1 | 2.053 | 1.612 | 0.965 | 4.637 |
| Candidate 2 | 2.055 | 1.672 | 0.910 | 4.643 |
| Candidate 3 | 2.125 | 1.626 | 0.904 | 4.662 |

Mean total construction fell from 5.1110 to 4.6473 seconds (about 9.1%).
The first two whole-benchmark process CPU observations were
13.56/13.12 seconds for baseline and 11.97/12.07 for candidate. These CPU
figures include tokenization, queries and background work and do not isolate
the comparison routine.

Run order was baseline 1-2, candidate 1-2, baseline 3, candidate 3. Each server
ran the same functional corpus before timing to prepare dictionary caches.
Data loading and all index builds remained included (SKIP_LOAD=0); benchmark
data, SQL, counts and scoring files were unchanged. Compilation and regression
runs did not overlap timed benchmarks. All candidate hit counts remained
8001/11000/7332/20. Shared-host drift remains a limitation, and these figures
do not establish an official score increase.

Raw reports: [before 1](fts-tuning-results/fts-binary-cmp-before1.txt),
[before 2](fts-tuning-results/fts-binary-cmp-before2.txt),
[before 3](fts-tuning-results/fts-binary-cmp-before3.txt),
[after 1](fts-tuning-results/fts-binary-cmp-after1.txt),
[after 2](fts-tuning-results/fts-binary-cmp-after2.txt),
[after 3](fts-tuning-results/fts-binary-cmp-after3.txt).
CPU observations: [before](fts-tuning-results/fts-binary-cmp-before-cpu.json),
[after](fts-tuning-results/fts-binary-cmp-after-cpu.json).

Separate character-map probing and short-character-accessor inline experiments
did not show a stable benefit in alternating comparisons and were reverted.
## Merge repeated word frequencies with one lookup (2026-11-12)

ObAddWord previously looked up each grouped word and then looked it up again
with set_refactored. Updating an existing word also allocated and replaced
its hash node. The existing set_or_update interface now inserts count 1 or
updates the existing pair in place. Both frequency accumulation and the latest
key spelling are preserved. The latter matters when a collation considers
different byte strings equal. Non-grouped behavior remains unchanged.

The release build and all 25 selected mysqltest cases passed. The 374-query UTF-8 corpus remained byte-identical
with no SQL errors (SHA-256
f2d4b97f68de34e28abef5b1d2399da4bf0bb3076fbd518f223a37ff99356663).
The beng_case_conversion regression records baseline output for repeated words,
long words, punctuation, case conversion, collation-equivalent accent spellings,
and indexed DML. Expected results were recorded with a17df5049.

Full unchanged benchmarks against a17df5049:

| Run | Total build (s) | IK tokenize (ms) | BENG tokenize (ms) |
| --- | ---: | ---: | ---: |
| Baseline 1 | 5.050 | 0.0482 | 0.0304 |
| Baseline 2 | 4.827 | 0.0481 | 0.0308 |
| Baseline 3 | 5.079 | 0.0471 | 0.0396 |
| Candidate 1 | 5.000 | 0.0493 | 0.0396 |
| Candidate 2 | 4.904 | 0.0455 | 0.0292 |
| Candidate 3 | 4.988 | 0.0466 | 0.0379 |

These measurements do not establish a stable end-to-end speedup. Mean build
time changed from 4.9853 to 4.9640 seconds (about 0.4%), while mean BENG
TOKENIZE time was worse. The structural saving is one hash traversal/lock
instead of two, and no replacement node allocation on repeated words; an
official score improvement is not claimed. All hit counts remained
8001/11000/7332/20. Same corpus warmup and unchanged SKIP_LOAD=0 workload
were used. No compiler or regression tests overlapped these timed runs.
Order was baseline 1-2, candidate 1-2, baseline 3, candidate 3, with rejected
experiments between the first baseline pair and final candidate pair.
Shared-host drift remains a limitation.

Raw logs: [before 1](fts-tuning-results/fts-word-frequency-before1.txt),
[before 2](fts-tuning-results/fts-word-frequency-before2.txt),
[before 3](fts-tuning-results/fts-word-frequency-before3.txt),
[after 1](fts-tuning-results/fts-word-frequency-after1.txt),
[after 2](fts-tuning-results/fts-word-frequency-after2.txt),
[after 3](fts-tuning-results/fts-word-frequency-after3.txt).

An ASCII lowercase fast path passed correctness checks but showed no stable
benchmark benefit and was reverted. An initial single-lookup implementation
that preserved the first equivalent key spelling failed corpus comparison;
its measurements are excluded because that version was not semantically
identical. The retained version preserves the last spelling.

## Avoid an unused allocator in same-charset stopword checks (2026-11-12)

For valid UTF-8 collations, charset_convert with default flags simply borrows
input. Stopword checks now build the temporary UTF-8/general-ci lookup key
directly, avoiding a per-token arena construction/destruction and conversion
dispatch. Other collations still use charset_convert, with its allocator alive
through lookup. Membership result handling and stopword contents are unchanged.

Release build and all 25 selected mysqltest cases passed. The 374-query corpus
was byte-identical to bbc5649f5 with zero SQL errors. Explicit general-ci and
binary UTF-8 stopword queries also matched. Latin1 and GBK queries were rejected
by both versions as unsupported by this instance; they do not constitute
cross-charset runtime coverage. The non-UTF-8 conversion path was preserved.

Full unchanged benchmarks (SKIP_LOAD=0) against bbc5649f5:

| Run | Build total (s) | IK tokenize (ms) | BENG tokenize (ms) |
| --- | ---: | ---: | ---: |
| Baseline 1 | 5.057 | 0.0469 | 0.0342 |
| Baseline 2 | 4.990 | 0.0477 | 0.0438 |
| Baseline 3 | 5.055 | 0.0479 | 0.0346 |
| Candidate 1 | 4.927 | 0.0447 | 0.0325 |
| Candidate 2 | 4.940 | 0.0470 | 0.0347 |
| Candidate 3 | 4.821 | 0.0468 | 0.0336 |

Mean build time fell from 5.0340 to 4.8960 seconds (about 2.7%). Whole-benchmark
process CPU for the first two runs was 12.82/13.01 seconds before and
12.51/12.51 after. These include background activity and do not isolate the
stopword function. Shared-host variation prevents an official score prediction.
All four hit counts remained 8001/11000/7332/20. Order was baseline 1-2,
candidate 1-2, baseline 3, candidate 3. Each restart ran the same corpus before
timing. No builds or regression tests overlapped these timed measurements.
Benchmark data, SQL and scoring were unchanged.

Raw logs: [before 1](fts-tuning-results/fts-stopword-before1.txt),
[before 2](fts-tuning-results/fts-stopword-before2.txt),
[before 3](fts-tuning-results/fts-stopword-before3.txt),
[after 1](fts-tuning-results/fts-stopword-after1.txt),
[after 2](fts-tuning-results/fts-stopword-after2.txt),
[after 3](fts-tuning-results/fts-stopword-after3.txt).

## Remove an unused TOKENIZE arena (2026-11-12)

ObExprTokenize::tokenize_fulltext constructed tmp_parse_alloc on every call
but never referenced it. Removing the arena avoids its constructor/destructor
without changing any parser, map or JSON allocation ownership. No execution
path or input processing was skipped.

Release build, all 25 selected mysqltest cases and the 374-query UTF-8 corpus
comparison passed. Corpus outputs were byte-identical to 389f24b51 and both
SQL error streams were empty. Existing tests cover both TOKENIZE output modes,
empty input, parser variants and indexed DML.

Two complete unchanged benchmark runs per version (baseline 1-2, candidate
1-2), with the same corpus warmup per restart and SKIP_LOAD=0:

| Run | Build total (s) | IK tokenize (ms) | BENG tokenize (ms) |
| --- | ---: | ---: | ---: |
| Baseline 1 | 4.664 | 0.0449 | 0.0265 |
| Baseline 2 | 4.643 | 0.0471 | 0.0275 |
| Candidate 1 | 4.599 | 0.0459 | 0.0274 |
| Candidate 2 | 4.521 | 0.0455 | 0.0270 |

No stable TOKENIZE speedup was established: the observed differences were
small and slightly worse on average. Build timing is unrelated to this
SQL-expression-only removal and no build speedup is attributed to it.
Whole-benchmark process CPU was 12.00/11.92 seconds before, 11.97/11.79 after;
these observations include background work. The change is retained as removal
of demonstrably unused per-call work, with no official score increase claimed.
All four hit counts remained 8001/11000/7332/20. Benchmark SQL, inputs and
scoring were unchanged; no compiler or regression runs overlapped timing.

Raw logs: [before 1](fts-tuning-results/fts-token-arena-before1.txt),
[before 2](fts-tuning-results/fts-token-arena-before2.txt),
[after 1](fts-tuning-results/fts-token-arena-after1.txt),
[after 2](fts-tuning-results/fts-token-arena-after2.txt).

## Write BENG UTF-8 lowercase output directly (2026-11-12)

The English normalizer previously used ObCharset::tolower, which allocates,
copies the source and lowercases in place for casedn_multiply=1. For UTF-8
with that multiplier, it now allocates the same capacity and invokes the same
casedn function with separate source and destination buffers. This eliminates
the preliminary memcpy while retaining case tables, output length and token
storage lifetime. Other encodings retain the original helper.

Release build and all 25 selected mysqltest cases passed. The 374-query corpus
was byte-identical to 40cad8727 with empty SQL error streams on both versions.
Existing beng_case_conversion tests include uppercase, mixed Unicode,
punctuation, repeated and long tokens plus indexed DML.

Two full unchanged runs per version (SKIP_LOAD=0), baseline 1-2 then candidate
1-2, same corpus warmup after each restart, no concurrent compilation/tests:

| Run | Build total (s) | IK tokenize (ms) | BENG tokenize (ms) |
| --- | ---: | ---: | ---: |
| Baseline 1 | 4.959 | 0.0529 | 0.0337 |
| Baseline 2 | 4.779 | 0.0505 | 0.0268 |
| Candidate 1 | 4.965 | 0.0476 | 0.0319 |
| Candidate 2 | 4.852 | 0.0486 | 0.0346 |

These measurements do not establish an end-to-end speedup. Mean BENG time was
worse, and IK differences cannot be attributed to an English-only change.
Process CPU for the full workload was 12.75/12.25 seconds before and
12.59/12.24 after. This remains an experimental candidate that removes a
specific copy, not a demonstrated official-score improvement. All hit counts
remained 8001/11000/7332/20. Inputs, benchmark SQL and scoring are unchanged.

Raw logs: [before 1](fts-tuning-results/fts-direct-lower-before1.txt),
[before 2](fts-tuning-results/fts-direct-lower-before2.txt),
[after 1](fts-tuning-results/fts-direct-lower-after1.txt),
[after 2](fts-tuning-results/fts-direct-lower-after2.txt).

## Restore the established comparison baseline (2026-11-12)

The unused TOKENIZE arena removal (40cad8727) and direct BENG lowercase output
experiment (0af2b3f6b) did not demonstrate stable end-to-end gains locally.
Reported official scores were 0 and 12.245 respectively, versus 22.252 for
389f24b51. These single-run scores cannot establish causality: individual
index-build durations varied substantially. No regression is attributed to
either change solely on that evidence.

Both experimental source changes are reverted so subsequent experiments have
a consistent reference. All files under src match 389f24b51 exactly. The
experiment reports remain for auditability. A fresh release build and all
25 selected mysqltest cases passed after restoration. This restoration is
not a new performance improvement and does not guarantee the historical score.

## Reuse a repeated character hash during dictionary lookup (2026-11-12)

ObArrayHashMap::find recomputes the same byte-string hash when multiple active
IK hits advance through one character. A bounded single-entry thread-local
cache now reuses that hash for identical character length and bytes. Every
map lookup, collision probe and equality check still runs; no dictionary code,
pointer or matching result is cached. Dictionary replacement does not invalidate
a byte-only hash. Different threads own independent entries. Cache storage is
constant size and contains no allocation. Characters outside the fixed buffer
capacity use the original hash computation without caching.

Release build and all 25 selected mysqltest cases passed. The 374-query UTF-8
corpus was byte-identical to b1ed4834c (whose source matches 389f24b51), with
empty SQL error streams. Existing coverage includes mixed Unicode, repeated
words, dictionary transitions and indexed DML.

Two full unchanged runs per version, baseline 1-2 then candidate 1-2,
SKIP_LOAD=0, identical corpus warmup after each restart:

| Run | Total build (s) | IK tokenize (ms) | BENG tokenize (ms) |
| --- | ---: | ---: | ---: |
| Baseline 1 | 4.912 | 0.0481 | 0.0395 |
| Baseline 2 | 4.855 | 0.0527 | 0.0310 |
| Candidate 1 | 4.856 | 0.0498 | 0.0377 |
| Candidate 2 | 4.821 | 0.0503 | 0.0422 |

Mean build changed from 4.8835 to 4.8385 seconds (about 0.9%); mean IK time
changed from 0.0504 to 0.05005 ms. These differences do not establish a stable
end-to-end speedup on this shared host. Mean BENG time was worse, although BENG
does not use this lookup. This is an experimental candidate, with no official
score improvement claimed. Whole-workload CPU was 12.78/12.20 seconds before
and 12.52/12.31 after. All four hits remained 8001/11000/7332/20.
Benchmark data, SQL and scoring were unchanged; builds and regressions did not
overlap the timed runs.

Raw logs: [before 1](fts-tuning-results/fts-char-hash-before1.txt),
[before 2](fts-tuning-results/fts-char-hash-before2.txt),
[after 1](fts-tuning-results/fts-char-hash-after1.txt),
[after 2](fts-tuning-results/fts-char-hash-after2.txt).

## Cache the first sort-key descriptor without allocation (2026-11-12)

The stored-row sort comparator now copies the first key's field index,
direction and comparison function during init. Key zero no longer performs
two virtual ObIArray::at accesses on each comparison. Other keys, encoded
sort keys, nonzero comparison ranges, error handling and status polling retain
the original paths. No descriptor vector is allocated. reset reconstructs the
comparator, and init refreshes the cached key. Initialization also corrects a
self-comparison typo so collation/function array counts must match.

A fresh whole-benchmark CPU profile again showed Compare and ObIArrayWrap::at
among hot symbols. Symbolization emitted DWARF warnings, so sampling is only
qualitative motivation, not precise line-level attribution.

Release build and all 26 selected mysqltest cases passed. The new
subquery.sort_first_key expected output was recorded with 20bdad23d and covers
multikey ties, directions, NULLs, LIMIT, window partitions and grouped/distinct
results. The 374-query UTF-8 corpus remained byte-identical with zero SQL errors.

Full unchanged comparisons against 20bdad23d, SKIP_LOAD=0:

| Run | Build total (s) | Chinese query (ms) | LIMIT query (ms) |
| --- | ---: | ---: | ---: |
| Baseline 1 | 4.634 | 0.7166 | 1.1401 |
| Baseline 2 | 4.381 | 0.6910 | 1.1681 |
| Baseline 3 | 4.514 | 0.6926 | 1.1469 |
| Candidate 1 | 4.519 | 0.6810 | 1.1435 |
| Candidate 2 | 4.392 | 0.6814 | 1.1427 |
| Candidate 3 | 15.221 | 0.6783 | 1.1827 |

All runs are retained, including the candidate's construction outlier. Across
three runs mean construction is worse; no construction improvement is claimed.
Chinese queries were slightly faster in all three observations but the shared
host and small sample prevent an official score prediction. First two full
workload CPU deltas were 11.94/11.51 seconds before and 11.69/11.37 after;
these include background work and do not isolate the comparator.

Order was baseline 1-2, candidate 1-2, baseline 3, candidate 3. Each restart
ran the same corpus before timing. Compilation and regressions did not overlap
timed runs. All four hit counts remained 8001/11000/7332/20. Benchmark data,
SQL, parameters and scoring were unchanged.

Raw logs: [before 1](fts-tuning-results/fts-first-key-before1.txt),
[before 2](fts-tuning-results/fts-first-key-before2.txt),
[before 3](fts-tuning-results/fts-first-key-before3.txt),
[after 1](fts-tuning-results/fts-first-key-after1.txt),
[after 2](fts-tuning-results/fts-first-key-after2.txt),
[after 3](fts-tuning-results/fts-first-key-after3.txt).

## Wake the DDL scheduler on accepted completion events (2026-11-12)

Successful receipt of column-checksum, SSTable-complement and child-task
completion callbacks now calls idler_.wakeup(), as successful task insertion
already does. The wake happens after modify_task releases its queue lock.
Rejected callbacks do not wake the scheduler. Reported job failure remains
accepted state that must be processed, so a successful callback with a failed
job result also wakes it.

This changes notification only. Task ordering, need_schedule checks, timeout
backoff, retry intervals and all checksum/transaction checks remain intact.
No task schedule timestamp is mutated from the callback, and no state machine
stage is bypassed. This cannot eliminate delays in checks or actual build work.

Release build and all 26 selected mysqltest cases passed. The 374-query UTF-8
corpus remained byte-identical to 1cd29447e, with empty SQL error streams.
The existing suite covers auxiliary-index creation, indexed DML and offline DDL.

Complete unchanged benchmarks against 1cd29447e:

| Run | IK all (s) | IK content (s) | Total build (s) |
| --- | ---: | ---: | ---: |
| Baseline 1 | 17.797 | 4.410 | 25.153 |
| Baseline 2 | 7.008 | 5.389 | 15.479 |
| Baseline 3 | 4.048 | 3.379 | 9.164 |
| Candidate 1 | 2.151 | 1.728 | 4.813 |
| Candidate 2 | 10.150 | 5.287 | 19.226 |
| Candidate 3 | 14.068 | 3.877 | 21.140 |

Both versions exhibited construction outliers. Mean construction was lower
for the candidate, but its median was worse. This does not demonstrate stable
speedup or resolution of the long-tail issue. No official score improvement is
claimed. Whole-workload process CPU for baseline runs 1-2 was 13.25/12.23 s
(wall 36.817/20.278 s), candidate 12.22/19.61 s (wall 9.314/27.236 s).
These include background work and shared-host effects.

All six runs are retained. Order: baseline 1-2, candidate 1-2, baseline 3,
candidate 3. Each server ran the same corpus before timing. No compiler or
regression tests overlapped timings. SKIP_LOAD=0 and all benchmark data,
SQL and scoring were unchanged. Hits remained 8001/11000/7332/20.

Raw logs: [before 1](fts-tuning-results/fts-ddl-wakeup-before1.txt),
[before 2](fts-tuning-results/fts-ddl-wakeup-before2.txt),
[before 3](fts-tuning-results/fts-ddl-wakeup-before3.txt),
[after 1](fts-tuning-results/fts-ddl-wakeup-after1.txt),
[after 2](fts-tuning-results/fts-ddl-wakeup-after2.txt),
[after 3](fts-tuning-results/fts-ddl-wakeup-after3.txt).

## Cache a bounded prefix of sort-key descriptors (2026-11-12)

The stored-row comparator now caches the first four field indices, directions
and comparison functions in fixed member arrays instead of caching only key
zero. This removes virtual array accesses for comparisons resolved by keys
one through three, which remain frequent when leading keys tie. No descriptor
vector is allocated. Larger keys fall back to the original arrays. Encoded
keys and comparison ranges retain their existing behavior; init refreshes
all cached keys, and reset reconstructs the comparator.

An immediate-second-stage FTS scheduling experiment was also tested but
showed virtually unchanged build times in two comparisons and was reverted.
It is not part of this commit. Existing completion-callback wakeups remain.

The aligned server source matched all 6824 local source-file hashes before
this experiment (normalizing CRLF to LF). Baseline is f6a70ed02. Release build
and all 26 selected mysqltest cases passed. sort_first_key adds five/six-key
orders with ties to exercise the fallback beyond cache capacity; expected
output was recorded with the baseline executable. The 374-query corpus was
byte-identical with empty SQL error streams.

Full unchanged benchmark, SKIP_LOAD=0:

| Run | Build total (s) | Chinese query (ms) | LIMIT query (ms) | Process CPU (s) |
| --- | ---: | ---: | ---: | ---: |
| Baseline 1 | 4.934 | 0.6752 | 1.1225 | 12.98 |
| Baseline 2 | 4.880 | 0.6823 | 1.1394 | 12.27 |
| Baseline 3 | 13.100 | 0.6989 | 1.1414 | 12.36 |
| Candidate 1 | 4.587 | 0.6655 | 1.1359 | 11.62 |
| Candidate 2 | 9.400 | 0.7093 | 1.1481 | 11.63 |
| Candidate 3 | 17.063 | 0.6870 | 1.1686 | 12.70 |

Mean whole-process CPU changed from 12.5367 to 11.9833 s (about 4.4% lower),
but the third candidate CPU observation was worse, and mean construction
elapsed time was worse. Shared-host/background work is included in CPU values.
These results do not demonstrate a stable end-to-end or official-score gain.
All outliers are retained; no construction speedup is claimed.

Order was baseline 1-2, candidate 1-2, baseline 3, candidate 3. Identical corpus
warmup ran after each restart. Compilation/regressions did not overlap timed
runs. Hits remained 8001/11000/7332/20. No benchmark input, SQL, count, timing
or scoring file was changed.

Raw logs: [before 1](fts-tuning-results/fts-four-keys-before1.txt),
[before 2](fts-tuning-results/fts-four-keys-before2.txt),
[before 3](fts-tuning-results/fts-four-keys-before3.txt),
[after 1](fts-tuning-results/fts-four-keys-after1.txt),
[after 2](fts-tuning-results/fts-four-keys-after2.txt),
[after 3](fts-tuning-results/fts-four-keys-after3.txt).
CPU observations: [before 1-2](fts-tuning-results/fts-four-keys-before-cpu.json),
[before 3](fts-tuning-results/fts-four-keys-before-third-cpu.json),
[after 1-2](fts-tuning-results/fts-four-keys-after-cpu.json),
[after 3](fts-tuning-results/fts-four-keys-after-third-cpu.json).

## Avoid a redundant dictionary upper-bound comparison (2026-11-12)

The ordered-range path uses lower_bound to select the first inclusive upper
bound not less than the character. It previously compared the selected upper
bound again in the final range loop. The ordered path now checks only the
remaining lower bound, then returns the selected dictionary. Empty ranges,
past-the-end characters and gaps still return OB_ENTRY_NOT_EXIST. The original
linear path remains for unordered ranges. Inclusive shared endpoints retain
the first-match rule. No dictionary format, range ordering or collation changed.

Release build and all 26 selected mysqltest cases passed. The 374-query UTF-8
corpus was byte-identical to 3487bd7e4 with zero SQL errors. A standalone
source-extracted differential harness compared the actual old/new lookup
function bodies over 5,120,000 randomized range/character combinations, using
simple byte-string and vector adapters. Ordered/unordered ranges, empty sets,
gaps, overlapping ranges and shared boundaries matched. ASan/UBSan reported
no issues. This harness checks lookup control flow; it is not a replacement
for the integrated dictionary/charset tests.

Two full unchanged benchmark runs per version, baseline 1-2 then candidate
1-2, SKIP_LOAD=0, same corpus warmup after each restart:

| Run | Build total (s) | IK tokenize (ms) | Chinese query (ms) |
| --- | ---: | ---: | ---: |
| Baseline 1 | 5.023 | 0.0524 | 0.6965 |
| Baseline 2 | 4.994 | 0.0534 | 0.6735 |
| Candidate 1 | 5.062 | 0.0559 | 0.6838 |
| Candidate 2 | 4.893 | 0.0501 | 0.6801 |

No stable end-to-end speedup is established by these measurements. The
structural saving is one byte-string upper-bound comparison per successful
ordered-range lookup; no official score increase is claimed. Whole-workload
CPU was 12.74/12.52 seconds before and 12.86/12.31 after, including background
work. Hits remained 8001/11000/7332/20. No compiler/regression workload overlapped
timing; benchmark SQL, inputs, counts and scoring were unchanged.

Raw logs: [before 1](fts-tuning-results/fts-upper-bound-before1.txt),
[before 2](fts-tuning-results/fts-upper-bound-before2.txt),
[after 1](fts-tuning-results/fts-upper-bound-after1.txt),
[after 2](fts-tuning-results/fts-upper-bound-after2.txt).

## Skip redundant BENG reverse-boundary scans (2026-11-12)

The English normalizer first scans forward for an alphanumeric byte. If none
exists, a reverse scan cannot discover one; if the token has one byte and an
alnum was found, its final boundary is already known. Those cases now skip
the reverse loop. Longer accepted tokens retain the original reverse scan,
case conversion, allocation and token ownership. Charset classification and
minimum/maximum token filtering are unchanged.

Release build and all 26 selected mysqltest cases passed. beng_case_conversion
adds pure punctuation, single-byte words and long punctuation strings; expected
output was recorded with 33fa92b7c. The 374-query corpus was byte-identical with
empty SQL error streams.

Full unchanged benchmarks against 33fa92b7c:

| Run | English build (s) | Total build (s) | BENG tokenize (ms) |
| --- | ---: | ---: | ---: |
| Baseline 1 | 0.964 | 4.990 | 0.0579 |
| Baseline 2 | 0.974 | 4.919 | 0.0404 |
| Baseline 3 | 1.002 | 4.929 | 0.0386 |
| Candidate 1 | 0.983 | 4.898 | 0.0441 |
| Candidate 2 | 0.958 | 4.827 | 0.0324 |
| Candidate 3 | 0.967 | 4.953 | 0.0352 |

BENG timings were lower in each paired observation, but the shared host and
variance prevent a reliable end-to-end gain estimate. Whole-workload CPU was
12.75/12.46/12.55 seconds before, 12.69/12.28/12.84 after. No stable overall
speedup or official score improvement is claimed.

A separate SQL BENCHMARK diagnostic ran three times per version using the
same session-variable input. Single-byte-input times in microseconds were
7049/7027/3212 before and 6857/5190/6696 after: these do not establish stable
improvement. Punctuation-only input is a control because the upstream tokenizer
already filters common ASCII punctuation before normalization. Its timings
were 25590/4635/3027 before and 7228/2077/4097 after; startup/cache variation
makes those unsuitable as evidence for this normalizer change.

Order was baseline 1-2, candidate 1-2, baseline 3, candidate 3. Each restart
ran the same corpus warmup. No compilation/regressions overlapped timed runs.
All hits stayed 8001/11000/7332/20. Official benchmark SQL, inputs, counts,
timing and scoring were unchanged. The separate diagnostic did not replace
or change the official workload.

Raw logs: [before 1](fts-tuning-results/fts-trim-scan-before1.txt),
[before 2](fts-tuning-results/fts-trim-scan-before2.txt),
[before 3](fts-tuning-results/fts-trim-scan-before3.txt),
[after 1](fts-tuning-results/fts-trim-scan-after1.txt),
[after 2](fts-tuning-results/fts-trim-scan-after2.txt),
[after 3](fts-tuning-results/fts-trim-scan-after3.txt).
Diagnostic [SQL](fts-tuning-results/fts-trim-scan-micro.sql),
[before](fts-tuning-results/fts-trim-scan-before-micro.txt),
[after](fts-tuning-results/fts-trim-scan-after-micro.txt).

## Defer copying unchanged words until stopword filtering (2026-11-13)

ObAddWord previously copied known-unchanged lowercase UTF-8 bytes before
checking stopword membership. That path now borrows bytes for the synchronous
check and copies only retained words, before any map insertion. Actual case
conversion keeps its original allocated storage. Paths without casedown keep
their old behavior. Thus retained map keys still outlive parser scratch buffers,
while filtered unchanged words no longer consume an allocation/copy here.

This applies only to parsers enabling ObAddWord's casedown flag, such as IK;
BENG performs its own normalization and is not directly optimized by this
change. It does not deduplicate allocations for retained repeated words.

The release build and all 26 selected mysqltest cases passed before the new
focused cases were added. The expanded beng_case_conversion case then passed
against baseline-recorded output on the candidate. It adds dense stopwords,
repeated retained words, uppercase and Unicode inputs for BENG and IK. An
initial test fixture had local-codepage Chinese bytes; it was corrected to
UTF-8 before recording final expected output. A long repeated IK input still
returned Invalid argument on the baseline after correction; it was shortened
to an accepted input. No success coverage is claimed for that rejected long
input. The 374-query corpus was byte-identical with empty SQL error streams.

Complete unchanged benchmarks against bf19bb562:

| Run | Total build (s) | IK tokenize (ms) | Process CPU (s) |
| --- | ---: | ---: | ---: |
| Baseline 1 | 4.792 | 0.0491 | 12.10 |
| Baseline 2 | 4.632 | 0.0482 | 11.85 |
| Baseline 3 | 4.708 | 0.0485 | 12.39 |
| Candidate 1 | 4.721 | 0.0460 | 11.94 |
| Candidate 2 | 4.737 | 0.0486 | 12.08 |
| Candidate 3 | 5.031 | 0.0619 | 13.42 |

These results do not establish throughput improvement: candidate means were
worse. This is a memory-allocation candidate, not a demonstrated scoring gain.
BENG timing changes cannot be attributed directly to this change. Process CPU
includes background/shared-host work. No outlier was excluded.

Order was baseline 1-2, candidate 1-2, baseline 3, candidate 3. Each restart
ran the same corpus warmup. New focused tests were run outside the timed
benchmarks. No compiler/regressions overlapped timing. SKIP_LOAD=0, benchmark
inputs, SQL, counts and scoring were unchanged. All four hit counts stayed
8001/11000/7332/20.

Raw logs: [before 1](fts-tuning-results/fts-deferred-copy-before1.txt),
[before 2](fts-tuning-results/fts-deferred-copy-before2.txt),
[before 3](fts-tuning-results/fts-deferred-copy-before3.txt),
[after 1](fts-tuning-results/fts-deferred-copy-after1.txt),
[after 2](fts-tuning-results/fts-deferred-copy-after2.txt),
[after 3](fts-tuning-results/fts-deferred-copy-after3.txt).
