# The in-memory store

`sh/sysl/terndb/store.sysl` is the table store of the in-memory mode and its multi-version concurrency control:
`isolation.md` §4.1–§4.5 and the abort rules and codes of §5. The executor runs statements against it
(`executor.md` §4.5) through `scan_range`, `get` and `write_rows`. Its tests are `store_tests.sysl` (the mechanism and every code) and `store_anomaly_tests.sysl` (the §7.2
workloads at each level, and each forbidden anomaly again on a fault-injected store); the unique keys and
foreign keys of `store_index.sysl` are tested in `store_constraint_tests.sysl` and, through DML, in
`exec_dml_constraint_tests.sysl`. `Faults` holds the fault-injected builds of §7.1 (validation or first-updater-wins
skipped, a snapshot taken per statement at snapshot level, a snapshot one commit behind the horizon), all off
in a real store; the history checker of `history.md` is proven against each.

## Structure

The rows live in pages exactly as `on-disk-format.md` §8.1 has in-memory mode keep them: one B+tree per table over
16 KB pages (`btree.md`), each row in the row format (§4.1–§4.2, `row-format.md`), and every older version in the
undo log (§5, `undo.md`). Checksums are not computed in memory (§8.1 rule 3). `check_pages` runs the B+tree check
and the undo check over everything the store holds; `store_pages_tests.sysl` exercises the page-backed paths.

- **`Store`** holds the `MemCatalog` of the operation in progress and the catalog's committed versions (below),
  the transaction-id counter, the visible horizon `h` (the last commit sequence number), one `TableData` per
  table that has been touched, **`rows`** (an `UndoRows`: the data space with every table's tree, the undo space,
  and the transaction table), the live transactions and the retained committed write sets. A transaction's id is
  its id in `rows`, and `h` is always `rows.last_commit`.
- **`TableData`** is a table's place in the store: its primary key's columns, their record type and their §4.6
  key encoding (each component ascending), the **`SchemaCodec`** of each schema version a row has been written in,
  and its unique keys. A codec holds the version's columns, where the key columns sit among them, and each
  non-key column's type with named types resolved as they stood at the version's first write; it is declared to
  `rows` as the version's `RowLayout`, so the leaf can lay out a body. A codec is made again at a write when the
  catalog's columns for its number differ, which happens only when a rolled-back `ALTER` gave the number out.
