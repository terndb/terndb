# The in-memory store

`sh/sysl/terndb/store.sysl` is the table store of the in-memory mode and its multi-version concurrency control:
`isolation.md` §4.1–§4.5 and the abort rules and codes of §5. It is a library; nothing in the executor calls it
yet. Its tests are `store_tests.sysl` (the mechanism and every code) and `store_anomaly_tests.sysl` (the §7.2
workloads at each level, and each forbidden anomaly again on a fault-injected store).

## Structure

- **`Store`** holds the `MemCatalog` its tables come from, the transaction-id counter, the visible horizon `h`
  (the last commit sequence number), one `TableData` per table that has been touched, the live transactions and
  the retained committed write sets.
- **`TableData`** keeps the table's rows as a `Buf[&Slot]` sorted by primary key (a record of the key columns,
  compared with `cmp`), found by binary search. sysl has no fast hash map yet (`docs/phase0/sysl-survey/`), so
  the ordered buffer stands in; an insert is O(n) in the number of keys. A slot is never removed: a delete is a
  tombstone version and an aborted insert leaves an empty chain, both reclaimed by a purge that does not exist yet.
- **`Slot`** is one row: its key, its row lock (the id of the live transaction owning the newest version, 0 for
  none) and its versions, oldest first. The newest is the in-place version, the rest the undo chain (§4.1).
- **`RowVersion`** carries its writer's id, its commit sequence number (0 while uncommitted) and its image, a
  `StoredRow` (schema version plus the `Value` record of that version's columns) or `None` for a delete. Every
  stored row carries its schema version (`ddl.md` §5.2).

## Version chains and visibility

A reader walks a slot's versions newest first and takes its own uncommitted version, else the first version
committed at or before its snapshot (§4.1). Commit stamps the transaction's top version of every row it wrote
with `c = h + 1`, releases the locks and publishes by setting `h = c`; publication is therefore in `c` order and
precedes the return of `commit`, which is the acknowledgement. Abort pops each written row's top version, so an
aborted version was never visible to anyone else.

A write (§4.2) to a row locked by another live transaction answers `Wait(holder)`. A write statement first
collects the rows its predicate selects at its snapshot, and answers `Wait` before writing any of them if one is
locked, so a waited statement leaves no trace and is simply called again once the holder has ended. The
wait-for graph is each live transaction's single `waits_for` edge; a wait that would close a cycle aborts the
waiter with `deadlock_detected`. At snapshot and serializable a write over a newest committed version newer than
the snapshot aborts with `serialization_failure` (rule 2); at read committed the statement re-reads the newest
committed version, re-evaluates its predicate on it and applies its change to it (rule 3). A write in a schema
version older than the newest committed image's aborts with `schema_changed` (`ddl.md` §5.4 rule 3).

## The predicate log and validation

A serializable transaction not declared read-only logs every read: a `LogEntry` of the table, the read's `Pred`
and, for a point lookup, the key. A `Pred` is a closure over a `StoredRow` (the membership predicate `L`, which may
read nested levels, §4.5 N3) and the column ids the read used (`C`), or `every`. A write statement logs its
predicate too; its `C` must name the columns its change reads.

Each transaction keeps one `Write` per row it wrote: `before` the newest committed image its first write replaced,
`after` the image its last write left. At commit a serializable transaction with writes is validated against the
write sets of the transactions that committed after its snapshot, exactly as §4.3's procedure: a write aborts it
when it changes visibility or a column of `C` and either image matches `L` (for a point entry, when the image is of
that key). Committed write sets are retained while some live serializable transaction has a snapshot below their
`c`, and dropped as soon as none has. A transaction with an empty write set skips validation and answers its
snapshot as its serialization point (§5.5).

Any error inside a transaction aborts it with that error's code (§5.1); later operations answer
`transaction_aborted` (`W2003`), and its `commit` answers the code that aborted it. Operations on a committed
transaction answer `unknown_transaction` (`W2001`), the handle being closed.

`Faults` switches validation or first-updater-wins off, the fault-injected builds §7.1 requires so that each
anomaly check is shown able to fail; a real store has both on.

## Concurrency

The store is **single-threaded**: transactions interleave in the order their operations are called, which is
how the tests drive deterministic interleavings, and a wait is the `Wait` answer rather than a blocked thread.
Readers take no latch because nothing runs beside them. Making it shared across threads (a `&sync` store with
per-row latches and a real wait) is the next slice of this work.

## Deferred

- Disk, WAL, the buffer manager and recovery (waiting on sysl's CRC32C and io_uring gaps); `transaction_status`.
- `lock_timeout` (no clock seam yet), savepoints, `max_predicate_log` coarsening, `max_snapshot_age` and undo
  purge.
- Translation of a row of an older schema version into the reader's (`ddl.md` §5.3): the store returns each image
  in its own version, and the executor will translate it. Catalog versioning under MVCC and `schema_changed` from
  catalog validation (§5.6).
- Secondary indexes, unique constraints other than the primary key, and constraint checks.
- Term bits in transaction ids and commit numbers (`replication.md` §1.4).
