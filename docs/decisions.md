# TernDB spec decisions

These are the answers applied to the open questions of the four specs in `docs/spec/`. Each was the director's
recommendation, applied so the specs could move on; **the user may overrule any of them**. Every row names exactly
the spec sections it changed, so a decision is reverted by restoring those sections and putting its question back
in that spec's "Open questions". Abbreviations: QT `query-tree.md`, ISO `isolation.md`, DISK `on-disk-format.md`,
WIRE `wire-protocol.md`, ERR `errors.md`, DDL `ddl.md`, CAPI `c-api.md`. Rows Q30 onward were decided while
writing `ddl.md`; to revert one, restore its sections and add its question to `ddl.md` §11. Rows C1 onward were
decided while writing `c-api.md`; to revert one, rewrite its sections and add its question to `c-api.md` §16.
Rows P1 onward were decided while writing `replication.md` (REPL); to revert one, rewrite its sections and mark its
question in `replication.md` §16 open again.

## Query tree, types and values

| Id | Question | Decision | Sections changed |
|---|---|---|---|
| Q1 | Literals in the tree? | Add `Literal` nodes for constants of the statement; they are part of the shape and the plan-cache key. Values that vary stay parameters. Injection stays impossible because trees are structured. | QT §1.2, §7 table, §7.3, §10.2, §12.4 (Expr kind 17), §12.6 heading and closing paragraph, §14.1 (literal rows), §15; ERR `Q3017`, `Q1007` |
| Q2 | Unsigned integers and `int8`? | Integers are `int8…int64` and `uint8…uint64` (`int128` kept; see QT Open question 1). | QT §2.1, §4.1 rules 1–2, §4.4 rule 1, §12.3 (kinds 21–25), §12.6, §14.1 types row, §14.2 (`i8` row removed); DISK §4.3, §4.4, §4.6; WIRE §6.2 table, §6.8 |
| Q3 | Unbounded `numeric`? | None in v1; `decimal` takes up to 38 digits. | QT §2.1 note 1; DISK §4.3 (`decimal` 1–38), §4.4 (p > 38 row removed), §4.6 |
| Q4 | `list<int64>` directly? | Yes: scalar relations, exposing one column `value`. | QT §2.5 rule 2, §6.7 (`collect`), §6.14, §14.1, §14.2; DISK §4.3 (tag 0x22), §4.4, §4.5, §4.6; WIRE §6.2 rule 6; ERR `Q3014` |
| Q5 | A `json` type? | Yes, a `json` scalar for interop, held in canonical (RFC 8785) form. Nesting stays relational. | QT §2.1, §3.1, §3.2, §3.3, §3.5, §4.1, §12.3 (kind 26), §12.6; DISK §4.3 (tag 0x17), §4.4, §4.6; WIRE §6.2, §6.8; ERR `Q1007` |
| Q6 | Collations? | None in v1 (binary UTF-8 order). Later they go on the comparison, with ICU sort keys in indexes; hook reserved. | QT §3.1 rule 4; DISK §3.9 (`key_spec` bit 2), §4.6 rule 4 |
| Q7 | Format of the published tree schema? | A JSON description generated from the implementation's source. | QT §13.1 |
| Q8 | Support window for majors? | A server accepts its current major and the two previous ones. | QT §13.3 |
| Q9 | Data-modifying CTEs, grouping sets? | Deferred from 1.0, as the spec already said. | none (question removed from QT §17) |
| Q10 | Substrait outbound, version range? | Import-only, pinned to one Substrait version per release. | QT §14 intro; ERR `Q7004` |
| Q11 | `arbitrary: true` for `Limit` over a bag? | Kept. The GraphQL front end orders by primary key instead of setting it. | QT §6.10 rule 2 |
| Q12 | Canonical text forms of values? | A short "values" section in QT. | QT §3.5 (new), §7.6 |

## Isolation

