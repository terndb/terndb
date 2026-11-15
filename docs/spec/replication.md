# TernDB replication

**Status: normative, draft.** This document defines how one TernDB primary streams its write-ahead log to read
replicas: how a replica is seeded, the stream's messages byte for byte, synchronous commit, how a replica applies
the log and serves reads, how DDL reaches it, and how a replica is promoted without accepting a split brain. Code
follows this text; a change to observable behaviour changes this text in the same commit. The key words MUST,
MUST NOT, SHOULD, SHOULD NOT and MAY are used as in RFC 2119. Every MUST is meant to be checked by one of the
invariants of §14.

References used throughout:

- **[ISO]** `isolation.md`. §3.6 fixes what a replica read guarantees (R1–R4, R3a); §6 fixes commit and
  durability; §8 fixes crash semantics.
- **[DISK]** `on-disk-format.md`. §5 the undo log, §6 the WAL, §7 format versioning.
- **[WIRE]** `wire-protocol.md`. Framing (§1), handshake and authentication (§2), the message catalogue (§3).
- **[DDL]** `ddl.md`. The catalog, schema versions and background jobs.
- **[ERR]** `errors.md`. The one registry of error codes; this document's codes are its `S` area and two `T`
  codes.

There is no distribution. A replicated TernDB is one primary, which alone accepts writes, and up to
`max_replicas` replicas, each a byte-for-byte copy of the primary's log applied continuously. Replication ships
the WAL of [DISK] §6 unchanged; there is no second log and no logical decoding.

## 1. Model

### 1.1 Roles

1. **Primary.** Exactly one node of a history accepts writes at a time: the primary of the current **term**
   (§1.2). It assigns transaction ids and commit sequence numbers, writes the WAL and serves it to replicas.
2. **Replica.** A node in CONTROL `state` 3 ([DISK] §2.2). It receives the primary's WAL, applies it and serves
   read-only transactions ([ISO] §3.6). A replica never writes WAL of its own; it writes pages, CONTROL and its
   copy of the received WAL.
3. **Synchronous replica.** A replica named in the primary's **synchronous set** (§6.2). Its acknowledgements
   count towards `synchronous = remote` and `remote_apply` commits. The set has at most 16 names; a commit waits
   for `synchronous_quorum` of them.
4. **Asynchronous replica.** Every other replica. Its acknowledgements count for nothing but WAL retention, flow
   control and feedback.
5. A primary serves at most `max_replicas` replication streams at once (default 16, at most 64). A replica
   streams only from the primary: there is no cascading replication.
6. A replica becomes a primary only by **promotion** (§10.2). A primary becomes a replica only by **planned
   switchover** (§10.3) or **rewind** (§10.5).

The word *promotion* in this document means promoting a replica to primary. It is unrelated to the promotion of a
memory-mode database to disk mode ([DISK] §8.3).

### 1.2 Terms, timelines and histories

1. A **term** is a `u32` in [1, 65,535]. **The timeline id of [DISK] §6 is the term**: every timeline is written
   by one primary during one term, and every WAL segment header carries the value in both `timeline` and `term`
   ([DISK] §6.6), which MUST be equal.
2. A new database starts at term 1. Every timeline switch takes a new term greater than every term this node has
   seen in any history file, segment header or message: promotion (§10.2), point-in-time recovery ([DISK] §6.11),
   and counter exhaustion (§1.4 rule 4). A switch that would need a term above 65,535 is refused with
   `term_exhausted` (`S3003`).
3. A **history** is the sequence of terms a timeline descends from. It is recorded in a **history file**
   `wal/<term:08x>.thist`, written and synced before the first record of the term, never changed afterwards,
   and archived with the segments (§5.7):

   | off | size | field |
   |----:|-----:|-------|
   | 0  | 4  | `crc`: CRC32C over [4, `len`) |
   | 4  | 8  | `magic`: ASCII `TERNHIS1` |
   | 12 | 4  | `len`: 40 + 40 × `n` |
   | 16 | 16 | `db_uuid` |
   | 32 | 4  | `term`: this file's term |
   | 36 | 4  | `n`: the number of entries |
   | 40 | 40 × n | entries, in strictly increasing `term` |

   An entry:

   | off | size | field | meaning |
   |----:|-----:|-------|---------|
   | 0  | 4  | `term` | |
   | 4  | 4  | reserved | |
   | 8  | 8  | `start_lsn` | the first LSN of this term in this history (4,096 for the first term) |
   | 16 | 8  | `end_commit` | the last commit sequence number this term contributed to this history; 0 for the last entry, whose term is still open |
   | 24 | 16 | `promotion_id` | 16 random octets chosen when the term began (§10.2) |

   The last entry is the file's own term. The term of entry `i` covers LSNs [`start_lsn[i]`, `start_lsn[i+1]`).
4. Two histories **agree up to LSN L** when, for every LSN below L, the two histories name the same term and the
   same `promotion_id`. A node MUST NOT accept, store or apply WAL bytes whose history does not agree with its own
   up to the bytes' LSN (§5.1 rule 4, invariant R13).
5. LSNs continue across a switch: the new term's first segment holds the old term's bytes up to the switch LSN,
   copied, with the new term in its header, and then `TIMELINE_SWITCH` ([DISK] §6.4) at the switch LSN.

### 1.3 Positions

Every replica has three WAL positions, each the end (exclusive) of a contiguous prefix of the primary's history:

| position | meaning |
|---|---|
| `received` | bytes written to the replica's WAL files (not necessarily synced) |
| `flushed` | bytes durable in the replica's WAL files |
| `applied` | bytes whose records have been applied: every record wholly below it is applied, and no record at or above it |

`applied ≤ received` and `flushed ≤ received` always hold. Each of them is non-decreasing for the life of a
stream. `applied_commit` is the commit sequence number of the last `TXN_COMMIT` below `applied`: it is the
replica's visible horizon `h` ([ISO] §4.1). On the primary, `durable_lsn` is [DISK] §6.7's, and
`durable_commit` is the commit sequence number of the last `TXN_COMMIT` below it.

### 1.4 Sequence numbers carry the term

1. A transaction id and a commit sequence number `c` ([ISO] §4.1) are each a `u64` whose **high 16 bits are the
   term** of the primary that assigned it and whose low 48 bits are a counter, starting at 1 in each term. So
   both remain strictly increasing across every switch, and a value names the term it came from: `term(x) = x >>
   48`.
2. `TXN_COMMIT` carries its `commit_seq`, and `CHECKPOINT_END` carries `last_commit_seq` ([DISK] §6.4), so that
   any reader of the WAL — a replica that starts from a base backup, a recovery, a promotion — knows `c` of every
   commit it applies.
3. A promotion starts both counters of the new term at `(term << 48) | 1`.
4. A primary whose counter would pass 2⁴⁸ − 1 in its term MUST switch to a new term first, exactly as promotion
   does but with no fencing: steps 1, 2 and 7 of §10.2, with no undo phase, since its transactions stay open
   across the switch.
5. A value whose high 16 bits are 0 is never assigned by a server; clients MUST treat the values as opaque.

*Rationale:* a commit acknowledged by an old primary and then lost by failover would otherwise share its `c` with
a different commit of the new history, and a client's causal token or `transaction_status` query would silently
name the wrong transaction. With the term in the value, the new history can tell that the token names a commit it
does not contain (§8.3).

## 2. Connections and security

### 2.1 Replication connections

1. Replication uses the native protocol of [WIRE] over the same listeners: direct TLS 1.3 with ALPN `terndb` on
   TCP, or a Unix-domain socket, the same preface and the same handshake. The replica, or the node performing a
   backup, rewind or fence, is the client.
2. The messages of this document belong to **wire version 1.1** ([WIRE] §2.2 rule 4). A node MUST NOT send them on
   a connection that negotiated a wire minor below 1, and a server receiving one there answers `W1001` as for any
   unknown type.
3. A connection becomes a **replication connection** when its first request after `Ready` is one of §2.3's
   requests. A replication connection carries no other request (`connection_in_replication_mode`, `S1009`), and a
   connection that has sent any other request first MUST NOT send one of §2.3's (`S1009`). `Ping`, `Pong`,
   `Terminate` and `Goodbye` are allowed on both.
4. At most one `StartReplication` stream is open on a connection at a time (`S1009`).

### 2.2 Authentication and the replication right

1. Authentication is [WIRE] §2.4's, with the same methods. A replica SHOULD use `scram-sha-256-plus` or `token`,
   and MUST verify the server's certificate against its configured trust anchors before sending a credential.
2. A role has the **replication right** if it is a superuser or a member of the predefined role
   `terndb_replication` ([DDL] §2.10). Every request of §2.3 needs it; without it the request fails with
   `replication_not_permitted` (`S1006`) after authentication has succeeded.
3. The replication stream carries every byte of the database. The replication right is therefore read access to
   everything, including catalog rows and data the role has no privilege on.