- **A row** is the tree entry (encoded key, row). `Slot` is a row as read now: its key, the key's bytes and its
  current image (`RowImage`: header and non-key column images). A `StoredRow` (schema version plus the `Value`
  record of that version's columns, key included) becomes column images with `encode_value` and comes back with
  `decode_value`, the key's values taken from the key. `RowVersion` (writer, commit number, image) is what the
  checks below read from a row's version chain.
- **Limits.** A key over 1,024 bytes is `key_too_long` (`D2001`). A row whose key and value exceed 4,000 bytes
  would need columns out of line in overflow chains (§4.7–§4.8), which wait on sysl 0.12 (CRC32C), so it is
  refused with `entry_too_large` (`D2002`) before any undo record is written; nothing is approximated.
- **`UniqueIx`** (`store_index.sysl`) is one secondary unique key of a table — a `unique` constraint, or a unique
  index `building` or `ready`, partial or over expressions (`enforced_keys`): a `Buf[IxEntry]` sorted by the key's
  value (then by primary key), each entry a value with no null component that some version of a row holds, and the
  row's primary key. A value with a null component, or a version the predicate is not true of, never conflicts and
  has no entry (Q48; on disk such an entry takes the non-unique form, §4.9 rule 1). An entry is added whenever a
  version is installed and never removed; a lookup fetches the row and reads its versions to decide whether the
  value is still there, as §4.9 rule 2 has a reader do. A plain key (columns only, no predicate) reads a version's
  entry from its image; any other evaluates its predicate and expression components over the version as a row of
  the current schema version (`ix_values`), the versions installed since the last check together. `sync_uniques`
  brings a table's indexes in line with the catalog before each check, building a key added since from every
  version of every row still reachable, so a `building` index holds every row's entry from its creation (Q71).
- **Jobs** (`store_jobs.sysl`, `ddl.md` §7): the commit of a transaction that ran DDL runs the `build_index` jobs it
  started before it returns — one per index left `building` that backs no constraint, and one per `unique`
  constraint whose validation is due — each in an internal read committed transaction at the newest snapshot that
  checks no two visible rows share a value and commits the index `ready` (and the constraint validated), or
  `failed`: a failed index is dropped, a failed constraint's index kept with the constraint not validated. Each
  job's `JobRecord` (`Store.jobs_of`) holds how it ended.

## Version chains and visibility

A row's current image names its writer in `txn`; its older versions are rebuilt by applying the undo records of
its version chain backward (§5.4). A reader takes its own uncommitted version, else the newest committed at or
before its snapshot. A write is one undo record and the row rewritten in its tree, through `UndoRows`: an INSERT
where there is no row, an UPDATE over a live row or a ghost, a DELETE that leaves a ghost. A transaction writing a
row again writes another record, so a statement or savepoint can take back just the later write. Commit changes
no row: it takes `c = h + 1` in the transaction table and publishes by setting `h = c`, so publication is in `c`
order and precedes the return of `commit`, which is the acknowledgement; the row locks, which are the rows' `txn`
fields, lapse with it. A transaction with an empty write set is recorded as committed at its snapshot and
publishes nothing. Abort walks the rollback chain and restores every row, so an aborted version was never visible
to anyone else. **Purge** runs whenever a transaction ends, up to the oldest snapshot a live transaction holds or
one beginning now would take: ghosts no snapshot can see leave their trees and undo pages below the horizon are
dropped (§5.5, §5.6). A table no catalog can read any more gives its tree's pages back.

A write (§4.2) to a row locked by another live transaction answers `Wait(holder)`. A write statement first
collects the rows its predicate selects at its snapshot, and answers `Wait` before writing any of them if one is
locked, so a waited statement leaves no trace and is simply called again once the holder has ended. The
wait-for graph is each live transaction's single `waits_for` edge; a wait that would close a cycle aborts the
waiter with `deadlock_detected`. At snapshot and serializable a write over a newest committed version newer than
the snapshot aborts with `serialization_failure` (rule 2); at read committed the statement re-reads the newest
committed version, re-evaluates its predicate on it and applies its change to it (rule 3). A write in a schema
version older than the newest committed image's aborts with `schema_changed` (`ddl.md` §5.4 rule 3).

## What the executor calls

- **`scan_range(t, table, lo, hi, logged)`** answers every row visible to `t` whose encoded key lies between `lo`
  and `hi`, and logs `logged` whatever the range, even an empty one; `scan_rows(t, table, logged)` is the whole
  range. The executor chooses the range from its filter (`executor.md` §4.6) and filters the rows itself,
  vectorised, so the predicate's closure runs only in validation. `slots_read` counts the leaf entries these
  scans visit, for the tests that an access path reads only its range.
- **`scan_vectors(t, table, lo, hi, logged, elem, columns)`** is the same read, logged the same way, answered
  as vectors of `elem` of at most `BatchSize` rows, decoding only the non-key columns `columns` names (all when
  none): what `ScanOp` calls (`executor.md` §4.5). `reads` (`ReadWork`) counts what the reads build and
  decode — leaf entries copied, images, `StoredRow`s, values, columns decoded into lanes, columns skipped —
  for the tests that pin a scan's cost per row.
- **`write_rows(t, table, changes)`** applies one statement's writes, each a `RowChange`: `Put` a new row (an
  insert, under §4.2 rule 4), `Replace` or `Remove` the row of a key the statement read. It answers `Wait`
  before writing any of them if one key is locked, so a waited statement leaves no trace. The executor orders
  removals first, so a key a row leaves is free for the row moving into it.
- **`usable(t)`** refuses a statement in an ended transaction, and **`abort_with(t, e)`** aborts `t` with the
  code of an error raised outside the store (validation, a runtime error, a check constraint).
