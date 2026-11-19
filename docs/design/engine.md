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

## Roles and privileges

A connection runs as the role it authenticated as: the server session calls `Executor.authenticated(user)` once
the handshake succeeds, before `Ready`, and `EngineConn` takes that user as its role, for DDL, for the privilege
checks and for its plan-cache key. The user directory is `CatalogAuthority` (`engine_authority.sysl`): it wraps
the server's own credentials and answers a verifier, a token's user or a peer login only for a role the newest
committed catalog holds with `login`, so any other user fails as an unknown one does (`W3001`); which methods are
permitted is left to the credentials, so it says nothing about which roles exist (`ddl.md` §2.10 rule 1).

Privileges are checked at validation (`statement_privileges`, `ddl.md` §2.10 rules 4 and 8) against the catalog
the statement's transaction sees, so a `Grant` or `Revoke` is seen by its own transaction at once, by others
from its commit, and not at all once rolled back; at serializable a revoke committed during a transaction is
not seen by it, at read committed it is seen by the next statement, and it never aborts a transaction by
itself. Tests: `engine_privileges_tests.sysl`.

## The server

`sh/sysl/terndb/server.sysl` is the `Server`: one `Engine` shared by every connection and one `ServerSession`
per connection, on **one thread** with non-blocking sockets and one `sysl.io.poll` poller, because the store is
single-threaded. Each turn of the loop (`Server.turn`) waits for readiness, reads what arrived on each
connection into its reassembler and hands the arrivals to its session, then **ticks every session** at the
engine clock's reading, and writes what the sessions answered; output a socket will not take yet waits for
writability. The tick is what resumes a parked statement once its holder, on another connection, has ended,
and what ends one whose `lock_timeout_ms` is spent, so a parked statement is served no later than the next
turn (`wire-protocol.md` §7.5 rules 2 and 5). `Server.run` turns every 10 ms while any request is in flight and
every second otherwise. The engine's statement time (`Engine.now`) is set from the wall clock each turn. A
connection whose transport ends has its session `lost`, which rolls back its open transactions (§5.1 rule 3)
and releases their locks; a session that asks to close is closed once its output is written. On SIGINT or
SIGTERM every session gets `Goodbye` with a five-second grace (§3.4 rule 5) and the socket file is removed.

**Transports.** The Unix-domain socket serves the protocol in full (§1.1 rule 4). TCP must be direct TLS 1.3
(§1.1 rule 2), which the server does not have yet, so a TCP listener only obeys §1.1 rule 3: a first octet of
`T` is answered with `W3003` on stream 0, and any other first octet, a TLS record included, is closed without
a response. The listener is opened only when asked for, on 127.0.0.1.

**Credentials.** The server's `Authority` (`ServerCredentials`, narrowed by `CatalogAuthority`) holds one SCRAM
verifier, the bootstrap role's (`terndb`), made at start from the configured password with a salt from the
operating system's generator and 4,096 iterations; every other role fails as an unknown user does, since no
verifier is stored in the catalog. Each connection's SCRAM server nonce is 18 random octets in hex. No token
is accepted, since nothing issues one. A `peer` login needs the peer's operating-system user from the socket,
which `sysl.net` does not report yet, so `peer` fails (`W3001`).

**Configuration** is the command line, since the specs name no configuration file:

```
TERNDB_PASSWORD=… terndb serve [--socket <path>] [--port <n>] [--database <name>] [--password-file <path>]
```

`--socket` defaults to `terndb.sock` in the working directory (a file left there by an earlier run is
replaced); `--database` is the one database's name, default `terndb`; the password comes from
`--password-file` (its trailing whitespace dropped) or else `TERNDB_PASSWORD`, and there is no default; `--port`
opens the TCP listener described above. The connection limits it announces are `max_frame_size` 1 MiB,
`max_in_flight` 64, `max_open_txns` 16 and `max_message_size` 16 MiB; wire 1.0 and the tree version of
`qtree.sysl`; no compression; a 10-second handshake deadline and no idle limit.

The program is `terndb serve`, in `cli/` (its own `sysl.hocon`, depending on the package by path), built with
`sysl build cli`; the same binary is the shell (`docs/design/shell.md`). Tests: `server_tests.sysl`, which run wire clients over a real Unix socket and a real TCP
connection against a server in the same process, turning its loop by hand on a `ManualClock`.

## What it does not do yet

- **TLS and `peer`** (§ The server): TCP refuses every connection, and a `peer` login fails, until TLS 1.3 with
  `tls-exporter` and a socket's peer credentials are reachable from sysl.

- **Reconciliation** (`ddl.md` §6.4) covers added unique keys only. A writer that commits after a DDL it did not
  see which added a check, not-null or referencing foreign-key constraint, a column whose default is not its fill
  (or a generated or filling column), or a foreign key referencing a table it removed or changed rows of, aborts
  with `schema_changed` instead of being reconciled. An added unique key is reconciled whatever its kind: over
  columns, partial, or over expressions, which the store evaluates as definition expressions (`store.md`).
- **Jobs other than `build_index`** do not run (`store.md` § Jobs): a check or foreign-key constraint stays not
  validated, and `AwaitJobs` returns no relation yet, although the store records how each build ended.
- **The `convert_column` job** (`ddl.md` §8.5 rule 2) does not run: a retype that is not metadata-only leaves the
  column `converting` in its old type, so every reader sees the old type and no stored row needs a `using`
  expression applied on read. Translation (`store.md`) applies the lossless conversions only.
- **Credentials** are the server's own `Authority`: `CatalogAuthority` narrows it to the roles the newest committed
  catalog holds with `login`, but no SCRAM verifier or token hash is stored in the database, and nothing issues a
  token yet.
- **Runtime errors name a node only where the tree the client sent was planned**: an `R…` error names the
  innermost expression whose evaluation raised it (`wire-protocol.md` §8.1 rule 2); a frame, `lag`/`lead` or
  `Limit` offset error names the offset expression; an aggregate or window call's own error (a `sum` that
  overflows) names its `Aggregate` or `Window`, since a call is no node. Each compiled expression keeps its
  pre-order index from planning, the error records it, and the engine turns it into a path (`node_paths`) only
  when the statement fails; the span then follows as for validation errors (§8.1 rule 4). An error names no
  node, and so carries no span, when it is raised in a tree the client did not send: a query that unnesting
  rewrote (any decorrelated subquery), or a view's stored query. Operator errors with no expression to blame
  (a single join's second match, a recursive CTE that does not end, a DML write of one row twice) name none.