4. A replica authenticates its own clients against its applied copy of the catalog's roles. Credentials that the
   server keeps outside the database are configuration, which the operator keeps the same on every node.
5. Plaintext TCP is refused as for every connection ([WIRE] §1.1 rule 3). There is no replication-specific
   transport.

### 2.3 Message types

| Type | Name | Direction | Stream | Kind |
|---|---|---|---|---|
| `0x60` | `Identify` | to server | new | request |
| `0x61` | `StartReplication` | to server | new | request |
| `0x62` | `ReplicaStatus` | to server | target | control |
| `0x63` | `FetchHistory` | to server | new | request |
| `0x64` | `StartBackup` | to server | new | request |
| `0x65` | `Fence` | to server | new | request |
| `0xE0` | `Identity` | to client | request | terminal |
| `0xE1` | `WalData` | to client | request | partial |
| `0xE2` | `PrimaryStatus` | to client | request | partial |
| `0xE3` | `ReplicationEnd` | to client | request | terminal |
| `0xE4` | `History` | to client | request | terminal |
| `0xE5` | `BackupBegin` | to client | request | partial |
| `0xE6` | `BackupData` | to client | request | partial |
| `0xE7` | `BackupEnd` | to client | request | terminal |
| `0xE8` | `Fenced` | to client | request | terminal |

1. Every request receives exactly one terminal message, its own or an `Error` ([WIRE] §1.5 rule 2).
2. `ReplicaStatus` names the `StartReplication` stream it reports on, as `Credit` does ([WIRE] §1.5 rule 3). One
   naming a stream that is not an open `StartReplication` stream MUST be ignored.
3. `0x66–0x6F` and `0xE9–0xEF` stay reserved (`W1001`).

### 2.4 Encoding

Grammars use [WIRE] §1.6's primitives, plus `uuid` (16 octets, as stored). The three messages sent continuously
during streaming — `WalData`, `PrimaryStatus`, `ReplicaStatus` — have fixed-width little-endian headers given as
tables, so that both ends parse them without a varint loop; their payloads are exactly the sizes the tables give
([WIRE] §1.4 rule 4).

A receiver refuses with `W1001`, as it does any malformed message: a flag bit that a table marks zero set in
`PrimaryStatus` or `ReplicaStatus`; a `WalData` with no WAL octet, or whose bytes reach past its `durable_lsn`
(§5.2 rule 1); a `BackupData` whose reserved octets are not zero or whose pages are not one or more whole
16,384-octet pages; a `space` other than 0 or 1; a `StartBackup` in `full` mode that lists pages, or in `pages`
mode that lists none; and a `StartReplication` whose `window` is below 1,048,576.

## 3. Identify and history

```
Identify      := (empty)

Identity      := uuid  db_uuid
                 enum  role { primary, replica, fenced }
                 u32   term                       (the current term: the timeline written or being applied)
                 u32   fence_term                 (§10.1)
                 u64   durable_lsn                (primary: durable_lsn; replica: flushed)
                 u64   applied_lsn                (primary: durable_lsn)
                 uvar  wal_format                 ([DISK] §7.1, the format being written or applied)
                 uvar  wal_format_max             (the newest WAL format this release can apply)
                 uvar  format_current
                 str   server_version             (informative)

FetchHistory  := u32 term
History       := bin file                         (the history file of `term`, §1.2 rule 3, byte for byte)
```

1. Any node answers `Identify`, whatever its role. A replica SHOULD identify the server before every
   `StartReplication` and `StartBackup`, and MUST check `db_uuid` against its own (`database_mismatch`, `S1002`).
2. `FetchHistory` returns the history file for any term in the server's own history, or one held in its archive.
   An unknown term is `history_diverged` (`S1003`).

## 4. Base backup

A replica is seeded from a **base backup**, taken from the running primary over a replication connection. The
backup takes no lock, blocks no writer and needs no pause: it is [DISK] §6.11 rule 1's online backup, carried
over the protocol.

### 4.1 Messages

```
StartBackup   := enum mode { full, pages }
                 list<(u8 space, u64 first_page, u64 count)> pages   (empty for full; ranges for pages)
                 str  slot                                           (empty: no slot)
                 bool fast                                           (checkpoint at full speed)

BackupBegin   := uuid db_uuid
                 u32  term
                 u32  generation
                 u64  redo_lsn                   (B: the backup checkpoint's redo start)
                 u64  checkpoint_lsn             (that checkpoint's CHECKPOINT_END)
                 u64  undo_low
                 u16  format_current, u16 format_min
                 uvar wal_format
                 list<(u8 space, u64 segment_no, u32 page_count)> files

BackupEnd     := u64 stop_lsn
                 u64 stop_commit                 (durable_commit at stop_lsn)
```

`space` is 0 for the data space and 1 for the undo space ([DISK] §2.1). `BackupData` has a fixed header:

| off | size | field |
|----:|-----:|-------|
| 0  | 1 | `space` |
| 1  | 7 | reserved, zero |
| 8  | 8 | `segment_no` |
| 16 | 8 | `first_page`: the page index within the segment |
| 24 | 16,384 × m | `m` ≥ 1 whole pages, consecutive from `first_page` |

### 4.2 Procedure on the primary

1. If `slot` is not empty, the slot (§5.8) is created if absent, and its `restart_lsn` is set to B in step 3
   before any page is sent. `slot_in_use` (`S2002`) if another stream holds it.