- **`statement(t)`** begins a statement: a new snapshot at read committed, and a **`Mark`**: where `t`'s statement
  log (the rows it wrote, for the batch's unique checks and its write list), writes, `LOCK_ONLY` locks, deferred
  checks and undo rollback chain stand. **`undo_statement(t)`** takes the statement back when it must wait
  part-way (a cascade meeting a locked row, a foreign-key check meeting a locked referenced row): the undo records
  after the mark are rolled back (`UndoRows.rollback_to`), which restores the rows and lets their locks go, and
  the lists are cut back. `write_rows` uses the same mark to undo its own batch.
- **`lookup_unique(t, table, ids, value)`** finds the visible row holding `value` in a unique or primary key, for
  an upsert's conflict check (logged as a read of every column).
- **`check_reference(t, fk, value)`**, **`referencing(t, fk, values)`**, **`holds_key`** and **`defer_check`** are
  the foreign-key primitives (below); the actions themselves are the executor's (`exec_dml.sysl`, `fk_effects`),
  since they compute defaults and generated columns and check the rows they change.

## Unique keys and foreign keys

A batch of writes (`write_rows`, `insert`, `update_where`) is checked against the table's secondary unique keys
once every change is applied, so a statement may exchange values among its rows (`ddl.md` §2.6 rule 5). For each
row the batch wrote, every other row with an entry for its value is judged: a version another live transaction
is writing that holds it answers `Wait` (the batch is undone first); `t`'s own version or its snapshot's holding it
is `unique_violation`; the newest committed one holding it is `serialization_failure` at snapshot and serializable,
`unique_violation` at read committed. A row another transaction only holds `LOCK_ONLY` is judged by its committed
version.

A **`LOCK_ONLY` lock** is a LOCK undo record (§5.2, Q49): the row then carries the locker in `txn` with
`LOCK_ONLY` set, its columns untouched, and is listed in `Txn.locks`. Anyone else writing the row waits for it as
for any lock while the locker is live; at commit or rollback it lapses as every row lock does. It is never a
version: first-updater-wins compares the version's writer, found past every LOCK record (`UndoRows.writer_of`),
with the snapshot, so a lock committed after a writer's snapshot does not refuse it. A lock taken over a committed
lock sets record flag bit 2, and so does a write over a locked row. `owns(s, t)` — the row carries `t` as its
writer, not as a locker — is what decides a write over `t`'s own version.

`check_reference` takes the referenced row `LOCK_ONLY` at every level and logs the read at serializable
(`ddl.md` §2.6 rule 3); a referenced row another live transaction is writing answers its id to wait for.
`referencing` scans the referencing table, reading `t`'s own versions and otherwise the newest committed ones,
logged at serializable; at snapshot and serializable a match found only through a commit after the snapshot is
`serialization_failure`. A deferred check is a `Deferred` in `Txn.deferred`, run by `commit` before validation:
`ChildCheck` re-reads the referencing row as `t` sees it then and checks it (a referenced row locked by another
live transaction is `lock_timeout`, since a commit does not wait), `ParentCheck` fails if a removed referenced value
is still referenced and no row holds it again.

The executor runs a statement's foreign keys after its writes (`fk_effects`): for every changed row of every table
the statement or a cascade wrote, the referencing values it wrote are checked (or deferred), and the referenced
values it removed or changed run their key's action — `restrict` at once, `cascade`, `set_null` and
`set_default` as writes of the referencing rows (their generated columns recomputed and constraints checked) that
join the work list, so a cascade reaches any depth; `no_action` is checked once every action has run. The
referencing scan is one pass over the referencing table for all of a batch's values, binary-searched.

`update_where` and `delete_where`, which take the change as a closure, serve the store's own tests: a closure
cannot fail, and the executor's changes can.

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

## Savepoints

`store_savepoint.sysl` (`wire-protocol.md` §5.5); tests `store_savepoint_tests.sysl`, and through the wire
`engine_savepoint_tests.sysl`.

- **A savepoint** is a `Save` in `Txn.saves`: its name, the `Mark` of the undo log, writes, `LOCK_ONLY` locks and
  deferred checks when it was set, and how many DDL statements and catalog locks the transaction had. While one
  is set, `statement` keeps the undo log instead of clearing it, so the log reaches back to the oldest savepoint.
  The newest savepoint of a name answers to it.
- **`rollback_to(t, name)`** runs `undo_to` from the savepoint's mark — the versions installed after it are popped
  or restored (their row locks going with them), the writes, lock-only locks and deferred checks taken after it
  dropped — so a unique-key entry the span installed matches no version any more and lapses. Its catalog side
  drops the later DDL steps, makes its view again from the earlier ones with their oids (`replay`), and lets go
  of the catalog locks taken since. The predicate log and the catalog read are kept, and so are sequence draws.
  It drops the wait-for edges of the transactions waiting for `t` and bumps `Store.released`, so that a statement
  parked on a released lock runs again.
- **An error inside a span** (`fail_txn`, which `guard` and `abort_with` call) aborts `t` but, while a savepoint is
  set and the code is not retryable, keeps its versions, locks and DDL in place: `t` stays among the live
  transactions, holding its locks, until `rollback_to` makes it live again or `abort`/`commit` rolls it back
  whole. A retryable code rolls it back whole at once, past every savepoint (`isolation.md` §5.2), and a later
  `rollback_to` answers `W2003`.

`Faults` switches validation or first-updater-wins off, the fault-injected builds §7.1 requires so that each
anomaly check is shown able to fail; a real store has both on.

## The versioned catalog

`catalog_mvcc.sysl` versions the catalog under MVCC (`isolation.md` §5.6, `ddl.md` §6 and §9); its tests are
`catalog_mvcc_tests.sysl`.

- **Versions.** `CatalogVersions` keeps every committed catalog a live transaction can still see, each stamped
  with the commit that made it, oldest first. A transaction's view is the newest committed at or before its
  snapshot with its own DDL applied; `enter(t)` installs it as `Store.catalog`, the catalog every operation
  reads, and a commit installs the newest committed one. A catalog's `version` and every oid come from counters
  kept beside the versions, so no two catalogs share a version and no two transactions hand out one oid.
- **DDL** (`ddl(t, env, role)`) applies the statement to a copy of `t`'s view and diffs the two
  (`changed_keys`): every object created, changed or dropped (by oid; an index or constraint change is its
  table's, a foreign key both tables'), and every name taken or given up. Each key is a catalog row lock: one
  another live transaction holds answers `Wait` (deadlocks found through the same wait-for graph as row locks);
  at snapshot and serializable a key changed by a commit after the snapshot is `serialization_failure`; at read
  committed the statement runs against the newest committed catalog. The statement is kept with the oids it
  handed out, so that it can be applied again to a newer committed catalog — at read committed for each later
  statement's view, and at commit when other DDL committed meanwhile (`ddl.md` §9 rule 8).
- **Catalog reads.** Every statement records, in its transaction, the parts of the definitions it read
  (`ReadPart`, `ddl.md` §6.2) as the committed catalog under its view held them: each table's identity, name and
  key, each column it resolved, a whole column list where a scan's rows reach the result or a binder is read by
  `Row`, the defaults an insert relies on, views (whose queries are read too), named types and sequences. At
  commit, a writer whose read the newest committed catalog contradicts (`conflicts`) aborts with
  `schema_changed`; a transaction with an empty write set does not. At read committed, a statement naming a
  table an earlier statement's read of which has changed is `schema_changed` before it plans.
- **Reconciliation** (`ddl.md` §6.4) re-checks the rows a writer wrote against unique keys added since; what it
  cannot do it refuses with `schema_changed` (`engine.md`).
- **Translation** (`ddl.md` §5.3). `scan` (whose predicate reads the stored image), `scan_rows`, `get` and `lookup_unique` answer each row in the current schema
  version of the installed catalog: columns matched by id, one added since reading its fill (a literal, else
  null), one dropped since skipped, and one present converted through each retype between, in order
  (`translate.sysl`; tests `translate_tests.sysl`). A scalar step is the strict cast of `exec_functions.sysl`; a
  record, union or relation converts by position, a record value short of its named type's fields reading the
  fills the type keeps (`CatType.fills`). A value that does not convert is `D1001`.
- **Ending.** Commit publishes the transaction's merged catalog at its commit number and records each key it
  locked as changed there; any end releases its keys. A version older than the one the oldest live transaction
  sees is dropped, and with it the rows of every table no retained catalog, no live view and the installed
  catalog holds — a dropped table once nobody can read it, or a table whose creation rolled back.
- **Sequences** are not transactional (`ddl.md` §2.7 rule 5): where a sequence draws from (its next value and
  whether it is exhausted) is kept beside the versions (`CatalogVersions.draws`) and carried into each catalog as
  it is installed, so a draw under one version is seen under every other. A draw (`Store.drawing`) takes the
  newest committed definition at that shared position, so an uncommitted `AlterSequence` moves no draw, its own
  transaction's included (rule 8); at commit the transaction's DDL is applied again over the shared position and
  each committed sequence it altered takes the result. A sequence a transaction created is its own until commit,
  and its draws and alters act at once.

## Concurrency

The store is **single-threaded**: transactions interleave in the order their operations are called, which is
how the tests drive deterministic interleavings, and a wait is the `Wait` answer rather than a blocked thread.
Readers take no latch because nothing runs beside them. Making it shared across threads (a `&sync` store with
per-row latches and a real wait) is the next slice of this work.

## Deferred

- Disk, WAL, the buffer manager and recovery (waiting on sysl's CRC32C and io_uring gaps); `transaction_status`.
- `max_predicate_log` coarsening, `max_snapshot_age` and `undo_retention_limit`.
- Overflow chains (§4.7–§4.8, waiting on sysl 0.12's CRC32C): until then a row over the inline limit is refused
  with `entry_too_large`.
- Secondary unique keys as B+trees of §4.9 entries, with SECONDARY undo records and delete marks: the unique keys
  stay sorted buffers in memory, entries named by primary key, which the pages do not hold.
- A retype of a primary-key column (its key encoding would change): keys are encoded with the key types the
  table had when the store first touched it.
- Translation of a value whose representation a lossless retype changes other than integer to float (to
  `decimal`, `json` to `text`); the commit latch of `ddl.md` §6.1 rule 3, which a single-threaded store does not
  need.
- Non-unique secondary indexes, and unique indexes with a predicate or an expression component: those need the
  executor's evaluator inside the store, so such an index enforces nothing in the in-memory store yet. A
  referencing scan is therefore a full scan of the referencing table. Check and NOT NULL constraints are checked
  by the executor (`executor.md` §4.5).
- A shared `LOCK_ONLY` mode (`ddl.md` §11 question 2): concurrent inserts referencing one row serialize on it.
- Term bits in transaction ids and commit numbers (`replication.md` §1.4).
