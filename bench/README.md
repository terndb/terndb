# TernDB's release benchmark

Every TernDB release is compared with PostgreSQL and DuckDB on every feature it has, and the raw results are
kept under `results/<release>/` so the next release can be diffed against them. The report for a release is
`REPORT-<release>.md`.

## What is here

| Path | What it is |
|---|---|
| `main.sysl` | The `bench` program: `verify`, `time`, `concurrent`, `client`, `report`, `diff` |
| `sh/sysl/terndb_bench/workloads.sysl` | Every workload, with its SQL for each engine |
| `sh/sysl/terndb_bench/trees.sysl` | The query trees TernDB runs where its 0.0.1 SQL cannot spell the query |
| `sh/sysl/terndb_bench/data.sysl` | The data generator: every value a hash of (table, key, column) |
| `sh/sysl/terndb_bench/tern.sysl`, `duck.sysl`, `pg.sysl`, `pq/` | One driver per engine |
| `sh/sysl/terndb_bench/runner.sysl` | Loading, the interleaved timing loop, the answer check |
| `sh/sysl/terndb_bench/cell.sysl`, `stats.sysl`, `results.sysl`, `diff.sysl`, `report.sysl` | The harness's own logic, unit-tested in `harness_tests.sysl` |
| `postgres.conf` | PostgreSQL's settings, included from a fresh data directory's `postgresql.conf` |
| `build.zsh`, `run.zsh` | Build the binaries; run one job |
| `results/<release>/` | Raw JSON, one file per pair and scale, kept for every release |

## Running it

PostgreSQL 16 runs as the session's own user from its own data directory, never as a Homebrew service:

```
initdb -D <data dir> -U $USER --auth=trust --encoding=UTF8 --locale=C
echo "include '<repo>/bench/postgres.conf'" >> <data dir>/postgresql.conf
pg_ctl -D <data dir> -l <log> start
```

`postgres.conf` puts the socket in `/Users/ed/.claude/tmp/tdbb`: a Unix socket's path must fit in 104 octets
on macOS. Then, each through marshal (timing jobs with `--exclusive --quiet-load auto`):

```
zsh bench/build.zsh <repo> <bin dir>
zsh bench/run.zsh <bin dir> verify S <out>/verify-S.json
zsh bench/run.zsh <bin dir> inproc S <out>/inproc-S.json
zsh bench/run.zsh <bin dir> server S <out>/server-S.json
zsh bench/run.zsh <bin dir> concurrent S <out>/concurrent-S.json
<bin dir>/bench report <out>
<bin dir>/bench diff results/<previous>/inproc-S.json results/<this>/inproc-S.json
```

`run.zsh` makes a fresh PostgreSQL database `bench` for each job and starts and stops the TernDB server the job
needs. A `--only a,b` style workload list may follow the output path. `bench diff` exits 3 when anything
regressed.

## The rules it keeps

- Every workload's answer is checked across all engines (`verify`), and a time whose answer differs is
  reported as a wrong answer, never as a number.
- Engines are timed in rounds, one run each per round, so the machine's drift falls on all of them alike.
- Each engine is set up at its own best; the report states every setting and why.
- The harness's logic is unit-tested: `sysl test bench --include-path duckdb=/opt/homebrew/include --link-path /opt/homebrew/lib`.
