# The isolation model and the lock-step comparison

`sh/sysl/terndb/iso_model.sysl` is the executable model of `isolation.md` §7.4: §4's mechanism written as
plainly as the spec states it, so that it can be checked by reading it beside §4. `iso_lockstep.sysl` makes it a
target of the history driver (`history.md`) and runs it beside the in-memory store (`store.md`) in lock step.
Their tests are `iso_model_tests.sysl` (each rule of §4.1–§4.5 and §5, one test each) and `iso_lockstep_tests.sysl`
(the lock-step sweeps, the fault-injected stores diverging, and the checker over the model's own histories).

## The model

- **Rows** are a table id, an integer key and an image: a `Buf[MoVal]` of columns, column 0 the key, each an
  integer or a list of integers (a nested relation, one value of its row, §4.5 N1–N2). There is no catalog, no
  schema version, no index and no executor.
- **`MoObject`** is one row: its write lock and its versions oldest first, each `MoVersion` its writer, its
  commit number (0 while uncommitted) and its image (`None` for a delete). Rows are kept sorted by table and
  key, so a scan answers in key order as the store's does.
- **`MoTxn`**: id, level, read-only flag, snapshot, state, predicate log, writes (one `MoWrite` per row:
  `before` from the first write, `after` from the last, §4.3) and the transaction it waits for.
- **Predicates are data** (`MoFilter`): a point lookup `key = k`, a key set, a key range `[lo, hi)`, a column
  equal to a value, a list column holding an element (a predicate over a nested level, §4.5 N3), or every row.
  A log entry (`MoEntry`) is a table, a filter, the columns `C` the access used, or `every`. A write
  statement's change is data too (`MoChange`: set, add, append, remove), and the columns it reads join `C`.

Each operation is one statement, so read committed takes its snapshot at the start of every operation (§3.3).
In the order of §4:

- `begin` takes `s = h`. Reads follow the version chain to the reader's own version or the newest committed at or
  before `s`. A serializable transaction not declared read-only logs every read (§4.3, §5.5): a point lookup as
  `key = k` of every column, found or not; a scan as its filter and columns (§4.4).
- `update` selects the rows it sees satisfying its filter; if one is locked by another live transaction it waits
  before writing anything (rule 1), and a wait that would close a cycle aborts the waiter with
  `deadlock_detected`. It logs its filter with the columns the filter and the change read, then writes each row:
  over its own version in place; at read committed over the newest committed version if the filter still holds
  of it (rule 3); otherwise under rule 2, which aborts at snapshot and serializable when the newest committed
  version is newer than `s`.
- `insert` (rule 4): waits for a live writer of the key; a key it sees is `unique_violation`; a key live in the
  newest committed version but not seen was committed after `s`, `serialization_failure` (at read committed
  `unique_violation`); a delete committed after `s` falls to rule 2.
- `commit`: an empty write set commits at `s` (§5.5). Otherwise `c = h + 1`; a serializable transaction is
  validated by §4.3's procedure verbatim over every retained commit with `s < c_j < c`; then its versions are
  stamped, its locks released and `h = c`, publication before the answer (§4.1).
- Any error aborts the transaction and undoes its writes (§5.1); later operations answer
  `transaction_aborted`, its commit the code that aborted it, and a committed one `unknown_transaction`.

The model keeps every committed write set (no retention pruning) and never reclaims a version: being obviously
right is its job, not being fast.

## As a driver target

`HTarget` (`history.sysl`) is what the driver runs against: seed the rows, draw a fresh value, begin, run one
statement (`StepEnd`: ran with its micro-operation, wait for a holder, or failed with a code), commit, abort,
whether a transaction is live, and `trouble`, which stops the run. `Bench` (the store), `ModelBench` (the model)
and `LockStep` implement it. `ModelBench` runs each `HStep` as `exec` runs it on the store, with the same
predicates and the same columns read: an append, a write or an add is an update of the row keyed `k`; a sum a
scan of the key set over `k` and `v`; W4's read a scan of `g = n` over `g`; a delete an update by key. A run's
`Names` gives each name its row key on first use and is shared by the targets in lock step, so both order rows
alike. `run_workload_on` runs a workload on any target.

## Lock step

`LockStep` holds a `Bench` and a `ModelBench` and sends every call to both, the store first, comparing:
the snapshot at begin, every statement's `StepEnd` (so every value read, every predicate read's rows, `Wait` or
proceed, and which code a failure carries), every commit's sequence number or abort code, and whether a holder is
live. Transactions are named by the store's ids; the model's are mapped, so a `Wait` names the same holder. The
first divergence is kept with the step number (each begin, statement and commit is one), the seed, the
transaction, the statement and both answers, and the driver stops there; `lockstep_divergence` answers it. The
seed reproduces the run exactly.

Sweeps run every workload the driver has (W1, W2, W3, W3b, W4, W4 on-call, W6) over `LockstepSeeds` seeds at
each of the four level mixes with lost acknowledgements on. Each store fault diverges on some seed, at a named
step, where the real store does not: `skip_validation` (W3b and W4 at serializable, as a commit the model
refuses), `skip_first_updater_wins` (W1 at snapshot, W2 at serializable), `si_statement_snapshot` (W1 at
snapshot, as a read) and `stale_snapshot` (every mix, at the first begin's snapshot).

The model's own histories go through `check_history` with each workload's options, white box included, and must
be clean at every mix; the anomalies snapshot and read committed permit (write skew, a phantom, the read-only
anomaly, read skew) must occur in them, so the check is not vacuous.

## Not yet

- §7.4 asks for the model to run inside the deterministic simulator (§7.3), whose scheduler explores the
  interleavings exhaustively for 3 transactions, 2 objects and 2 predicates; that waits on the simulator.
  Seeded schedules stand in.
- `synchronous`, crash recovery to a WAL prefix and a replica applying a durable prefix are not modelled: the
  store has none of them yet.
- The nested-level predicate (`MoHas`) is covered by the model's own tests only: the driver has no workload over
  a nested level (W5, `history.md`).
- Savepoints, `lock_timeout`, `LOCK_ONLY` locks, secondary unique keys, foreign keys and DDL are the store's and
  outside the model.
