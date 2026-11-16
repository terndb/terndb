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
  of the first phase that has one (`wire-protocol.md` §8.1 rule 3). A DDL statement is validated by applying it
  to a copy of the catalog. The parameter block is decoded by the declared types, resolved in the catalog.
  Then the transaction: the explicit one named, or a new implicit one with the invocation's options. At read
  committed the statement snapshot is taken here, and the `ResultHeader` reports it.
- **`next`** runs one parameter tuple at a time — a query or DML statement through `run_statement`, a DDL
  statement through `MemCatalog.apply` — and hands out that tuple's batches one per call. Batches are passed
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
`wire-protocol.md` §7.5. A cycle is refused by the store with `deadlock_detected` before anything parks.

## What it does not do yet

- **DDL is not transactional.** The catalog is not versioned under MVCC (`store.md` § Deferred), so a DDL
  statement changes the catalog at once, whatever transaction it runs in, and a rollback does not undo it.
  A dropped table's rows go with it.
- **Savepoints** are refused with `unsupported_request` (`W2018`), since the store keeps none.
- **`lock_timeout_ms`** is not applied: the `Executor` has no clock. A parked statement is bounded by
  `statement_timeout_ms` and by `Cancel`.
- **The role** is fixed when the connection is made (`Engine.connect`); the `Executor` trait carries no
  authenticated user. DDL is applied, and the plan cache keyed, as that role.
- **`Authority`** is not backed by the catalog's roles; a server supplies its own.
- Error spans: the engine reports paths, not `span`s, since nothing maps a path to a node's pre-order index yet.
