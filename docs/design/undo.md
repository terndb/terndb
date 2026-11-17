# The undo log

`sh/sysl/terndb/undo.sysl` is the undo log of the in-memory mode: undo pages laid out byte for byte as
`on-disk-format.md` §5 specifies, the records of §5.2 chained as §5.3 says, the version a snapshot sees
rebuilt as §5.4 says, rollback, and the truncation and purge of §5.5–§5.6. `undo_check.sysl` holds the
structural check. Nothing calls it yet: wiring it under the store (`store.sysl`), in place of the store's
in-memory version lists, is a later item. Its tests are `undo_tests.sysl` (each edge case by name) and
`undo_model_tests.sysl` (a seeded model test).

## Structure

- **`UndoRecord`** is a decoded §5.2 record; `encode_undo_record` and `decode_undo_record` are the bytes.
  All six kinds encode and decode. INSERT, UPDATE and DELETE are written; LOCK and SECONDARY are not yet
  (§ Deferred), and a version chain that names one is `data_corrupted`.
- **`ColumnImage`** is one non-key column as a row stores it: null, or its encoded bytes (§4.4), a
  32-byte stub for a column out of line. **`RowImage`** is the §4.1 header (`RowHeader` of
  `row_format.sysl`) and those columns. This is exactly the grain at which an UPDATE keeps old values,
  so the undo log needs no column types; converting a §4.2 row body to and from it is the wiring
  item's.
- **`UndoSpace`** is the undo space (§2.1 space 1, §5.1): the resident pages, which are always the
  consecutive page numbers from `base` to the high-water mark `next_page`; the one lane's write
  position, 0 when it holds no page; and `low`, which is `undo_low`. A page's PID is
  `0x1000000000000000 | n`, its header `type` 0x05 and `owner` 0, and its first record is at byte 32,
  so page 0's first address is 32 and 0 stays "none" (§1 rule 8). Checksums, LSNs and key versions
  stay 0 in memory (§3.1 rule 3, §8.1 rule 3).
- **`UndoRows`** is what the store will become: the clustered leaf (a buffer of rows sorted by table
  then key, standing in for the B+tree), the transactions by id, the visible horizon `h`
  (`isolation.md` §4.1) as `last_commit`, the live snapshots, and the horizon of the last purge.
  A transaction is active, committed with its commit number, or rolled back, and keeps `last`, the
  head of its rollback chain.

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
- **Commit** changes no row. It gives T commit number `h + 1` and publishes it, so whether a snapshot
  sees a row's `txn` is read from the transaction table.
- **Reconstruction** (§5.4). A reader with snapshot `s` in transaction `own` sees `txn` when it is
  `own` or committed at or before `s`. Until it does, the record at `undo` is applied backward: INSERT
  answers "no row"; DELETE and UPDATE give the old version with `DELETED` as flag bit 0 says,
  `txn ← prev_txn`, `undo ← prev_version`, and an UPDATE restores its columns and schema version. Needing
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
transaction's rollback chain reaching exactly its records at or above `undo_low`, and a live
transaction's never falling below it; and each row in key order, with no flag but `DELETED`, an undo
address, a writer that began and was not rolled back, no ghost whose DELETE was purged, and a version
chain down to `undo_low` whose records are the row's own, of the kinds a chain holds, and in commit
order.

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
- LOCK records and `LOCK_ONLY` rows; §5.2 records no "old version was lock-only" flag, so a lock over a
  committed lock-only row needs a decision first. SECONDARY records and delete-marked index entries.
- Out-of-line old values (flag bit 1, and freeing the old overflow chain in purge, §5.6): overflow
  chains do not exist yet (`docs/design/row-format.md`). The decoder accepts bit 1 and the writer never
  sets it.
- `undo_retention_limit`, `max_snapshot_age` and replica feedback (§5.5).
- Rollback to a savepoint: the rollback chain supports it (stop at the record that was `last` when the
  savepoint was taken), but no caller needs it until the store is wired.
- Replacing the store's version lists with this module, and the leaf with the B+tree.
