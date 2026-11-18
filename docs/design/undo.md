# The undo log

`sh/sysl/terndb/undo.sysl` is the undo log of the in-memory mode: undo pages laid out byte for byte as
`on-disk-format.md` §5 specifies, the records of §5.2 chained as §5.3 says, the version a snapshot sees
rebuilt as §5.4 says, rollback, and the truncation and purge of §5.5–§5.6. `undo_check.sysl` holds the
structural check. The store (`store.sysl`) keeps its rows in an `UndoRows`. Its tests are `undo_tests.sysl`
(each edge case by name), `undo_lock_tests.sysl` (LOCK records), `undo_leaf_tests.sysl` (the leaf, its
layouts and rollback to a record) and `undo_model_tests.sysl` (a seeded model test).

## Structure

- **`UndoRecord`** is a decoded §5.2 record; `encode_undo_record` and `decode_undo_record` are the bytes.
  All six kinds encode and decode. INSERT, UPDATE, DELETE and LOCK are written; SECONDARY is not yet
  (§ Deferred), and a version chain that names one is `data_corrupted`.
- **`ColumnImage`** is one non-key column as a row stores it: null, or its encoded bytes (§4.4), a
  32-byte stub for a column out of line. **`RowImage`** is the §4.1 header (`RowHeader` of
  `row_format.sysl`) and those columns. This is exactly the grain at which an UPDATE keeps old values,
  so the undo log needs no column types.
- **The leaf** is one B+tree per table (`btree.md`, `LeafTree`), over the data space `data`, keyed by the
  encoded primary key; each value is the §4.1 header and the §4.2 body. A body needs, beyond the column
  images, only which columns are fixed-width and how wide: a **`RowLayout`** per table and schema version,
  which the store declares from the version's types (`layout`). A version never declared takes its layout
  from its first row, every column variable-width, which is how bytes with no type are stored (the tests
  here). `encode_leaf_row` gives exactly the bytes `encode_row` gives for the same types. A row is refused
  before any record is written (`admit`) when its column count or a fixed width is not the layout's
  (`type_mismatch`), its key is over 1,024 bytes (`key_too_long`) or key and row exceed 4,000 bytes
  (`entry_too_large`: no column goes out of line until overflow chains exist).
- **`UndoSpace`** is the undo space (§2.1 space 1, §5.1): the resident pages, which are always the
  consecutive page numbers from `base` to the high-water mark `next_page`; the one lane's write
  position, 0 when it holds no page; and `low`, which is `undo_low`. A page's PID is
  `0x1000000000000000 | n`, its header `type` 0x05 and `owner` 0, and its first record is at byte 32,
  so page 0's first address is 32 and 0 stays "none" (§1 rule 8). Checksums, LSNs and key versions
  stay 0 in memory (§3.1 rule 3, §8.1 rule 3).
- **`UndoRows`** is the rows of the store: the undo space, the leaf and its layouts, the transactions by
  id, the visible horizon `h` (`isolation.md` §4.1) as `last_commit`, the live snapshots, the horizon of
  the last purge, and `undone`, the records rolled back to a savepoint. A transaction is active,
  committed with its commit number, or rolled back, and keeps `last`, the head of its rollback chain.

## How each operation maps to §5

- **Append** (§5.1 rules 2–3). A record goes on the lane's page when it fills the page exactly or
  leaves at least 3 bytes, the shortest PAD; otherwise the lane covers the tail with one PAD and takes
  the page at the high-water mark. A page filled exactly leaves the lane with no page. A record over
  16,352 bytes is `entry_too_large` (`D2002`); with keys of at most 1,024 bytes and in-place entries
  of at most 4,000 (§3.6, §4.7) none arises.