2. Request a checkpoint ([DISK] §6.8), spread unless `fast`, and wait until it is **complete**. Let B be its
   `redo_lsn`. WAL at and above B is retained (as for [DISK] §6.6's "running backup") until `BackupEnd` is sent,
   and thereafter by the slot if one was named.
3. Send `BackupBegin`, listing every data and undo segment that exists, with its length in pages.
4. Send every page of every listed segment (mode `full`), or every page in the requested ranges (mode `pages`,
   §10.5), as `BackupData` messages, in any order. A `BackupData` message SHOULD carry at most 64 pages. A page MAY
   be read from its segment file, where it may be torn by a concurrent write, or copied from the buffer pool, where
   the copy MUST be one whole version (taken under the page's optimistic latch) and MUST NOT be sent until
   `durable_lsn` ≥ its `page_lsn`. A segment deleted while the backup runs is skipped from the point of deletion;
   pages past a segment's listed length are not sent.
5. Send `BackupEnd` with `stop_lsn` = `durable_lsn` after the last page was sent.

*Why a torn copy is harmless:* copying starts only after the checkpoint at B is complete, so every page write that
can overlap the copy is of a page modified at or after B, and the first such modification logged an `FPI` or a
`PAGE_INIT` at or after B ([DISK] §6.5 rule 3). Redo from B therefore rewrites every page that could have been
copied torn, exactly as crash recovery does (D11). A page that is not written during the copy cannot be torn.

Undo pages are copied the same way. Purge and undo truncation keep running: an undo segment deleted during the copy
is deleted on the replica by the `UNDO_TRUNCATE` and `FILE_DELETE` records that follow B. No snapshot of the
primary is taken or held by the backup.

### 4.3 Procedure on the replica

1. Create an empty directory, write each received page at its place in `data.<generation>/` or
   `undo.<generation>/`, and extend each listed segment to its listed length with zero pages where no page was
   received.
2. Write CONTROL ([DISK] §2.2) with `db_uuid`, `generation`, the formats, `timeline` = `term`, `checkpoint_lsn`,
   `redo_lsn` = B, `undo_low`, `min_recovery_lsn` = `stop_lsn`, `fence_term` = `term`, `state` = 3, and sync it
   and every written file. CONTROL is synthesized, never copied.
3. Fetch the history file of `term` (§3) into `wal/`.
4. Start streaming at B (§5.1) on the same slot. Redo starts at the checkpoint's `redo_lsn` as in [DISK] §6.9;
   the analysis phase loads the checkpoint's transaction table from `CHECKPOINT_END`.
5. The replica is **consistent** once `applied` ≥ `min_recovery_lsn`. Before then it MUST NOT accept a
   transaction; a `Begin` is refused with `replica_lag_timeout` after `replica_wait_timeout`.

A transaction that ended before B appears in no record the replica applies. Its versions are visible to every
replica snapshot, because every replica snapshot is at least `stop_commit` and that transaction's `c` is at most
`last_commit_seq` of the checkpoint at B (§7.3 rule 2).

## 5. Streaming

### 5.1 StartReplication

```
StartReplication := uuid db_uuid
                    u32  term                    (the term of the history the replica holds)
                    u32  fence_term              (the replica's, §10.1)
                    u64  start_lsn               (where the primary starts sending)
                    str  replica_name            (matched against the synchronous set; MAY be empty)
                    str  slot                    (empty: no slot)
                    bool create_slot
                    u64  window                  (§5.5; at least 1,048,576)
                    uvar wal_format_max
```

The primary checks, in order, and fails the request with the first that applies:

1. it is not the primary of its term, or is fenced: `not_primary` (`S1001`);
2. `db_uuid` differs: `database_mismatch` (`S1002`);
3. `fence_term` exceeds the primary's term: the primary is stale. It MUST fence itself (§10.4) and answer
   `not_primary`;
4. the replica's history does not agree with the primary's up to `start_lsn` (§1.2 rule 4): the replica's
   `term` is not in the primary's history, or `start_lsn` lies beyond the end of `term` in the primary's
   history: `history_diverged` (`S1003`);
5. the WAL at `start_lsn` is neither in `wal/` nor in `archive/`: `wal_unavailable` (`S1004`);
6. the primary's `wal_format` exceeds `wal_format_max`: `wal_format_unsupported` (`S1005`);
7. the slot is unknown and `create_slot` is false: `unknown_slot` (`S2001`); held by another stream:
   `slot_in_use` (`S2002`); invalidated: `slot_invalidated` (`S2003`);
8. `max_replicas` streams are open: `too_many_replicas` (`S1007`), retryable.

Otherwise the stream is open: the primary sends `WalData` from `start_lsn` and `PrimaryStatus` messages until it
ends the stream (§5.9). A replica chooses `start_lsn` = its `flushed`, or B after a base backup. It MAY choose
its `received` when it kept the unsynced bytes; it MUST NOT choose a value above `received`.

### 5.2 WalData

| off | size | field | meaning |
|----:|-----:|-------|---------|
| 0  | 8 | `start_lsn` | the LSN of the first WAL byte carried |
| 8  | 8 | `durable_lsn` | the primary's `durable_lsn` when sent; at least `start_lsn` + n |
| 16 | 8 | `send_time` | `i64` µs since the epoch, the primary's wall clock (informative) |
| 24 | 4 | `timeline` | the term whose WAL the bytes are |
| 28 | 4 | `term` | the primary's current term |
| 32 | n | WAL bytes | n ≥ 1, the bytes [`start_lsn`, `start_lsn` + n) of the primary's history, as stored in its segments |

1. **The primary MUST send only WAL that is durable** ([ISO] §3.6 R4): `start_lsn` + n ≤ `durable_lsn`. It
   SHOULD send newly durable WAL as soon as `durable_lsn` advances, without waiting for more.
2. Successive `WalData` messages of a stream are contiguous: each `start_lsn` equals the previous one's end.
3. One message carries bytes of one segment and one `timeline` only. The bytes are copied from the segment files
   or the WAL buffer and are never re-encoded: they include the segment's PAD records, and the 4,096-octet
   segment header is never sent (the replica writes its own, §7.1).
4. A replica MUST refuse a message whose `start_lsn` is not its `received`, whose `term` is below its
   `fence_term`, or whose `timeline` disagrees with its history at `start_lsn` (§1.2 rule 4), by closing the
   connection; these are protocol violations and the replica logs them as `history_diverged`.

### 5.3 PrimaryStatus

| off | size | field | meaning |
|----:|-----:|-------|---------|
| 0  | 8 | `durable_lsn` | |
| 8  | 8 | `durable_commit` | the last commit sequence number below `durable_lsn` |
| 16 | 8 | `send_time` | `i64` µs, the primary's wall clock (informative) |
| 24 | 4 | `term` | the primary's current term |
| 28 | 4 | `flags` | bit 0 `REPLY_REQUESTED`; bit 1 `SYNCHRONOUS`: this stream counts for synchronous commit (§6.2); bit 2 `FEEDBACK_CLAMPED` (§7.6); others zero |
| 32 | 8 | `echo` | the highest `probe` received on this stream (§8.4), or 0 |
| 40 | 8 | `horizon` | the horizon of the last `PURGE_HORIZON` written (informative) |

1. The primary MUST send a `PrimaryStatus` within `status_interval` of the last message it sent on the stream,
   and at once (before any further `WalData`) after receiving a `ReplicaStatus` with a new non-zero `probe`.
2. The fields describe the moment of sending, after every `ReplicaStatus` already received was processed.

### 5.4 ReplicaStatus

| off | size | field | meaning |
|----:|-----:|-------|---------|
| 0  | 8 | `received` | §1.3 |
| 8  | 8 | `flushed` | |
| 16 | 8 | `applied` | |
| 24 | 8 | `applied_commit` | the replica's visible horizon `h` |
| 32 | 8 | `oldest_snapshot` | feedback (§7.6): the oldest live replica snapshot, or `applied_commit` if none; 0 when feedback is off |
| 40 | 8 | `probe` | 0, or a value greater than every earlier `probe` on this stream (§8.4) |
| 48 | 8 | `send_time` | `i64` µs, the replica's wall clock (informative) |
| 56 | 4 | `fence_term` | the replica's, §10.1 |
| 60 | 4 | `flags` | bit 0 `REPLY_REQUESTED`; others zero |

1. The replica MUST send a `ReplicaStatus` within `status_interval` of the last one; at once when the primary set
   `REPLY_REQUESTED`; whenever `received` has advanced by `window`/2 since the last report; and, while the stream
   is `SYNCHRONOUS`, after every flush that advanced `flushed` and after every apply that advanced
   `applied_commit`.
2. A replica MUST NOT report `flushed` above what is durable in its WAL files, nor `applied` above what its
   readers can see.
3. Every field but `probe`, `send_time` and `flags` is non-decreasing on a stream. The primary closes a stream
   whose positions decrease, or exceed what it has sent, with `W1001`.
4. A `fence_term` above the primary's term fences the primary (§10.4).

### 5.5 Flow control

1. The primary MUST NOT send a `WalData` while its end of sent bytes exceeds the replica's last reported
   `received` by `window` or more. It MAY send one message that takes the excess past `window`.
2. A replica that cannot keep up stops reporting `received` and the stream pauses; TCP back-pressure acts below
   this. Nothing in the protocol drops WAL: a paused stream holds WAL on the primary only through its slot (§5.8).
3. `window` is fixed for the life of the stream. A replica SHOULD choose at least the WAL produced in 2 × its
   round-trip time at the primary's peak rate.

### 5.6 Timeline boundaries

When the primary's history switches term at LSN S below the bytes being sent, the stream carries the old term's
bytes up to S with `timeline` = the old term, then the new term's bytes from S, beginning with the segment copy
and `TIMELINE_SWITCH` of §1.2 rule 5. Before it writes the first byte of a new `timeline`, the replica MUST fetch
that term's history file (§3) if it does not hold it, and MUST check that the file's entries agree with its own
history up to S.

### 5.7 Catch-up from archived segments

1. The primary's archiver copies each completed segment, and each history file, into `archive/` ([DISK] §2) and
   syncs it before the segment may be recycled.
2. A `StartReplication` whose `start_lsn` lies in a segment no longer in `wal/` is served from `archive/`
   transparently, at the speed the window allows.
3. A replica MAY be configured with a `restore_source`: a directory holding archived segments and history files
   (for example a mounted copy of the archive). Before it streams, and whenever the primary answers
   `wal_unavailable`, a replica reads every segment it needs from `restore_source`, checking each header's
   `db_uuid`, `timeline`, `term` and `first_lsn`, and the chain of `prev_segment_crc`, and applies it as if
   streamed. It then streams from the end of what it read.
4. If neither the primary nor `restore_source` has the WAL a replica needs, the replica cannot catch up and MUST
   be re-seeded by a base backup (§4).

### 5.8 Slots and retention

1. A **slot** is a named, persistent claim on the primary's WAL. Its `restart_lsn` is the `flushed` most
   recently reported on a stream holding it (or B for a backup, §4.2), and no segment at or above it may be
   recycled ([DISK] §6.6).
2. Slots are files `slots/<name>.tslot` in the database directory, of 4,096 octets:

   | off | size | field |
   |----:|-----:|-------|
   | 0  | 4  | `crc`: CRC32C over [4, 4096) |
   | 4  | 8  | `magic`: ASCII `TERNSLT1` |
   | 12 | 1  | `name_len` (1–63); the name is `[a-z0-9_]`, and the file name repeats it |
   | 13 | 63 | `name`, zero-padded |
   | 76 | 4  | `flags`: bit 0 `INVALIDATED` |
   | 80 | 8  | `restart_lsn` |
   | 88 | 4  | `restart_term` |
   | 92 | 4004 | reserved |

   A slot file is written to `slots/<name>.tmp`, synced, renamed over the old one and the directory synced. The
   primary rewrites each slot at every checkpoint and when it is created or dropped; after a crash a slot's
   `restart_lsn` may be older than the last report, which only retains more.
3. A slot whose `restart_lsn` falls more than `max_slot_wal_keep` (default 64 GiB; 0 means no limit) below
   `durable_lsn` is **invalidated**: the flag is set, it retains nothing, and every later `StartReplication` on it
   fails with `slot_invalidated` (`S2003`). A slot is dropped by the administrator.
4. A stream with no slot retains WAL only while it is connected. A slot is held by at most one stream.
5. Slots are local to the primary. A promoted replica starts with no slots; replicas that reconnect to it with
   `create_slot` recreate theirs.

### 5.9 Ending a stream

1. A stream ends with `ReplicationEnd`, with an `Error`, or when the connection closes.
   ```
   ReplicationEnd := u32 term, u64 end_lsn          (the last byte of `term` that this primary will write)
   ```
2. The primary sends `ReplicationEnd` only in a planned switchover (§10.3), after it has sent every byte below
   `end_lsn`. It is the one case in which a primary stops serving its term without being fenced.
3. A replica whose stream ends reconnects with exponential back-off from 100 ms to 10 s, re-identifies the server
   (§3) and re-checks history before streaming again.

## 6. Synchronous commit

### 6.1 Levels

`synchronous` ([ISO] §6) has four levels. Each includes the guarantees of the one before it:

| level | a writing transaction's commit is acknowledged once | survives |
|---|---|---|
| `off` | its commit record is published | nothing: a primary crash may lose it, with every later commit ([ISO] §8) |
| `on` (default) | the WAL is durable on the primary through its commit record | a crash of the primary |
| `remote` | also durable (`flushed`) through it on `synchronous_quorum` members of the synchronous set | a crash of the primary followed by promotion (§10) |
| `remote_apply` | also applied (`applied`) through it on `synchronous_quorum` members | the same, and the commit is visible on those members at acknowledgement |

1. A read-only transaction at a level is acknowledged once its snapshot `s` meets the level's condition with
   `s`'s commit record in place of its own, so that no acknowledged transaction depends on a commit the level does
   not protect ([ISO] §6).
2. A commit is published at every level before it is acknowledged ([ISO] §4.1), and MAY be published before it is
   durable anywhere. A transaction that saw an unprotected commit cannot be acknowledged at a level before that
   commit meets the level, because its own commit record or snapshot follows it in the WAL.
3. `remote` and `remote_apply` with an empty synchronous set behave as `on`.

### 6.2 The synchronous set and quorum

1. `synchronous_replicas` is an ordered list of at most 16 replica names; `synchronous_quorum` is `k`, with
   1 ≤ `k` ≤ `n`, the list's length. The pair is the **synchronous configuration**.
2. A stream is `SYNCHRONOUS` when its `replica_name` is in the list, it streams the primary's current term, and
   its authenticated role has the replication right. Two streams presenting the same name: the newer one is
   refused with `slot_in_use` (`S2002`).
3. Every synchronous configuration is written to the WAL as a `SYNC_CONFIG` record ([DISK] §6.4): at startup
   when it differs from the newest one in the WAL, and on every change.
4. **Changing the configuration.** From the moment the record for a new configuration `(S', k')` is written until
   it is `flushed` on `k` members of the old `(S, k)` and on `k'` members of `S'`, a commit at `remote` needs both
   quorums. Afterwards it needs only the new one. This is what lets promotion trust the configuration it reads
   from its own WAL (§10.2 step 3).

### 6.3 What a commit waits for

1. Let `q_flush` be the `k`-th largest `flushed` among the `SYNCHRONOUS` streams, and `q_apply` the `k`-th largest
   `applied` (0 when fewer than `k` streams are `SYNCHRONOUS`). A commit at `remote` whose commit record ends at
   LSN E is acknowledged once `durable_lsn` ≥ E and `q_flush` ≥ E; at `remote_apply` once `durable_lsn` ≥ E and
   `q_apply` ≥ E.
2. **Group commit is unchanged** ([DISK] §6.7). The flusher never waits for replicas. Remote waiters sit in one
   queue ordered by E; every `ReplicaStatus` from a `SYNCHRONOUS` stream recomputes `q_flush` and `q_apply` (at
   most 16 values) and releases every waiter at the head of the queue whose E it now covers, in one pass.
3. A replica that is `SYNCHRONOUS` flushes received WAL as soon as it arrives and no flush is in progress (a
   group flush of its own), and reports at once (§5.4 rule 1). So a `remote` commit costs one primary fsync, one
   network round trip and one replica fsync, in sequence, shared by every commit of the group.
4. A remote waiter whose transaction has been published but not acknowledged is committed. A client that loses
   the connection while waiting resolves the outcome by `transaction_status` ([ISO] §5.4), which answers
   `committed(c)`.

### 6.4 When no quorum is reachable

1. A commit at `remote` or `remote_apply` waits for its quorum without limit while `synchronous_commit_timeout`
   is 0 (the default).
2. With a non-zero `synchronous_commit_timeout`, a commit that has waited that long fails with
   `commit_not_confirmed` (`T2002`). Its transaction **is committed**: it is durable on the primary and visible,
   and `transaction_status` answers `committed(c)`. The code says only that the requested level was not reached.
   It is not retryable.
3. When the primary is fenced (§10.4), every remote waiter fails with `commit_unknown` (`T2001`), because the new
   history may or may not contain it.

## 7. Apply on the replica

### 7.1 Writing and flushing received WAL

1. The replica writes every received byte at its LSN into `wal/<timeline:08x>-<first lsn:016x>.twal`, the same
   names and layout as the primary ([DISK] §6.6). When it creates a segment it writes the header itself, with the
   primary's `db_uuid`, `wal_format`, `timeline` = `term` = the message's `timeline`, `first_lsn`, and
   `prev_segment_crc` of its own previous segment header of this history.
2. A replica flushes its WAL files under the same rules as a primary ([DISK] §6.7 rules 3–4, D14): a failed sync
   is fatal, and the replica stops and recovers.
3. A replica retains its WAL from its CONTROL `redo_lsn` (§7.7). It MAY archive what it receives into its own
   `archive/`.

### 7.2 Redo

1. Apply is [DISK] §6.9's redo, run continuously over the received WAL: the same dispatch by `pid`, the same
   barrier for `FILE_*` records, the same per-page rule (apply iff `page_lsn` < the record's `lsn`), and the same
   corruption rule, which stops apply with `data_corrupted` (`D1001`).
2. **Apply never runs past `received`** and, because only durable WAL is sent, never applies WAL that is not
   durable on the primary (R4).
3. A mini-transaction is applied only once its last record has been received ([DISK] §6.3 rule 2).
4. **A replica never runs the undo phase.** A transaction open on the primary stays open on the replica until its
   `TXN_COMMIT` or `TXN_ABORT` arrives; its rollback, if any, arrives as the primary's `COMPENSATION` records.
   Its rows stay write-locked by its `txn` in the meantime, and replica readers read past them through undo.
5. Records with special effects on a replica:

   | record | effect |
   |---|---|
   | `TXN_COMMIT` | publishes the commit (§7.3) |
   | `TXN_ABORT` | removes the transaction from the open set |
   | `CHECKPOINT_END` | allows a restartpoint (§7.7) |
   | `PURGE_HORIZON` | a conflict barrier (§7.5) |
   | `SYNC_CONFIG` | recorded as the newest synchronous configuration, for promotion |
   | `TIMELINE_SWITCH` | moves the replica to the new term; checked against the history file |
   | `FORMAT_UPGRADE` | if `to` is above `wal_format_max` or the page formats this release reads, apply stops before the record (§11.3) |

   Every other record is applied as recovery applies it.

### 7.3 Publication and the commit map

1. The replica keeps a **commit map** from transaction id to commit sequence number, filled from the `txn` and
   `commit_seq` of every `TXN_COMMIT` it applies, and the set of transactions open since its start checkpoint
   (from `CHECKPOINT_END`'s table and from every record whose header names a `txn` not yet seen).
2. A replica snapshot `s` sees transaction T iff T is in the commit map with `c ≤ s`, or T is neither in the map
   nor open and T < `next_txn_id` of the replica's start checkpoint: such a T ended before the checkpoint, and if it
   committed its `c` is at most the checkpoint's `last_commit_seq`, which is below every replica snapshot (§4.3
   rule 5). A rolled-back T leaves no version naming it. Entries below the horizon of the last applied
   `PURGE_HORIZON` MAY be dropped from the map, since every live replica snapshot then sees them (§7.5).
3. **Publication.** Applying a `TXN_COMMIT` with `commit_seq` c at LSN L publishes c: `applied_commit` becomes c
   only once every record below L has been applied by its worker. Commits are therefore published in `c` order
   ([ISO] §4.1), and a replica snapshot taken as `s = applied_commit` sees exactly a prefix of the primary's commit
   order (R1).

### 7.4 Readers during apply

1. Replica readers follow [ISO] §4.1 and [DISK] §5.4 unchanged: they read the in-place version and walk the
   version chain through undo pages that redo produced. Redo rebuilds exactly the primary's undo chains, so a
   replica snapshot `s` reads the versions a primary snapshot `s` read (R7).
2. **A reader never observes part of a mini-transaction.** Apply takes the exclusive latch of every page of a
   mini-transaction before any of its records is applied, and releases them after the last, as the primary did
   when it logged it ([DISK] §6.3 rule 3). Readers latch optimistically and retry, as on the primary.
3. Page latches are the only synchronisation between apply and readers. Apply never waits for a reader except at
   a purge barrier (§7.5).

### 7.5 Purge barriers and conflicts

1. **The only replay that can remove data a replica snapshot needs is purge and undo truncation** ([DISK] §5.5,
   §5.6): removing a ghost row, freeing an overflow chain or a dropped tree, removing a delete-marked index entry,
   or deleting undo below `undo_low`. Row changes never conflict, because the replica rebuilds the undo chains
   that old snapshots read.
2. Before purge or truncation on the primary acts on any record of a transaction with commit `c`, the primary MUST
   have logged a `PURGE_HORIZON` record with `horizon` ≥ `c` ([DISK] §6.4, §5.6). `horizon` never decreases and
   never exceeds the primary's horizon, and every commit record with `c ≤ horizon` precedes it in the WAL.
3. On applying `PURGE_HORIZON(h)`, the live replica transactions with snapshot `s < h` **conflict**. Apply waits
   until none of them remains, or until `max_replay_delay` (default 30 s; `infinite` waits without limit; 0 does
   not wait) has passed since it reached the record. It then aborts every one still live with
   `replica_conflict` (`T1006`, `details.reason = "purge"`) and continues.
4. A snapshot taken while apply waits at a barrier has `s = applied_commit ≥ h` and does not conflict with it, so
   a barrier cannot starve.
5. A replica conflict is the only reason apply waits for a reader; the primary never waits for a replica's
   readers unless feedback asks it to (§7.6).

### 7.6 Feedback

Feedback is opt-in (`replica_feedback`, default off; [DISK] §5.5). With it on:

1. The replica reports `oldest_snapshot` in every `ReplicaStatus`. The primary's horizon is the minimum of its own
   and the `oldest_snapshot` of every connected stream with feedback, subject to rule 3. Feedback holds back
   purge and `undo_low`, and therefore the `PURGE_HORIZON` that would have conflicted.
2. **Correctness never depends on feedback.** A missing, late or wrong feedback value only changes how many
   replica transactions conflict at §7.5's barriers. A replica snapshot taken after a report may be older than the
   report claimed only if it is at least `applied_commit`, which the barrier rule already covers.
3. **The primary bounds what feedback costs it.**
   - *Undo:* a feedback value is honoured only while the undo held above `undo_low` stays within
     `replica_feedback_undo_limit` (default half of `undo_retention_limit`) and the snapshot it holds is no older
     than `max_snapshot_age` ([ISO] §5.5). Beyond either, the primary ignores that stream's value, sets
     `FEEDBACK_CLAMPED` on it, and the replica's old snapshots conflict at the next barrier.
   - *Work:* a `ReplicaStatus` costs the primary O(1) work plus one recomputation of a minimum over at most
     `max_replicas` (≤ 64) values. A stream sends at most one status per `status_interval` (default 1 s, at least
     10 ms) plus one per `window`/2 octets received, plus, if `SYNCHRONOUS`, one per flush and one per applied
     commit group; the primary MAY ignore statuses arriving faster than one per 1 ms on one stream, except
     those that advance `flushed` or `applied` of a `SYNCHRONOUS` stream.
   - *Connection:* a stream's feedback ends when the stream does. A disconnected replica holds nothing back.
4. Without feedback, `oldest_snapshot` is 0 and the primary ignores it.

### 7.7 Restartpoints, restart and the consistency point

1. On applying a `CHECKPOINT_END` whose checkpoint began at B, a replica MAY take a **restartpoint**: write every
   page dirty with `page_lsn` < B, sync the segments, then write CONTROL with `checkpoint_lsn` = that record's LSN,
   `redo_lsn` = B and the checkpoint's counters. The primary's FPIs are relative to its own checkpoints, so redo
   from a restartpoint repairs a torn page exactly as on the primary.
2. **WAL before data on the replica.** A replica MUST NOT write a page to its segment until its own WAL is
   `flushed` through the page's `page_lsn`, and MUST first raise CONTROL `min_recovery_lsn` to at least that
   `page_lsn` (in batches; one CONTROL sync MAY cover many pages).
3. On restart, a replica recovers by redo from its CONTROL `redo_lsn` over its own WAL, with no undo phase, and is
   consistent again only once `applied` ≥ `min_recovery_lsn`. It then streams from its `flushed`.
4. A replica's first snapshot after a restart MAY be older than one it served before ([ISO] §8); R2 is per
   connection.

## 8. Reads on replicas

### 8.1 What a replica offers

1. A replica runs read-only transactions only. Every write, DDL, `next_value` and explicit `read_only = false` is
   refused with `read_only_transaction` (`T4001`).
2. Every replica transaction is serializable, whatever it asks for, and `TxnBegun.effective` says so ([WIRE] §5.2
   rule 3). It never aborts with `serialization_failure` or `schema_changed`; it MAY abort with `replica_conflict`
   (§7.5) and with `resource_exhausted` under the replica's own `max_snapshot_age` ([ISO] §5.5).
3. The guarantees are [ISO] §3.6's R1 (consistent prefix), R2 (monotonic per connection), R3 (causal token), R3a
   (bounded staleness) and R4 (no unflushed data). A read-committed transaction takes `applied_commit` per
   statement; the others take it at begin.
4. `Ready.horizon` and `TxnBegun.snapshot` on a replica report its `applied_commit` at that moment.

### 8.2 Read-your-writes through a commit token

1. A client carries a **token**: the largest `commit` from a `TxnCommitted` or `Complete`, or `snapshot` from a
   `TxnBegun`, it has observed on any node. Passing it as `TxnOptions.after` gives read-your-writes and monotonic
   reads across the primary and every replica (R3).
2. `remote_apply` with `synchronous_quorum` = `n` makes every commit visible on every synchronous replica before
   it is acknowledged, so a read there sees it without a token. Any other configuration needs the token.

### 8.3 Checking a token against the history

On a `Begin` with `after = n`, every server — primary or replica — first classifies `n` by its term `t = n >> 48`
against its own history:

| case | action |
|---|---|
| `t` is the server's current term | wait until `applied_commit` ≥ `n` (on a primary, its visible horizon) |
| `t` is above the current term | wait: the server has not yet reached the switch to `t`, and §5.6 will bring it |
| `t` is an earlier term in the history, and `n` ≤ its `end_commit` | satisfied at once: the server has applied past the end of `t` |
| `t` is an earlier term, and `n` > its `end_commit`; or `t` is below the current term and absent from the history | fail with `token_not_in_history` (`T2003`) |

Waits end with `replica_lag_timeout` (`T1005`) after `replica_wait_timeout`. `token_not_in_history` means that
the commit the token names, or one the client observed, was lost by a failover (§10.6); it is not retryable, and the
client knows that what it wrote or read may be gone.

### 8.4 Bounded staleness

`TxnOptions.max_lag = t` ([ISO] §3.6 R3a) asks for a snapshot including every commit the primary had made durable
at least `t` before the `Begin`. A replica proves it without comparing clocks:

1. Each `ReplicaStatus` MAY carry a new `probe`; the replica records the local monotonic time `q` at which it sent
   it. The primary answers with a `PrimaryStatus` whose `echo` is that probe and whose `durable_commit` is `d`
   (§5.3 rule 1). Every commit the primary had made durable by time `q` has `c ≤ d`, because the primary read
   `d` after the probe arrived.
2. For a `Begin` at local time `T`, the replica takes the newest echoed probe with `q ≥ T − t`. If there is one, it
   waits until `applied_commit ≥ d`. If there is none, it sends a probe at once and waits for its echo, then for
   `applied_commit ≥ d`.
3. The wait ends with `replica_lag_timeout` after `replica_wait_timeout`. A replica with no stream to the primary
   cannot satisfy `max_lag` and times out.
4. A replica SHOULD send a probe in every periodic `ReplicaStatus`, so that a `max_lag` of at least
   `status_interval` plus one round trip is normally met without an extra round trip.
5. On a primary, `max_lag` is always satisfied at once.

### 8.5 Transaction status on a replica

`TransactionStatus` on a replica answers from its applied WAL: `committed(c)` once the commit is applied, `aborted`
once the abort is applied, `in_progress` for a transaction it has seen open or whose id is of the current or a
later term and above every id it has applied, and `status_horizon_passed` (`W2010`) for an id below its start
checkpoint's `next_txn_id` that it cannot classify. A client resolving an unknown outcome SHOULD ask the primary.

## 9. DDL on replicas

1. The catalog is versioned under MVCC like data ([ISO] §5.6), and DDL reaches a replica as the ordinary page,
   undo and commit records of the transaction that ran it. No WAL record kind is specific to DDL ([DDL] §7 rule 4).
2. A replica statement reads the catalog at its snapshot, exactly as on the primary. Because a replica
   transaction never writes, no DDL committed during its life can abort it with `schema_changed`.
3. A `TXN_COMMIT` whose transaction wrote any catalog tree carries the `CATALOG` flag ([DISK] §6.4). On applying
   one, the replica sets its catalog version ([DDL] §1 rule 3) to its `c` and invalidates every plan cache entry
   keyed on an older version. `Prepared.catalog_version` and `Ready.catalog_version` on a replica report it.
4. Background jobs do not run on a replica; their batches arrive as ordinary transactions ([DDL] §7 rule 8). An
   index is used by a replica statement only if its catalog `state` at the statement's snapshot is ready, so the
   redo-only entries of a job still building it are never read.
5. Dropping or rewriting a structure is safe for replica readers until purge frees it: a dropped table's or a
   superseded rebuild's tree is freed only by purge ([DISK] §5.6), below a `PURGE_HORIZON`. **Every DDL conflict
   on a replica is therefore a purge barrier conflict** (§7.5), and a replica transaction that was reading the old
   structure is aborted with `replica_conflict` only if it outlives `max_replay_delay` after that barrier.
6. Roles and privileges are catalog rows: a replica checks privileges against its snapshot's catalog, like the
   primary, and a `Revoke` applied on the replica does not abort a running replica transaction.

## 10. Failover

### 10.1 Fencing state

1. Every node keeps its **fence term** and **fence promotion id** in CONTROL ([DISK] §2.2): the highest term it
   has accepted to exist, and the `promotion_id` that term was created with. They are raised and synced before
   any message that depends on them is sent.
2. A node with fence term F:
   - accepts and applies no WAL of a term below F, except bytes its history already holds below the switch to F;
   - sends no `ReplicaStatus` to a primary whose term is below F, other than one closing the stream with
     `fence_term` = F;
   - if it is a primary whose term is below F, it is **fenced** (§10.4).

### 10.2 Promotion

Promotion is started by the administrator on one replica, the **candidate** (`terndb promote`, or the equivalent
administrative call). A node that is not a replica refuses with `not_a_replica` (`S3004`). The candidate:

1. Chooses the new term `T` = 1 + the largest term it knows (its fence term and every term in its history files),
   and 16 random octets as the `promotion_id`. `term_exhausted` (`S3003`) if `T` > 65,535.
2. Raises its own fence term to `T` with that `promotion_id` and syncs CONTROL. From here it accepts nothing from
   the old term.
3. **Fences the synchronous set.** Let `(S, k)` be the newest `SYNC_CONFIG` in its WAL, and `(S₀, k₀)` the one
   before it if the newest is not yet known to be in effect (§6.2 rule 4). For each such configuration it sends
   `Fence` to the members of `S` (at the addresses of its `replication_peers` setting), counting itself if it is
   a member, and needs `Fenced` from at least `n − k + 1` of them within `promotion_timeout` (default 30 s); else
   it stops with `fence_quorum_unreached` (`S3001`). An empty set needs no fence.
   ```
   Fence  := u32 term, uuid promotion_id, str candidate
   Fenced := u32 fence_term, uuid promotion_id, u32 timeline, u64 flushed, u64 applied
   ```
   A node receiving `Fence` with `term` above its fence term raises its fence term to it (and syncs CONTROL),
   stops acknowledging any primary of a lower term, and answers `Fenced`. With an equal `term` and the same
   `promotion_id` it answers `Fenced` again. Otherwise it answers `stale_term` (`S1008`) with `details.fence_term`,
   and the candidate MAY restart from step 1 with a higher term.
4. **Checks that it is the most advanced.** If any `Fenced` reports a `flushed` beyond the candidate's on the same
   history, the candidate stops with `not_most_advanced` (`S3002`, naming that member in `details.member`); the
   administrator promotes that member instead.
5. Applies every byte it has received (§7.2), ending at E, the end of the last complete mini-transaction; any
   trailing partial mini-transaction is discarded.
6. If `fence_command` is configured, runs it with the old primary's name, and stops with
   `fence_quorum_unreached` if it does not succeed.
7. Writes the history file of `T` (its own history plus an entry `(T, E, 0, promotion_id)`, and the previous last
   entry's `end_commit` set to the last `c` below E), the first segment of `T` and `TIMELINE_SWITCH` at E; runs the
   undo phase of [DISK] §6.9 for every transaction still open, in the background; takes a checkpoint; and writes
   CONTROL with `state` 1 and `timeline` `T`. New transactions start at once, with both counters at
   `(T << 48) | 1` (§1.4).

`terndb promote --force` skips steps 3, 4 and 6. It MAY lose commits acknowledged at `remote` and MAY leave two
primaries accepting writes; it exists for the case in which the old primary and its synchronous set are known to
be gone.

*Why an acknowledged `remote` commit survives:* when it was acknowledged, `k` members of `S` had flushed it, each
before it was fenced. Step 3 fences `n − k + 1` members, so at least one fenced member had flushed it, and step 4
makes the candidate's `flushed` at least that member's. *Why the old primary cannot acknowledge another `remote`
commit:* at most `k − 1` members remain unfenced, and a fenced member refuses to acknowledge the old term (§10.1).

### 10.3 Planned switchover

1. The administrator asks the primary to hand over to a named replica R (`terndb switchover <R>`). The primary
   stops accepting new transactions (in-flight ones run to their commit or are cancelled after a grace period, as
   `Goodbye` does), waits until R reports `flushed` and `applied` at its `durable_lsn`, raises its own fence term
   to `T` = its term + 1 and becomes **fenced**, and sends `ReplicationEnd(term, end_lsn = durable_lsn)` to every
   stream.
2. R then promotes with steps 1–7 of §10.2, using `T` and no fence of the old primary.
3. Because the old primary's WAL ends exactly where R's new term begins, it rejoins as a replica of R without
   rewind: it rewrites CONTROL with `state` 3 and streams from its `durable_lsn` (§5.1).

### 10.4 How an old primary is fenced

A primary fences itself the moment it learns of a higher term, from any of: a `StartReplication.fence_term` or
`ReplicaStatus.fence_term` (§5.1, §5.4), a `Fence` request, or an `Identity` of another node it is configured to
check. A fenced primary:

1. raises and syncs its fence term;
2. accepts no commit: remote waiters fail with `commit_unknown` (`T2001`, §6.4), every new commit request with
   `not_primary` (`S1001`), and so does every new transaction, read-only ones included, because its state may hold
   commits the new history lacks;
3. ends every replication stream with `not_primary`;
4. reports `role = fenced` in `Identity`, and remains fenced until it is rewound (§10.5) or the administrator
   re-seeds it.

Commits it acknowledged at `on` or `off` before it learned are lost (§10.6). No replica of the new term ever
applies them (R12).

### 10.5 Rewind

An old primary O rejoins the new primary P as a replica without a full base backup:

1. O fetches P's history (`Identify`, `FetchHistory`) and finds the **divergence point** D: the start LSN of the
   first term in P's history that is absent from O's history (or has a different `promotion_id`). If O's WAL ends
   at or before D, O needs no rewind: it streams from its end (§10.3 rule 3).
2. O scans its own WAL from D to its end and collects the set M of every page that a page record there touches,
   in both spaces, and every file that a `FILE_*` record there creates, extends or deletes.
3. O chooses the last complete checkpoint C of its own history whose `CHECKPOINT_END` lies below D. The WAL from
   C's `redo_lsn` to D is common to both histories.
4. O sends `StartBackup(mode = pages, pages = M)` to P. P checkpoints and sends the pages of M as of its current
   state, and the full list of segments with their lengths, as for a base backup (§4.2). O writes the pages,
   deletes every segment P does not list, truncates or extends every other to P's length, and fetches every page
   past O's old length of a segment.
5. O writes CONTROL with `checkpoint_lsn` and `redo_lsn` of C, `min_recovery_lsn` = the `stop_lsn` of the backup,
   `timeline` and fence term = P's term, and `state` 3; it deletes its WAL at and above D and its slots, and
   fetches P's history files. It then streams from D on P's history (from `wal/`, `archive/` or `restore_source`).
6. Redo from C repairs every page: a page outside M is in O's state at D, which is the common state, and P's WAL
   from D replays onto it; a page of M is in P's state at the backup, newer than anything replayed before
   `stop_lsn`, and a torn copy of it is rewritten by a later `FPI` (§4.2). O is consistent at `stop_lsn` and from
   then on byte-identical to any other replica at the same `applied` (R20).
7. Rewind needs every change to an on-disk page to be described by a WAL record. [DISK] §6.5 guarantees this; a
   build that adds an unlogged page change breaks rewind.

### 10.6 What failover loses

1. A commit acknowledged at `remote` or `remote_apply` is never lost by a promotion that fenced the synchronous set
   (§10.2). With an empty synchronous set, or after `--force`, it may be.
2. A commit acknowledged at `on` or `off` on the old primary and not yet `flushed` on the promoted replica is lost,
   together with every later commit of the old term ([ISO] §8). Its transaction id and `c` are of the old term, and
   above the old term's `end_commit` in the new history, so `transaction_status` on the new history answers
   `aborted`, and a token naming it fails with `token_not_in_history` (§8.3).
3. A replica that had received more of the old term than the candidate diverges, and is refused with
   `history_diverged` (`S1003`) until it is rewound. Promoting the most advanced replica overall loses least.

## 11. Versioning and upgrades

### 11.1 Protocol

The messages here are wire 1.1 (§2.1). A later minor MAY append fields to them and add messages in the reserved
ranges, under [WIRE] §2.2's rules. The fixed-width messages of §5.2–§5.4 grow only by appending after their last
field, and a receiver uses the negotiated minor to know their length.

### 11.2 Releases and formats

1. A primary and its replicas MAY run different releases, of any minor version, as long as they share a wire major
   and every replica can apply the primary's WAL format: `wal_format` ≤ the replica's `wal_format_max`
   (`wal_format_unsupported`, `S1005`, otherwise) and the replica reads the page formats the WAL writes
   ([DISK] §7.2).
2. A release that writes format N replays WAL of formats N and N−1, and keeps writing N−1 until the operator asks
   for the format upgrade ([DISK] §7.2 rule 3). So a primary on a newer release keeps streaming WAL an older replica
   can apply, until the upgrade.

### 11.3 The upgrade order

1. Upgrade each asynchronous replica, then each synchronous replica, one at a time; each reconnects and keeps
   applying the old format.
2. Switch over to an upgraded replica (§10.3), or restart the primary on the new release.
3. Upgrade the old primary, which rejoins as a replica.
4. Only when every node runs the new release, run the format upgrade ([DISK] §7.3) on the primary. Its
   `FORMAT_UPGRADE` reaches every replica; a replica that cannot apply it stops applying before the record,
   keeps serving reads at its last `applied_commit`, and reports `wal_format_unsupported` in its log and metrics.
5. Until step 4 every step can be reversed by reinstalling the previous release.

## 12. Settings

| setting | node | default | meaning |
|---|---|---|---|
| `max_replicas` | primary | 16 (≤ 64) | concurrent replication streams (§1.1) |
| `synchronous_replicas` | primary | empty | the synchronous set, ≤ 16 names (§6.2) |
| `synchronous_quorum` | primary | 1 | `k` (§6.2) |
| `synchronous_commit_timeout` | primary | 0 (none) | §6.4 |
| `max_slot_wal_keep` | primary | 64 GiB | §5.8 |
| `replica_feedback` | replica | off | §7.6 ([DISK] §5.5) |
| `replica_feedback_undo_limit` | primary | ½ `undo_retention_limit` | §7.6 |
| `max_replay_delay` | replica | 30 s | §7.5 |
| `replica_wait_timeout` | replica | as [ISO] §3.6 | §8.3, §8.4 |
| `status_interval` | both | 1 s (≥ 10 ms) | §5.3, §5.4 |
| `replication_window` | replica | 16 MiB | `StartReplication.window` (§5.5) |
| `restore_source` | replica | none | §5.7 |
| `replication_peers` | every node | empty | name → address, for `Fence` (§10.2) |
| `promotion_timeout` | candidate | 30 s | §10.2 |
| `fence_command` | candidate | none | §10.2 step 6 |

## 13. Errors

Every code is registered in [ERR]; this table says where each is raised.

| Code | Name | Raised |
|---|---|---|
| `S1001` | `not_primary` | `StartReplication` or `StartBackup` to a node that is not the primary of its term, or a request to a fenced primary (§5.1, §10.4); retryable on another node |
| `S1002` | `database_mismatch` | the peer's `db_uuid` differs (§3, §5.1) |
| `S1003` | `history_diverged` | the replica's history does not agree with the primary's at `start_lsn`, or a history file is unknown (§3, §5.1, §10.6) |
| `S1004` | `wal_unavailable` | the WAL at `start_lsn` is neither in `wal/` nor in `archive/` (§5.1, §5.7) |
| `S1005` | `wal_format_unsupported` | the primary's WAL format is above the replica's `wal_format_max` (§5.1, §11.2) |
| `S1006` | `replication_not_permitted` | the role lacks the replication right (§2.2) |
| `S1007` | `too_many_replicas` | `max_replicas` streams are open (§5.1); retryable |
| `S1008` | `stale_term` | a `Fence` whose term does not exceed the node's fence term (§10.2) |
| `S1009` | `connection_in_replication_mode` | a non-replication request on a replication connection, or the reverse; a second stream (§2.1) |
| `S2001` | `unknown_slot` | §5.1 |
| `S2002` | `slot_in_use` | the slot, or the replica name, is held by another stream (§4.2, §5.1, §6.2) |
| `S2003` | `slot_invalidated` | the slot passed `max_slot_wal_keep` (§5.8) |
| `S3001` | `fence_quorum_unreached` | promotion could not fence `n − k + 1` members, or `fence_command` failed (§10.2) |
| `S3002` | `not_most_advanced` | a fenced member has flushed more than the candidate (§10.2) |
| `S3003` | `term_exhausted` | a new term would exceed 65,535 (§1.2) |
| `S3004` | `not_a_replica` | promotion asked of a node that is not a replica (§10.2) |
| `T2002` | `commit_not_confirmed` | `synchronous_commit_timeout` passed; the transaction is committed (§6.4) |
| `T2003` | `token_not_in_history` | `after` names a commit this history does not contain (§8.3) |

Codes of other documents raised here: `T1005` `replica_lag_timeout` (§4.3, §8.3, §8.4), `T1006`
`replica_conflict` (§7.5), `T1008` `resource_exhausted` (§8.1), `T2001` `commit_unknown` (§6.4, §10.4), `T4001`
`read_only_transaction` (§8.1), `W2010` `status_horizon_passed` (§8.5), `D1001` `data_corrupted` (§7.2).

## 14. Invariants

These are the properties the deterministic simulation and the replication harness MUST check. The simulation runs
a primary and 1–4 replicas, can crash any node at any instruction, partition and heal any pair, delay, duplicate
and reorder nothing within a connection but anything across connections, pause apply, promote any replica, and
tear writes as [DISK] §9 does. R1–R4 and R3a are [ISO] §3.6's and keep their meaning there.

**Reads**

- **R1–R4, R3a.** As [ISO] §3.6 (consistent prefix, monotonic per connection, causal token, bounded staleness,
  no unflushed data), on every replica, through every failure above.
- **R5.** A replica transaction never aborts with `serialization_failure` or `schema_changed`, and aborts with
  `replica_conflict` only when a `PURGE_HORIZON(h)` with `h > s` was applied while it was live and
  `max_replay_delay` had passed since apply reached it.
- **R6.** No replica accepts a transaction before it is consistent (§4.3 rule 5, §7.7 rule 3).
- **R7.** For every replica snapshot `s` and every row, the version a replica reads equals the version a primary
  snapshot `s` reads (§7.4), at every apply worker count.
- **R8.** No replica reader observes part of a mini-transaction (§7.4 rule 2): every B+tree a replica reader
  descends satisfies [DISK] D20 at the moment of each node read.

**Log and pages**

- **R9.** The bytes a replica holds at every LSN below `received` equal the bytes of the primary's history at that
  LSN.
- **R10.** A replica stopped with `applied` = L has pages byte-identical, after writing its dirty pages, to the
  primary's database recovered by [DISK] §6.9's redo to L without its undo phase.
- **R11.** `applied ≤ received`, `flushed ≤ received`, and every reported position is non-decreasing on a stream;
  a reported `flushed` is durable on the replica.

**Histories and fencing**

- **R12.** After a node has synced fence term F, it applies no WAL byte of a term below F beyond its history's
  switch to F, sends no acknowledging `ReplicaStatus` to a primary of a term below F, and, if it was a primary of a
  lower term, acknowledges no commit.
- **R13.** No node holds WAL bytes whose history disagrees with its own (§1.2 rule 4); every segment header has
  `term` = `timeline`; every history file is a prefix-extension of its parent's.
- **R14.** Every transaction id and `c` has the term of the timeline that assigned it in its high 16 bits, and
  each is strictly increasing along every history, across every promotion and every crash.
- **R15.** A `Begin` with `after = n`, where `n` names a commit absent from the server's history, fails with
  `token_not_in_history`; it never returns a snapshot.

**Durability and failover**

- **R16.** Every commit acknowledged at `remote` is, at acknowledgement, durable on the primary and `flushed` on
  `synchronous_quorum` members of the synchronous configuration in effect (two quorums during a change); at
  `remote_apply`, applied on them.
- **R17.** After any promotion that did not use `--force`, the new history contains every commit acknowledged at
  `remote` or `remote_apply` in any earlier term of it.
- **R18.** After a promotion to term T, no node acknowledges a commit at `remote` or `remote_apply` in a term below
  T.
- **R19.** No acknowledged commit at `on` that a promotion lost is reported `committed` by any node of the new
  history.

**Backup, rewind and retention**

- **R20.** A replica seeded by base backup (§4), and an old primary after rewind (§10.5), each reach a state at
  `applied` = L that satisfies R10, for every L at or above its consistency point.
- **R21.** No segment at or above a valid slot's `restart_lsn` is recycled; a slot past `max_slot_wal_keep` is
  invalidated and its replica receives `slot_invalidated`, never WAL with a gap.
- **R22.** A backup changes nothing observable on the primary: every primary transaction's history is the same
  with and without a concurrent backup.

**Purge and feedback**

- **R23.** Every record that purge or truncation acts on, for a transaction with commit `c`, follows in the WAL a
  `PURGE_HORIZON` with `horizon ≥ c`, and `horizon` never decreases.
- **R24.** With feedback on or off, dropped, delayed or wrong, R1–R23 hold; and the undo held above `undo_low` on
  account of feedback never exceeds `replica_feedback_undo_limit` by more than one checkpoint interval's writes.

**Flow and versions**

- **R25.** The primary's sent-but-unreported bytes on a stream never exceed `window` plus one message.
- **R26.** A replica never applies a record whose `rec_format`, `wal_format` or page format it does not know; it
  stops before it.

## 15. Worked examples

### 15.1 A commit record in a WalData message

A primary in term 1 streams to a replica on request stream 1. Transaction `0x0001_0000_0000_002A` (term 1, counter
42) commits as `c = 0x0001_0000_0000_0011` (term 1, counter 17) at 2026-11-15T00:00:00Z. Its commit record is the
first record of segment 1, at LSN `0x0400_1000` (segment `1 = 0x0400_1000 >> 26`, offset 4,096), and its last undo
record is at undo address `0x14020` (undo page 5, offset 32). The flush that made it durable padded to the next
block, so `durable_lsn` = `0x0400_2000`. The message carries only the commit record:

```
0000  60 00 00 00 e1 00 00 00 01 00 00 00 00 00 00 00   frame: length 96, type 0xE1 WalData, flags 0, stream 1
0010  00 10 00 04 00 00 00 00                           start_lsn   0x04001000
0018  00 20 00 04 00 00 00 00                           durable_lsn 0x04002000
0020  dc 85 a9 57 45 60 06 00                           send_time   0x0006604557a985dc µs (1.5 ms after the commit)
0028  01 00 00 00 01 00 00 00                           timeline 1, term 1
0030  1e f9 7a 8f 40 00 00 00                           record: crc 0x8f7af91e, len 64
0038  00 10 00 04 00 00 00 00                                   lsn 0x04001000
0040  2a 00 00 00 00 00 01 00                                   txn 0x000100000000002a
0048  20 00 01 00 00 00 00 00                                   kind 32 TXN_COMMIT, flags 0, rec_format 1, reserved
0050  00 80 a9 57 45 60 06 00                                   commit_time 0x0006604557a98000 = 1,794,700,800,000,000 µs
0058  20 40 01 00 00 00 00 00                                   last_undo 0x14020
0060  11 00 00 00 00 00 01 00                                   commit_seq 0x0001000000000011
0068  00 00 00 00 00 00 00 00                                   commit_flags 0 (no catalog change)
```

Checks: the frame payload is 32 header octets plus one 64-octet record, so `length` = 96 = `0x60`. `start_lsn` +
64 = `0x0400_1040` ≤ `durable_lsn` (§5.2 rule 1). The record's `crc` is CRC32C ([DISK] §1 rule 4) over its octets
[4, 64), i.e. the message octets `0x34`–`0x6F`, and is `0x8F7A_F91E`; an implementation checks its CRC32C first
against the standard check value, CRC32C(`"123456789"`) = `0xE306_9283`. The commit's `c` and the `txn` have term
1 in their high 16 bits (§1.4). The 64-octet record keeps the next record 8-aligned ([DISK] §6.2 rule 1).

### 15.2 The replica's acknowledgement

The replica, a `SYNCHRONOUS` member, writes the bytes, flushes them, applies the commit, and reports (§5.4 rule 1),
carrying feedback (its oldest live snapshot is `0x0001_0000_0000_000F`) and probe 7, 800 µs after the primary sent:

```
0000  40 00 00 00 62 00 00 00 01 00 00 00 00 00 00 00   frame: length 64, type 0x62 ReplicaStatus, stream 1
0010  40 10 00 04 00 00 00 00                           received        0x04001040
0018  40 10 00 04 00 00 00 00                           flushed         0x04001040
0020  40 10 00 04 00 00 00 00                           applied         0x04001040
0028  11 00 00 00 00 00 01 00                           applied_commit  0x0001000000000011
0030  0f 00 00 00 00 00 01 00                           oldest_snapshot 0x000100000000000f
0038  07 00 00 00 00 00 00 00                           probe 7
0040  fc 88 a9 57 45 60 06 00                           send_time 0x0006604557a988fc
0048  01 00 00 00 00 00 00 00                           fence_term 1, flags 0
```

With `synchronous_quorum` = 1, this report makes `q_flush` = `0x0400_1040`, which covers the commit record's end
E = `0x0400_1040`, so a waiter at `remote` is released by it (§6.3), and since `q_apply` is the same, so is one at
`remote_apply`. The primary answers the new probe at once with a `PrimaryStatus` whose `echo` is 7 (§5.3 rule 1).

## 16. Open questions

Each question below has been answered by applying the recommendation given; `docs/decisions.md` records each as
`P1`–`P14` with the sections it changed, so that any of them can be reverted.

1. **Where does the stream run?** A separate protocol or port, or messages of the native protocol?
   *Recommendation, applied (P1):* the native protocol's reserved ranges, as wire 1.1, on dedicated replication
   connections, with fixed-width headers for the three hot messages. One listener, one TLS setup, one
   authentication path.
2. **Cascading replication?** *Recommendation, applied (P2):* not in v1. Every replica streams from the primary,
   which keeps fencing and the synchronous set in one place; `max_replicas` is at most 64.
3. **Timeline and term: two numbers or one?** *Recommendation, applied (P3):* one. The timeline id is the term,
   every switch takes a term above every term known, and each term carries a random `promotion_id` so that two
   careless promotions to the same number are still told apart.
4. **How does a new history tell a token from a lost commit?** *Recommendation, applied (P4):* transaction ids
   and commit sequence numbers carry the term in their high 16 bits, and the WAL carries every `c`
   (`TXN_COMMIT.commit_seq`, `CHECKPOINT_END.last_commit_seq`).
5. **Which synchronous levels?** *Recommendation, applied (P5):* `off`, `on`, `remote`, `remote_apply`, with a
   quorum `k` of at most 16 named replicas; `synchronous` becomes an enum on the wire (byte-identical to the old
   bool for `off` and `on`) and in the C ABI. A level that acknowledges on receipt without a replica fsync is
   left out: it protects only against a primary crash that the replica survives unflushed.
6. **What does a remote commit do when no quorum answers?** *Recommendation, applied (P6):* wait without limit by
   default; an optional timeout reports `commit_not_confirmed` for a transaction that is committed locally.
7. **How is split brain made impossible to accept, without a consensus service?** *Recommendation, applied (P7):*
   quorum-intersection fencing at promotion over the synchronous set, a fence term persisted in CONTROL, and
   synchronous configuration changes carried in the WAL with joint quorums. No primary lease: a partitioned old
   primary's `on` and `off` commits are lost, as [ISO] §8 already says, and an operator who wants them refused
   uses `remote`.
8. **What may conflict with replica readers?** *Recommendation, applied (P8):* only purge and undo truncation,
   announced by a `PURGE_HORIZON` record. DDL replay itself never conflicts; its destructive part is purge.
9. **Feedback on by default?** *Recommendation, applied (P9):* keep [DISK] §5.5's opt-in (decision Q24), and
   bound it: clamped by `replica_feedback_undo_limit` and `max_snapshot_age`, dropped on disconnect, and costing
   the primary O(`max_replicas`) per status at a bounded rate.
10. **How is a replica seeded?** *Recommendation, applied (P10):* a base backup over the protocol, copied after a
    completed checkpoint, with torn copies repaired by FPIs, and CONTROL synthesized on the replica.
11. **How does an old primary rejoin?** *Recommendation, applied (P11):* a built-in rewind that fetches only the
    pages its own divergent WAL touched, through `StartBackup` in `pages` mode.
12. **Who may replicate?** *Recommendation, applied (P12):* superusers and members of a predefined role
    `terndb_replication`, with no new role attribute.
13. **Slots and their bound?** *Recommendation, applied (P13):* persistent named slots on the primary, invalidated
    past `max_slot_wal_keep` (default 64 GiB) so that a dead replica cannot fill the primary's disk.
14. **How is `max_lag` measured?** *Recommendation, applied (P14):* by echoed probes timed on the replica's own
    monotonic clock, so that no clock comparison between nodes is needed.