| Id | Question | Decision | Sections changed |
|---|---|---|---|
| Q13 | Wait or abort on a locked row? | Wait, bounded by `lock_timeout` and deadlock detection; the only policy. | ISO §4.2 rule 1 |
| Q14 | Element-level versioning for nested relations? | None in v1: one value per row, stored through the overflow chain. | ISO §4.5 closing paragraph; DISK §4.5 (`form`), §4.8 rule 5 |
| Q15 | Projection-aware validation? | Yes: validation is column-precise; only columns read are checked. | ISO §2.2 (item read), §2.3 (rw), §4.3 (pseudo-code, Precision), §4.6 (serializable proof) |
| Q16 | `synchronous = remote`? | In Phase 3, with replication. | ISO §6, §8 |
| Q17 | Fencing the old primary? | A term number in the WAL; details in `replication.md`. | ISO §8; DISK §6.6 (`term` at offset 36) |
| Q18 | Bounded-staleness replica reads? | Optional `max_lag`. | ISO §3.6 (R3a); WIRE §3.5 `TxnOptions.max_lag_ms`; ERR `T1005` |
| Q19 | `max_snapshot_age` default? | 1 hour, configurable (0 = no limit). | ISO §5.5; DISK §5.5; ERR `T1008` |
| Q20 | Language of the model? | sysl, run inside the simulator in CI. | ISO §7.4 |
| Q21 | Online DDL? | Its own spec, `ddl.md`, next. | ISO §5.6 (last bullet) |
| Q22 | `REPEATABLE READ`, `READ UNCOMMITTED` synonyms? | Accepted only in the Postgres wire layer. | ISO §1 |

## On-disk format

| Id | Question | Decision | Sections changed |
|---|---|---|---|
| Q23 | Overflow values in the WAL? | Written in full to the WAL in v1. | DISK §4.8 rule 4 |
| Q24 | Replica feedback? | Only through opt-in feedback (`replica_feedback`, off by default); otherwise the replica query is cancelled with `replica_conflict`. | DISK §5.5, §6.11 rule 3; ISO §5.6 |
| Q25 | Writing the previous format? | A release can still write format N−1 until `--upgrade-format`. | DISK §7.2 rules 2–5; ERR `D3001`, `D3002` |
| Q26 | Per-core undo lanes? | From the start. | DISK §5.1 |
| Q27 | In-engine CoW clones on disk? | No: on-disk clones are reflink copies only. | DISK §8.4; ERR `D3003` |
| Q28 | Asynchronous commit? | It is `synchronous = off`; no separate feature. | ISO §6; DISK §6.7 rule 2 |
| Q29 | Encryption at rest? | Reserve the fields now: CONTROL `feature_flags` bit 1, page `key_version`, WAL segment `key_version`. | DISK §2.2, §3.1 (`owner` narrowed to 48 bits, rule 2), §3.9, §6.6 |
| Q48 | How is a unique index entry with a NULL component stored? | In the non-unique form (index columns ‖ primary key, empty value), so NULLs never conflict with one another, as in the SQL standard and Postgres; a reader tells the forms apart by a NULL index component, and a uniqueness check reads only unique-form entries. To revert: drop the sentence from DISK §4.9 rule 1 and from DDL §2.6 rule 5. | DISK §4.9 rule 1; DDL §2.6 rule 5 |
| Q49 | How is a LOCK over a committed lock-only row recorded? | Undo record `flags` bit 2, a free bit of the existing field, marks a record whose old version had `LOCK_ONLY` (a LOCK over a lock, or an UPDATE or DELETE over a locked row); no layout change. Finding a version's writer skips every LOCK record with bit 2; reconstruction restores `LOCK_ONLY` from it; the LOCK is still the row's write lock while its transaction is active. To revert: drop bit 2 from DISK §5.2 and the skipping from §4.1 and §5.4. | DISK §4.1 (`LOCK_ONLY`), §5.2 (`flags`, lock-only old versions), §5.3 rule 1, §5.4 steps 1 and 3 |
| Q50 | In the external null bitmap, what is "variable column i"? | The column's number among the non-key columns, fixed-width columns included, exactly as the null bitmap numbers them; not its position among the variable-width columns. A set bit on a fixed-width or NULL column, or at or past n, is `data_corrupted`; a dropped column has no number in later schema versions. | DISK §4.2 item 2 |
| Q52 | Do undo pages have an allocation bitmap? | No: undo pages are taken at the high-water mark and freed only by truncation below `undo_low`, addresses are never reused, so the resident run of pages is the whole allocation state; truncation is what returns undo pages to the page space. §3.7's bitmap is for data segments only. | DISK §3.7 (rule 4), §5.1 rule 1 |