- **Writes.** Every write by transaction T appends one record whose `txn_prev` is T's previous record
  and whose `prev_version`, `prev_txn` and `old_schema_version` are the row's header before the write;
  the row then carries `txn = T` and `undo` = the record's address. An insert where no row is there is
  an INSERT. **An insert over a ghost is an UPDATE with flag bit 0 set, listing every column of the
  ghost**, so the deleted version and every version before it stay reachable. An update lists the
  columns whose value changes, or every column of the old schema version when the version changes
  (§5.2); one that changes nothing still writes a record with no old values. A delete sets `DELETED`
  and leaves the columns. A write over a row whose `txn` is another live transaction answers
  `UndoLocked(holder)` and changes nothing (§4.1, the write lock is the `txn` field); deciding to wait,
  or `serialization_failure` for a version newer than the snapshot (`isolation.md` §4.2), stays the
  store's. A transaction that writes a row twice writes two records, so the rollback chain can undo
  either, as savepoints will need.
- **Locks** (`lock`, Q49). A transaction that neither wrote nor locked a live row takes its write lock
  with a LOCK record: the row then carries `txn = T`, the record's address and `LOCK_ONLY`, its columns
  untouched. A LOCK, UPDATE or DELETE over a row with `LOCK_ONLY` sets record flag bit 2, "the old
  version was lock-only", and an UPDATE or DELETE clears `LOCK_ONLY` (a write is a version, a lock is
  not). `writer_of` finds a version's writer: its `txn`, or for a lock-only row the `prev_txn` of the
  LOCK at `undo`, past every LOCK with bit 2; 0 when that record is below `undo_low`, where the version
  is seen by every snapshot. While T is active the row is locked as by any write; after T commits the
  row keeps `LOCK_ONLY`, and its version is still its writer's.
- **Commit** changes no row. It gives T commit number `h + 1` and publishes it, so whether a snapshot
  sees a row's `txn` is read from the transaction table. `commit_at(T, c)` ends a transaction that wrote
  no version as committed at an existing commit point (its snapshot), publishing nothing.
- **Reconstruction** (§5.4). A reader with snapshot `s` in transaction `own` sees a version when its
  writer (`writer_of`) is `own` or committed at or before `s`. Until it does, the record at `undo` is
  applied backward: INSERT answers "no row"; DELETE, LOCK and UPDATE give the old version with `DELETED`
  and `LOCK_ONLY` as flag bits 0 and 2 say, `txn ← prev_txn`, `undo ← prev_version`, and an UPDATE
  restores its columns and schema version. A LOCK under a version without `LOCK_ONLY`, or another kind
  under one with it, is `data_corrupted`. Needing
  a record below `undo_low` is `data_corrupted` (rule D17), as is a record of another transaction than
  the row's `txn`. The answer is a copy that shares no buffer with the leaf. This gives the same
  version as the store's "own version, else the newest committed at or before `s`", which the model
  test checks.
- **Rollback** walks T's rollback chain from `last`, and applies each record backward to the row it
  names, which must still carry T and that record's address: an INSERT removes the row, anything else
  restores the old header and columns. No row ever names a rolled-back transaction afterwards.
  **Restoring a ghost whose DELETE is already below `undo_low` removes the row instead**: purge passed
  that DELETE while the rolled-back insert owned the row, so it left the row, and no later purge would
  reach it again. The ghost is invisible to every snapshot, since its DELETE committed at or before the
  purge horizon. The model test found this case.
- **Rollback to a record** (`rollback_to(T, stop)`, for a statement that must wait part-way and for a
  savepoint): the records above `stop`, an earlier head of T's chain (`last_record`), are applied
  backward as rollback applies them, and `stop` becomes the head again. T stays active. The records
  rolled back stay in the log, off T's chain and named by no row, and are listed in `undone` until purge
  passes them.
- **Purge and truncation** (§5.5, §5.6). The horizon is the oldest live snapshot, or `h` with none.
  `purge(upto)` takes the older of the horizon and `upto`, but never less than the last purge's (the
  `PURGE_HORIZON` rule that it never decreases). From `undo_low` in address order a record is finished
  when its transaction committed at or before that horizon (a DELETE then removes its ghost if the
  row's `undo` is still the record's address) or was rolled back; PAD always is. The walk stops at the
  first record that is not finished, and `undo_low` advances to it, dropping every page below. When it
  reaches the end of the log the lane leaves its page first, so an emptied log holds no page and the
  next record starts a new page number.

