# HNSW In-Traversal Filter Migration For PostgreSQL 18.4

This note records the migration of the previous hybrid vector + scalar filtering experiment to the current `/home/pgvector` tree and the installed PostgreSQL 18.4 environment.

## Source Layout

Current local versions:

```text
PostgreSQL: /home/postgres, installed through /usr/local/pgsql
pgvector:   /home/pgvector
pg_config:  /usr/local/pgsql/bin/pg_config
```

The current pgvector upstream tree did not contain `hnsw.in_traversal_filter`, so the migration reintroduced it against the latest HNSW function signatures.

## Code Changes

### GUC

Files:

```text
/home/pgvector/src/hnsw.c
/home/pgvector/src/hnsw.h
```

`hnsw.c` adds:

```c
bool hnsw_in_traversal_filter;
```

and registers:

```sql
hnsw.in_traversal_filter
```

Default is `off`.

### HNSW Search Path

Files:

```text
/home/pgvector/src/hnswscan.c
/home/pgvector/src/hnswutils.c
/home/pgvector/src/hnsw.h
```

The latest `HnswSearchLayer()` signature already included `int64 *tuples`. The migration adds one more optional argument:

```c
IndexScanDesc filterScan
```

Only the query scan path passes this argument:

```text
GetScanItems(... layer 0 ...) -> HnswSearchLayer(..., scan)
ResumeScanItems(...)          -> HnswSearchLayer(..., scan)
```

Build and insert paths pass `NULL`.

### Filtering Rule

`/home/pgvector/src/hnswutils.c` adds `ElementMatchesTidFilter()`.

When all conditions are true:

```text
hnsw.in_traversal_filter = on
filterScan != NULL
filterScan->xs_tid_filter != NULL
lc == 0
!inserting
```

the HNSW ground-layer traversal uses PostgreSQL 18's executor TID filter callback:

```c
scan->xs_tid_filter(scan, &element->heaptids[i], scan->xs_tid_filter_state)
```

Semantics:

- All graph nodes can still enter the exploration heap `C`.
- Only elements with at least one heap TID passing the scalar quals enter result heap `W`.
- This preserves neighbor traversal through filtered-out nodes while preventing filtered-out vectors from occupying the HNSW candidate result list.

`postfilter` behavior is preserved when the GUC is `off`.

## Test Scripts

### pgvector SQL Smoke

Script:

```text
/home/pgvector/scripts/test_latest_in_traversal_filter.sh
```

Run:

```bash
/home/pgvector/scripts/test_latest_in_traversal_filter.sh
```

Useful options:

```bash
BUILD_INSTALL=0 /home/pgvector/scripts/test_latest_in_traversal_filter.sh
RUN_TAP=1 /home/pgvector/scripts/test_latest_in_traversal_filter.sh
```

Default behavior:

1. Build and install pgvector with `/usr/local/pgsql/bin/pg_config`.
2. Connect to the running PostgreSQL instance on `127.0.0.1:55432`.
3. Create a temporary 1,000-row vector table.
4. Compare:
   - `postfilter`
   - `in_traversal_hnsw`
   - `in_traversal_exact`

Expected smoke output:

```text
guc,off
postfilter,0
in_traversal_hnsw,5
in_traversal_exact,5
SQL smoke passed
```

TAP tests create a temporary PostgreSQL cluster and cannot run as root. Use `RUN_TAP=1` only from a non-root user with the PostgreSQL TAP libraries available.

### pgvector TAP Test

File:

```text
/home/pgvector/test/t/049_hnsw_in_traversal_filter.pl
```

The TAP test verifies the same behavior as the SQL smoke. In this environment, running it through `make prove_installcheck` as root is blocked by PostgreSQL's `initdb` root check.

### Hacker News Benchmark Compare

Script:

```text
/home/vector-benchmark/benchmark/bash-test/pgvector-hackernews-in-traversal-compare.sh
```

Default route set:

```text
in_traversal_hnsw,in_traversal_exact
```

Quick smoke:

```bash
cd /home/vector-benchmark && \
SETUP=0 PREPARE=1 REUSE=0 \
TABLE=hackernews_openai_pgvector_100k \
RATIOS=1 QUERIES=2 K=5 CLIENTS=1 JOBS=1 DURATION=1 EF_SEARCH=8 \
SQL_DIR=/tmp/pgvector_hn_latest_in_traversal_sql_smoke \
QUERIES_TABLE=hackernews_pgbench_queries_latest_intrav_smoke \
TRUTH_TABLE=hackernews_pgbench_truth_latest_intrav_smoke \
RESULTS_TABLE=hackernews_pgbench_results_latest_intrav_smoke \
RESULT_CSV=/tmp/pgvector_hn_latest_in_traversal_smoke.csv \
/home/vector-benchmark/benchmark/bash-test/pgvector-hackernews-in-traversal-compare.sh
```

Full 1M compare:

```bash
cd /home/vector-benchmark && \
SETUP=0 PREPARE=1 REUSE=0 \
TABLE=hackernews_openai_pgvector_1m \
RATIOS=1,5,10,20,30,40,50,60,70,80,90,95,99 \
QUERIES=100 K=10 CLIENTS=8 JOBS=8 DURATION=60 EF_SEARCH=128 \
RESULT_CSV=/home/vector-benchmark/benchmark/results/pgvector_hn_in_traversal_compare_$(date +%Y%m%d_%H%M%S).csv \
/home/vector-benchmark/benchmark/bash-test/pgvector-hackernews-in-traversal-compare.sh
```

Reuse the generated SQL/test tables:

```bash
cd /home/vector-benchmark && \
REUSE=1 \
TABLE=hackernews_openai_pgvector_1m \
RATIOS=1,5,10,20,30,40,50,60,70,80,90,95,99 \
QUERIES=100 K=10 CLIENTS=8 JOBS=8 DURATION=60 EF_SEARCH=128 \
RESULT_CSV=/home/vector-benchmark/benchmark/results/pgvector_hn_in_traversal_compare_reuse_$(date +%Y%m%d_%H%M%S).csv \
/home/vector-benchmark/benchmark/bash-test/pgvector-hackernews-in-traversal-compare.sh
```

If `RATIOS`, `ROUTES`, `QUERIES`, `K`, or generated SQL files change, run once without `REUSE=1` so the truth tables and pgbench SQL are regenerated.

## Verified Locally

Commands run:

```bash
make PG_CONFIG=/usr/local/pgsql/bin/pg_config
make PG_CONFIG=/usr/local/pgsql/bin/pg_config install
BUILD_INSTALL=0 /home/pgvector/scripts/test_latest_in_traversal_filter.sh
```

Hacker News smoke output:

```text
route=in_traversal_hnsw recall_id=1 recall_distance=1 underfilled=0
route=in_traversal_exact recall_id=1 recall_distance=1 underfilled=0
```
