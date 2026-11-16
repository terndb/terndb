# The engine behind a server session

`sh/sysl/terndb/engine.sysl` joins the session layer to the database. An **`Engine`** is one in-memory database —
the MVCC `Store` (`store.md`), which holds the `MemCatalog` — shared by every connection of a server. An
**`EngineConn`** is one connection of a role, and is the `Executor` a `ServerSession` drives
(`session_executor.sysl`). Its tests (`engine_tests.sysl`, `engine_txn_tests.sysl`) drive a real client and a
real server session joined by real frames, so every statement crosses the wire as an encoded envelope and
parameter block and every result comes back as `Batch` payloads.

## A statement's life

- **`start`** validates: the envelope is decoded (`read_envelope`), checked structurally (`check_errors`) and
  type-checked against the catalog (`type_errors`, `type_envelope`), and a validation error carries every error
  of the first phase that has one (`wire-protocol.md` §8.1 rule 3). The catalog is the one the statement's
  transaction sees (§ Transactional DDL): an explicit transaction's view, at read committed at the statement
  snapshot taken first, or the newest committed catalog for an implicit one. A DDL statement is validated by
  applying it to a copy of that catalog. The parameter block is decoded by the declared types, resolved in the
  catalog. Then the transaction: the explicit one named, or a new implicit one with the invocation's options;
  the statement's catalog read is recorded in it (`ddl.md` §6.2). The `ResultHeader` reports the snapshot.
- **`next`** runs one parameter tuple at a time, its transaction's view installed first — a query or DML
  statement through `run_statement`, a DDL statement through `Store.ddl` — and hands out that tuple's batches
  one per call. Batches are passed
  through as the executor made them unless the request asked for `batch_rows` or `batch_bytes`, or the tuple is
  not the first, in which case they are decoded and encoded again (§6.4 rules 5–6).
- **`finish`** commits an implicit transaction; `cancel` rolls it back, or aborts an explicit one with
  `cancelled`.

Any failure in a transaction aborts it in the store with its code (`isolation.md` §5.1): validation and binding
errors in `start`, runtime, constraint and isolation errors from the store in `next`, a commit's validation in
`finish` or `commit`. The session decides `txn_state` and `retryable`.

## Row-lock waits

The store is single-threaded and answers `Wait(holder)` rather than blocking, having written nothing. The
statement is then **parked**: `next` answers `NotYet` while `holder` is live, and runs the statement again
from its start once it is not. The session asks again at every arrival and every tick, which is
`wire-protocol.md` §7.5. A cycle is refused by the store with `deadlock_detected` before anything parks. A DDL
statement parks the same way on another transaction's catalog lock (`ddl.md` §6.1). A holder that rolls back to
a savepoint lets the locks of the span go while it stays live (`wire-protocol.md` §5.5 rule 5), so a parked
statement also runs again whenever the store's count of such rollbacks (`Store.released`) has moved since it
parked.

The engine is made with a `Clock` (`clock.sysl`: `now_ms`, milliseconds from an origin of its own), which a
server gives as `MonotonicClock`, over `sysl.posix.time.monotonic`; the tests give a `ManualClock` they set by
hand. A statement notes the reading when it first parks, and every `next` that finds it still parked
compares the time since with its transaction's `lock_timeout_ms` — `Begin`'s for an explicit transaction,
the statement's implicit options otherwise, both already filled from the connection's defaults. A wait that
long fails with `lock_timeout` and aborts the transaction through `Store.abort_with`, which a retryable code
rolls back whole (`wire-protocol.md` §7.5 rule 5). The wait goes on across runs again until a run gets past
the lock, and a cycle is still refused before anything parks, so deadlock detection comes first. Tests:
`engine_lock_timeout_tests.sysl`.

## Savepoints

`savepoint`, `rollback_to` and `release` are the store's (`store.md` § Savepoints); the session keeps the names
and refuses an unknown one with `W2012` before the engine sees it, and a failure the store answers aborts the
transaction like any error. Tests: `engine_savepoint_tests.sysl`.

## Transactional DDL

The catalog is versioned under MVCC in the store (`store.md` § The versioned catalog), so DDL obeys its
transaction (`ddl.md` §9): it is visible to its own transaction at once and to others from its commit, and a
rollback — asked for, or by any error — takes it back with the rows written under it. The store's `catalog` is
the catalog of the operation in progress, so the engine installs the right one before every use: `Store.enter(t)`
for a statement of `t`, `enter_latest` for `Prepare` and for a statement that will begin an implicit
transaction. `catalog_version` (in `Ready` and `Prepared`) is the newest committed catalog's. Tests:
`engine_ddl_txn_tests.sysl`.

The **plan cache** keys an entry by the version of the catalog it was validated against; no two catalogs share a
version, a transaction's own view included. A lookup by digest (`cached(shape, txid)`) installs the catalog
`txid` sees and validates an entry of another version again, dropping it if it fails or its result type
changed, so a statement cached under one transaction's uncommitted table is `W2004` to everyone else.

## What it does not do yet

- **Reconciliation** (`ddl.md` §6.4) covers added unique keys only. A writer that commits after a DDL it did not
  see which added a check, not-null or referencing foreign-key constraint, a column whose default is not its fill
  (or a generated or filling column), or a foreign key referencing a table it removed or changed rows of, aborts
  with `schema_changed` instead of being reconciled, since the store cannot evaluate expressions.
- **The `convert_column` job** (`ddl.md` §8.5 rule 2) does not run: a retype that is not metadata-only leaves the
  column `converting` in its old type, so every reader sees the old type and no stored row needs a `using`
  expression applied on read. Translation (`store.md`) applies the lossless conversions only.
- **The role** is fixed when the connection is made (`Engine.connect`); the `Executor` trait carries no
  authenticated user. DDL is applied, and the plan cache keyed, as that role.
- **`Authority`** is not backed by the catalog's roles; a server supplies its own.
- Error spans: the engine reports paths, not `span`s, since nothing maps a path to a node's pre-order index yet.