## The invariant checker

`check_undo(rows)` fails with `data_corrupted` (`D1001`) naming the page, record, transaction or row at
the first broken rule: the resident pages exactly `base` up to the high-water mark with `undo_low` on
the first (or, with none resident, at the next page's first address), and the lane on the newest; each
page's header (type, format, own PID, owner 0, no checksum, LSN or key version); every record decoding,
the records packing each page to its end (or to the lane's position, with zero bytes after it), and
`undo_low` a record boundary; each record's transaction known and its links running backward; each
transaction's rollback chain reaching exactly its records at or above `undo_low` but those rolled back to
a savepoint, and a live transaction's never falling below it; the B+tree check of every table's tree
(`check_btrees`, which covers key order); and each row with no flag but `DELETED` and `LOCK_ONLY` (never
both), an undo address, a writer that began and was not rolled back, no ghost whose DELETE was purged, and
a version chain down to `undo_low` whose records are the row's own, of the kinds a chain holds, none rolled
back, a LOCK exactly under each lock-only version, and in commit order between versions that are not
lock-only.

## Spec gaps

- **The undo space has no bitmap.** §3.7's bitmaps are page 0 of each data segment; undo pages are
  taken at the high-water mark (§5.1 rule 3), freed only by truncation below `undo_low` (§5.5 rule 1),
  and never reused. The resident pages are therefore always one range, and that range is the whole
  allocation state; freeing a page drops its frame. The check requires exactly that range.
- **The shortest PAD.** §5.1 rule 2 requires every unused tail to be a PAD but gives no PAD layout and
  no minimum. A PAD is now `len` and `kind` (3 bytes at least) with its other bytes zero, and a writer
  that would leave a tail of 1 or 2 bytes moves the record to the next page instead. The rule now says
  so.
- **Rolled-back transactions and truncation.** §5.5 lets `undo_low` pass only records of committed
  transactions, so a rolled-back transaction's records would pin undo for ever. Records of a
  transaction whose rollback has finished qualify too, since no row names them. The rule now says so.
- **Chains of one transaction.** §5.3 says `txn` strictly decreases along a version chain in commit
  order. A transaction that writes a row twice puts two records with equal `txn` next to each other
  (the second's `prev_txn` is its own `txn`); the order is strict between distinct transactions.
- **An insert over a ghost.** §5.2 does not say which record it writes. An INSERT would end the chain
  at "absent" and lose the versions behind the ghost from older snapshots, so it is an UPDATE with
  flag bit 0 listing every column of the ghost, which is what bit 0 exists for.
- **Ordinals of an UPDATE** must ascend strictly; a repeated or descending ordinal is
  `data_corrupted`. §5.2 does not say, but a repeat would make applying the record ambiguous.
- **Purge order.** §5.6 lets purge walk every committed record below the horizon. Here it stops at the
  first unfinished record, so a ghost after a long transaction's first record waits for it; the walk is
  then a prefix and is never repeated.

## Concurrency

Single-threaded, like the store, with one lane. The lane design of §5.1 rule 3 holds: nothing assumes
contiguity of a transaction's records. The check's "links run backward" is true of one lane only.

## Deferred

- WAL records for undo (`UNDO_APPEND`, `UNDO_TRUNCATE`, `PURGE_HORIZON`, §6.4) and recovery's use of
  them (§6.9, §6.10); checksums; latching, several lanes and epoch-based reclamation.
- SECONDARY records and delete-marked index entries.
- Out-of-line old values (flag bit 1, and freeing the old overflow chain in purge, §5.6): overflow
  chains do not exist yet (`docs/design/row-format.md`). The decoder accepts bit 1 and the writer never
  sets it; a leaf row with `HAS_EXTERNAL` is `data_corrupted` here.
- `undo_retention_limit`, `max_snapshot_age` and replica feedback (§5.5).