## Wire protocol

| Id | Question | Decision | Sections changed |
|---|---|---|---|
| W1 | Default port? | 7365. | WIRE §1.1 rule 1 |
| W2 | `bool` width in results? | One octet, as specified. | none (question removed) |
| W3 | Columnar parameter input? | Bulk relation parameters may use the result encoding. | WIRE §4.2 (`columnar` list, rules 2–5); QT §10.4 |
| W4 | Mutual TLS? | In Phase 2. | WIRE §2.4 rule 9 |
| W5 | Token format? | Opaque random tokens, stored hashed. | WIRE §2.4 rule 6 |
| W6 | Plaintext TCP? | Never; local unencrypted access is the Unix socket with `peer` auth. | WIRE §1.1 rules 3–4, §1.2, §2.4 rule 3; ERR `W3003` |
| W7 | Plan-cache scope for digests? | Keyed per role. | WIRE §4.4 rule 5; ERR `W2004` |
| W8 | Compression? | zstd per batch, negotiated, off by default. | WIRE §3.4 (`ClientHello.compression`, `ServerHello.compression`), §6.4 rule 7 |
| W9 | Notifications? | Later; the message types stay reserved. | WIRE §3.2 |
| W10 | Ordered implicit transactions? | No. | WIRE §4.6 rule 3 |
| W11 | `ExecuteSql`? | No. | WIRE §0 rule 4 |
| W12 | Arrow C Data Interface? | Export comes later. | WIRE §6.8 rule 3 |
| W13 | Union discriminant width? | Keep 1 or 2 octets by arity; unions capped at 65,536 variants. | WIRE §6.2 rule 5; QT §2.4 rule 1; ERR `Q3016`, `W2017` withdrawn |
| W14 | Error-code style? | One style everywhere: area letter (`Q R T W D`) and four digits, with a `snake_case` name as alias, registered in the new `docs/spec/errors.md`. | ERR (new); QT §11.3; ISO §5.2; WIRE §8.1 rules 1, 5, 8, §8.2, §8.3; DISK §1 rule 9 and the sites that now name `D` codes (§3.1, §3.6, §3.10, §4.7, §7.2, §8.4) |
| W15 | What does DML without `returning` return? QT said an `int64` count; WIRE sent no result type and the count in `Complete`. | No value: the statement has no result type; its row count is an execution outcome in `Complete.counts` (and `terndb_result_counts`), one per parameter tuple, 0 for DDL. Every value is then a relation read by one decoder, and a batch gets a count per tuple. | QT §5 rules 2–3; WIRE §0 rule 4, §3.6 (`Prepared`, `ResultHeader` comments), §4.3 rules 3–4, §11 |
| W16 | Are the locks taken after a savepoint released by a rollback to it? The spec was silent. | Yes, all of them: row write locks, `LOCK_ONLY` locks and catalog locks; those taken before the savepoint are kept. Nothing kept depends on them (Postgres releases a rolled-back subtransaction's row locks the same way, and keeps its table locks, which TernDB does not have). Revert: keep them to the transaction's end, by not truncating the lock lists in the undo. | WIRE §5.5 rule 5; ISO §4.3 (Savepoints) |
| W17 | Does a rollback to a savepoint clear an error with a retryable code? | No: a retryable code guarantees the transaction is aborted and had no effect (ISO §5.2), so it rolls the transaction back whole at once and a later `RollbackToSavepoint` is `W2003`. A non-retryable error after a savepoint keeps the transaction's writes and locks until it is rolled back. Revert: let any error be cleared, and weaken ISO §5.2's guarantee to hold only for a transaction with no savepoint. | WIRE §5.3 rule 1, §5.5 rule 6; ISO §5.1; ERR `W2003` |
| W18 | Once a newer savepoint shadows a name, can the older one be reached again? | Yes, as in Postgres: once the newer one is released or rolled back past, the older one is the newest of its name. | WIRE §5.5 rule 1 |
| W19 | What does `lock_timeout_ms` bound, and what do absent and `0` mean? The spec was silent. | The whole wait of a statement for a lock, from when it first parks until a run of it gets past the lock, so a re-run that parks on a new holder does not restart it; a wait reaching the value fails with `lock_timeout` (retryable, rolling the transaction back). Absent (the server default) bounds no wait; `0` does not wait at all, which is `NOWAIT`, unlike Postgres's `0` = no limit, since `opt<uvar>` already spells "no limit" and `statement_timeout_ms` reads `0` the same way. Deadlock detection runs first. Revert: `0` = no limit, or a per-holder wait. | WIRE §7.5 rules 4–5 |
| W20 | Which span does an error carry, and which errors carry none? The spec said only "one covers the node at `path`". | The span whose index is the pre-order index of the node `path` names, on every entry; none for an empty path or one that names no node (`/params/…`, `/spans/…`), none from an ancestor when the node has no span of its own, and none for a decode error, since the spans follow the statement and are never read. Revert: fall back to the nearest spanned ancestor. | WIRE §8.1 rule 4 |

## DDL and the catalog

| Id | Question | Decision | Sections changed |
|---|---|---|---|
| Q30 | Make DISK §4.3 match the QT types exactly? | Yes: descriptors are one-to-one with QT §2 and use its names (`datetime`, `timestamp`, `int8`…); a record descriptor (0x23); one relation descriptor (0x20) whose element is a record, union or scalar, with an element `nullable` octet; nullability lives in `sys.columns.nullable`, record-field and element octets, so the nullable wrapper needs no tag; union arity is a `u32`; tags 0x12 and 0x22 stay unassigned. Format 1 is unreleased, so no format bump. | DISK §4.2 (closing sentence), §4.3, §4.4, §4.5, §4.6, §10 |
| Q31 | Is enum a type? | No: an enum is a union whose payloads are all `{}`, nameable in the catalog; it is stored as a union (a 2-octet discriminant), and may be a key component, encoded as its big-endian discriminant. | QT §2.4 rule 1; DISK §4.3 rule 2, §4.5, §4.6 (table, rule 4); DDL §2.8 |
| Q32 | Named catalog types: nominal or aliases? | Aliases for a structure (type kind 32, `QualName` payload, resolved at validation, kept in the shape); results stay structural; a column remembers the name it was declared with for generators; changing a named type retypes its columns losslessly. | QT §12.3 (kind 32 and the paragraph after it), §15; DDL §2.8; ERR `Q6004`, `Q6018` |
| Q33 | Tables without a primary key? | Not allowed; an `identity` column is the cheap key. | DDL §2.2 rule 2; ERR `Q6009` |
| Q34 | How does online DDL meet running transactions? | DDL waits for no reader or writer (the shared catalog lock of ISO §5.6 is gone); the catalog read is column-precise, and only a DDL that changed something a transaction read aborts it with `schema_changed`; a writer that straddles a DDL reconciles its own writes at commit (new indexes, constraints, columns, shadows) under a per-table commit latch; background scans need no wait for old transactions. | ISO §5.6 (bullets 2–4, last); DDL §6, §7 rule 2; ERR `T1004` |
| Q35 | Index kinds in v1? | Unique, partial and expression indexes; every build is online (no blocking form); no multi-valued indexes over nested values, no covering columns. | DDL §2.5, §8.9; DISK §3.9 (`key_spec` 0xFFFF), §4.6 rule 5, D22 |
| Q36 | Adding constraints online? | Added not validated and enforced at once, then validated by a background job; a failed validation leaves the constraint not validated rather than removing it; foreign keys have the five SQL actions, simple matching and an optional deferred check; at SI/RC a check locks the parent row `LOCK_ONLY`. | DDL §2.6, §8.7 |
| Q37 | Sequences or identity? | Both: identity columns own a sequence; sequences are non-transactional (gaps), with a logged `cached_to` so no value is ever reissued; `next_value(schema, name)` is volatile. | DDL §2.7; QT §7.12; DISK §3.9 (`sys.sequence_state`); ERR `R0008` |
| Q38 | Roles and privileges in v1? | Minimal: roles with login, superuser and membership; owners; table/view, namespace, sequence and type privileges; checked at validation; all catalog relations readable by all roles; no column privileges or row security. | DDL §2.10; ERR `Q6015` |
| Q39 | Shape of a DDL statement? | One statement kind per object verb (32–53); `AlterTable` carries a list of actions applied as one schema version; DDL declares no parameters and returns no value; jobs are awaited by `AwaitJobs(txid)`, which returns a relation, rather than by an option on the DDL. | DDL §3; QT §1 rule 5, §5, §12.5; WIRE §0 rule 4; ERR `Q6001`, `Q6010` |
| Q40 | Changing the primary key, and running out of schema versions? | A rebuild into a shadow table under a new oid, filled by dual writes and a copy job, swapped in one transaction; `rewrite` uses the same path and restarts schema versions at 1. | DDL §5.5, §8.11; DISK §5.6 (freeing a dropped tree); ERR `Q6012` |
| Q41 | How is the catalog queried? | Read-only system views in the namespace `catalog`, a stable versioned contract; the `sys.*` trees are internal and cannot be scanned; types are reported as QT §12.3 bytes plus text. | DDL §4; DISK §3.9 |
| Q42 | Generated columns and volatile defaults? | Generated columns are stored only; adding a column with a volatile default, a generated column or an identity column is a background fill, invisible until done. | DDL §2.7 rule 2, §8.2 |
| Q43 | Catalog storage for the new objects? | `sys.objects` gains kinds 6–8, `definition` (replacing the empty `options`), `state` and `owner_oid`; `sys.columns` gains `column_id`, `type_oid`, `fill`, `generated`, `identity`, `state` and loses `dropped`; trees 4–7 are `sys.depends`, `sys.grants`, `sys.jobs`, `sys.sequence_state`; an update that changes a row's schema version logs the whole old row in its undo. | DISK §3.9, §5.2 (UPDATE payload) |
| Q44 | Is a logged read enough for a serializable foreign-key check? | No: the check locks the referenced row `LOCK_ONLY` at every level (and is also logged at serializable), since a snapshot or read committed delete validates nothing and could otherwise commit an orphan beside a serializable insert. Unique and referenced values are checked with ISO §4.2 rules 1, 2 and 4 (wait for a live writer; a commit after the snapshot is a race); a deferred check that meets a locked referenced row at commit is `lock_timeout` (`serialization_failure` is never raised at read committed); actions run in the statement, unique keys are checked once per batch of a statement's writes to a table. | DDL §2.6 rules 2, 3 and 5, §11 question 2; ISO §4.2 rule 4, §4.4 table; QT §9.3 rule 2 |
| Q45 | How does transactional DDL meet commits it did not see, and is a reader validated against them? | A transaction with an empty write set is serialized at its snapshot and its catalog read is not validated at commit (a reader of a dropped table commits); a create or rename also locks the name in its namespace; at commit a transaction's DDL is applied in order to the newest committed catalog under the oids it first created, and a statement that no longer applies is `schema_changed`; reconciliation with a new unique key that meets a live writer is `lock_timeout`. | ISO §5.6 bullet 3; DDL §6.1 rule 1, §6.3 rule 1, §6.4 rule 1, §9 rules 7–8; ERR `T1003`, `T1004` |
| Q46 | How does an uncommitted `AlterSequence` meet draws, which are not transactional? | It moves no draw until it commits: every draw, the altering transaction's own included, takes the committed options at the shared position; at commit the alter is applied to the position draws have left (a `restart` sets it); a rollback leaves the position as the draws left it, so no value is drawn twice. The alter takes the sequence's catalog lock even when it only restarts. A sequence the transaction created acts at once. | DDL §2.7 rule 8, §9 rule 2 |
| Q47 | Which catalog decides a login, and what does a change to a role do to a connection already made? | A login reads the newest committed catalog: a role an open transaction created, or one without `login`, fails as an unknown user does (`W3001`), and which methods are permitted does not depend on the role existing. The connection runs as the authenticated role; losing `login` or the role being altered stops the next login only, and every statement of a connection is checked as its role against its transaction's catalog. There is no `SET ROLE` or `SET SESSION AUTHORIZATION` in v1. | DDL §2.10 rule 1 |
| Q51 | What stops a table too wide to store any row? | `CreateTable` and `AlterTable` (`add_column`, `retype_column`) refuse, with the new `Q6022` `row_cannot_fit`, a table whose smallest row (20-octet header, key, null bitmap, fixed-width columns in full, a `u16` offset for each variable-width column, nested and list columns included) exceeds the 4,000-octet inline limit; with no key and no fixed column that is more than 1,872 variable-width columns. The limit is derived from the row-format constants; `Q6011` is checked first. A retype counts the column at its new type as well as its current one, at the moment the `AlterTable` is accepted, so the table fits before and after the conversion finishes. | DDL §2.2 rule 4, §2.13 rule 10, §8.1, §8.5; ERR `Q6022` |
| Q53 | What happens to a retype of a column whose earlier retype is still converting? | It is refused, with the new `Q6023` `column_retyping` ("the column is being retyped; wait for the conversion to finish"), before the conversion or the row limit is looked at, so not `Q6008`. Nothing is recorded and the other actions of the statement are not applied. The column can be retyped once the conversion finishes; a metadata-only retype never blocks, and another column is unaffected. | DDL §2.13 rule 9, §8.5 rule 4; ERR `Q6023` |
| Q54 | What does `drop_column` of a converting column do? | It is allowed and abandons the conversion: the column, its target, `using`, guard and replacement indexes go in the statement's transaction and the `convert_column` job is cancelled in it (§7 rule 6), so no converted data survives and the job has nothing left to do. A column re-added under the name is new (new id) and never converting. | DDL §2.3 rule 3, §2.13 rule 11, §8.3 |
| Q55 | What does `rename_column` of a converting column do? | It is allowed and the conversion continues under the new name: the id, state and target are kept, `using` and the guard are rewritten, and the job's final transaction makes the renamed column `ready`. A retype of it under the new name is `Q6023`. | DDL §2.13 rule 11, §8.4 |
| Q56 | What does `set_default` of a converting column do? | It is allowed only if the default is valid at the current type and at the target type (a null default is valid at both when both are nullable); a default failing either is `Q3001`, the existing code, so no new code. Dropping a default is always allowed. | DDL §2.13 rule 11, §8.6 |
| Q57 | What happens to a retype of a filling column? | It is refused with the new `Q6024` `column_filling`, at the same point as Q53 (before the conversion and row-limit checks), nothing recorded. `Q6023` was not reused: it says the column is being retyped, which a fill is not, and the remedy is another job. | DDL §2.13 rule 11, §8.5 rule 5; ERR `Q6024` |

## C ABI

| Id | Question | Decision | Sections changed |
|---|---|---|---|
| C1 | Async completion: callback or pollable descriptor? | A completion queue with a level-triggered descriptor (`terndb_queue_fd`), drained on the caller's thread; no completion callbacks. Every waiting operation has a sync form equal to async plus wait. | CAPI §6, §12 (queue and op functions), §15.1 properties 14–15 |
| C2 | Errors: thread-local last error or out-parameter? | An owned `terndb_error *` through a trailing `terndb_error **err` out-parameter (NULL allowed); no thread-local state. | CAPI §1 rule 4, §10, §12 (error functions) |
| C3 | Structs or opaque option objects? | No struct with a visible layout anywhere in the ABI: options are opaque objects with typed setters, open settings a string key/value `terndb_config` (unknown keys refused), the allocator two function pointers to `terndb_init`. Enumerations are `int32_t` with `#define`s. | CAPI §1 rules 1–2, §3, §4.1, §5.2, §12 |
| C4 | Client-chosen integer handles, as on the wire? | No: the library allocates transaction and statement handles; `terndb_begin_async` writes the handle at once so a begin, executes and commit pipeline without waiting. | CAPI §5.3 rules 2–3, §7 rule 1 |
| C5 | Closing a database or connection with live dependants? | Permitted in any order: dependants stay valid to free and fail other calls with `C1006`; a batch outlives its result, connection and database. | CAPI §2.2 rules 3–4, §4.3, §5.1 rule 4 |
| C6 | Codes for C-layer failures? | A new area `C` (`C1001`–`C1009`, `C2001`), and `D4001`–`D4004` for opening a database. Not sent on the wire. | ERR §1 rule 1, §6 (D4 rows), §7 (new); CAPI §10.3 |
| C7 | Threads and blocking? | Embedded engine is thread-per-core plus one service thread; no caller code on a core thread except the allocator; a connection is used by one thread at a time (overlap detected, `C1004`); a blocking call from an engine thread is `C1003`. | CAPI §2.3, §2.4 |
| C8 | Logging and metrics? | One process-wide log sink, called on the service thread from a bounded queue that drops rather than blocks; metrics pulled as snapshots per database with a minimum stable name set. | CAPI §11 |
| C9 | Promotion as a function? | `terndb_promote` = snapshot to a directory then open it in disk mode; the source stays open. Opening a memory-mode directory in disk mode is promotion too. | CAPI §4.2 rule 4, §4.4 rule 3 |
| C10 | Text across the ABI: `uint8_t` or `char`? | Text is `(const char *, size_t)` UTF-8, bytes `(const uint8_t *, size_t)`, neither NUL-terminated. | WIRE §9.1 rule 1; CAPI §1 rule 3 |
| C11 | Role of an embedded session? | A new database gets a superuser login role `terndb`, which an embedded session takes by default; no authentication in-process. | CAPI §4.2 rule 2, §5.1 rule 1 (and see CAPI §16 question 7) |
| C12 | Flow control for a pull API? | `prefetch_bytes` (default 4 MiB) bounds unpulled batches, mapped to wire credit in network mode. | CAPI §5.2 rule 3; §15.1 property 18 |

## Replication

| Id | Question | Decision | Sections changed |
|---|---|---|---|
| P1 | Where does the replication stream run? | Messages of the native protocol in its reserved ranges (`0x60–0x65`, `0xE0–0xE8`), as wire 1.1, on dedicated replication connections; `WalData`, `PrimaryStatus` and `ReplicaStatus` have fixed-width headers. | REPL §2, §3, §4.1, §5.1–§5.4, §11.1; WIRE §0 rule 3, §3.2 closing paragraph |
| P2 | Cascading replication? | None in v1: every replica streams from the primary; `max_replicas` default 16, at most 64. | REPL §1.1 rule 5, §12 |
| P3 | Timeline and term: one number or two? | One: the timeline id is the term, every timeline switch takes a term above every one known, each term has a random `promotion_id`, and history files record the lineage. | REPL §1.2; DISK §2 file set, §6.4 (`TIMELINE_SWITCH` payload), §6.6 (`term` = `timeline`) |
| P4 | Telling a causal token from a commit lost by failover? | Transaction ids and commit sequence numbers carry the term in their high 16 bits (48-bit counter per term); the WAL carries every `c`; a token absent from the history is `T2003`. | REPL §1.4, §8.3, §10.6 rule 2; ISO §3.6 R3, §4.1; DISK §6.4 (`TXN_COMMIT.commit_seq`, `CHECKPOINT_END.last_commit_seq`), D19; ERR `T2003` |
| P5 | Which synchronous levels? | `off`, `on`, `remote`, `remote_apply`; a quorum `k` of at most 16 named replicas; `synchronous` is an enum on the wire (byte-identical to the bool for `off`/`on`) and an `int32_t` level in the C ABI. No receive-only level. | REPL §6.1–§6.3; ISO §6, §8; WIRE §3.4, §3.5 (`Synchronous`), §5.2 rules 2 and 5, §5.4 rule 1; CAPI §12 header (`TERNDB_SYNC_*`, `terndb_txn_options_set_synchronous`) |
| P6 | A remote commit with no reachable quorum? | Waits without limit by default; optional `synchronous_commit_timeout` reports `T2002` for a transaction that is committed on the primary. | REPL §6.4; ERR `T2002` |
| P7 | Preventing split brain without a consensus service? | Promotion fences `n − k + 1` members of the synchronous set at the new term and must be the most advanced of them; fence term persisted in CONTROL; configuration changes carried by `SYNC_CONFIG` with joint quorums; optional `fence_command`; no primary lease, so a partitioned old primary's `on`/`off` commits are lost. | REPL §6.2 rules 3–4, §10; DISK §2.2 (`fence_term`, `fence_promotion_id`), §6.4 (`SYNC_CONFIG`); ISO §8; ERR `S1008`, `S3001`–`S3004` |
| P8 | What may conflict with replica readers? | Only purge and undo truncation, announced by a new `PURGE_HORIZON` record; DDL replay itself never conflicts. | REPL §7.5, §9 rule 5; ISO §5.6; DISK §5.5, §5.6, §6.4 (kind 44), §6.11 rule 3 |
| P9 | Replica feedback on by default? | Stays opt-in (Q24), and is bounded: clamped by `replica_feedback_undo_limit` and `max_snapshot_age`, dropped on disconnect, O(`max_replicas`) per status at a bounded rate. | REPL §7.6, §12; DISK §5.5 |
| P10 | Seeding a replica? | Base backup over the protocol after a completed checkpoint, pages from files or the buffer pool, torn copies repaired by FPIs, CONTROL synthesized on the replica. | REPL §4 |
| P11 | Rejoining an old primary? | Built-in rewind: fetch from the new primary only the pages the old primary's divergent WAL touched (`StartBackup` mode `pages`), then redo from the last common checkpoint. | REPL §10.5 |
| P12 | Who may replicate? | Superusers and members of a predefined role `terndb_replication`; no new role attribute. | REPL §2.2; DDL §2.10 rule 6; ERR `S1006` |
| P13 | Slots and their bound? | Persistent named slots on the primary (`slots/<name>.tslot`), invalidated past `max_slot_wal_keep` (default 64 GiB). | REPL §5.8; DISK §2 file set; ERR `S2001`–`S2003` |
| P14 | Measuring `max_lag` across nodes? | Echoed probes timed on the replica's own monotonic clock; no clock comparison between nodes. | REPL §5.3, §5.4, §8.4 |
