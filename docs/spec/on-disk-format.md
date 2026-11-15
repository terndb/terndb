# TernDB on-disk format

**Status: normative.** This document defines every byte TernDB writes to stable storage: the data and undo
segments, the write-ahead log, the control file and the memory-mode snapshot. Code follows this spec, and a change
to the format changes this spec in the same commit.

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as in RFC 2119. Every MUST is meant to be
checkable, either by a reader of the bytes or by the deterministic simulation that §9 describes.

Visibility rules (which version a snapshot sees, lock conflicts, isolation levels) belong to `isolation.md`. This
document defines only the stored fields those rules read.

---

## 1. Conventions

1. All multi-byte integers in page headers, slots, row values, undo records, WAL records and the control file are
   **little-endian**. Key bytes are the one exception: they use the order-preserving encoding of §4.6.
2. `uN` is an unsigned N-bit integer and `iN` is a two's-complement N-bit integer. Fields are packed with no implicit
   alignment padding. Every offset in a table is a byte offset from the start of the enclosing structure.
3. **Reserved** fields MUST be written as zero, and a reader MUST ignore them. A later format version may assign
   them, and the assignment then bumps the format version (§7).
4. **CRC32C** is the Castagnoli polynomial (0x1EDC6F41, reflected), with an initial value of 0xFFFFFFFF and a final
   XOR of 0xFFFFFFFF. "CRC over [a, b)" means the CRC of exactly those bytes.
5. The **page size** is 16,384 bytes (16 KiB) and is fixed for the life of a database. The **I/O block** is 4,096
   bytes. Every write the engine issues is a whole multiple of 4,096 bytes at an offset that is a multiple of 4,096.
6. The storage device is assumed to make a single aligned 4 KiB block write atomic and **nothing larger**. A 16 KiB
   page write MAY therefore tear at any 4 KiB boundary, and a multi-block WAL write MAY persist any subset of its
   blocks.
7. "Durable" means written and covered by a successful sync. On Linux the sync is `fdatasync` (or `O_DSYNC` writes);
   on macOS it MUST be `fcntl(F_FULLFSYNC)`, because plain `fsync` there does not flush the drive cache.
8. A value of 0 in any LSN, page id, undo address or transaction id field means "none".

---

## 2. The file set

A database is a directory. Its contents:

```
<db>/
  CONTROL                               the control file (§2.2)
  data.<gen>/                           data space, generation <gen> (§2.3)
    00000000.tseg  00000001.tseg  …     data segments, 1 GiB each
  undo.<gen>/                           undo space, same generation
    000000000000.tund  …                undo segments, 64 MiB each
  wal/
    <timeline>-<first lsn>.twal  …      WAL segments, 64 MiB each (§6.6)
  archive/                              optional: WAL kept for backup and PITR
```

1. File names are lowercase hexadecimal, zero-padded to the widths shown: data segment number 8 digits, undo
   segment number 12 digits, WAL timeline 8 digits and WAL first-LSN 16 digits.
2. A file not named here MUST be ignored on open, with one exception: a `data.<g>` or `undo.<g>` directory whose
   generation is not the one in CONTROL is garbage from an interrupted snapshot (§8.3) and MUST be deleted on open.
3. Every create, rename or delete of a file MUST be followed by a sync of its parent directory before the change is
   relied on.
4. Only one process may open a database read-write. It MUST hold an exclusive advisory lock (`flock`) on `CONTROL`
   for as long as it is open.

### 2.1 Page ids and the two page spaces

A **page id** (PID) is a `u64`:

| bits  | meaning |
|-------|---------|
| 63    | swizzle tag. It MUST be 0 in every stored byte (§3.8) |
| 62–60 | space: 0 = data, 1 = undo, 2–7 reserved |
| 59–0  | page number within the space |

The data page number `n` lives in segment `n >> 16`, at byte offset `(n & 0xFFFF) × 16384`. The undo page number
`n` lives in undo segment `n >> 12`, at offset `(n & 0xFFF) × 16384`.

*Rationale:* one global page space per kind, like InnoDB's system tablespace, makes a PID self-locating with no
per-relation file map, and fixed-size segments make a reflink clone or a backup a plain per-file operation.
Postgres's one-file-per-relation was rejected because it makes a PID two-dimensional and DDL a file operation.

### 2.2 The control file

`CONTROL` is 8,192 bytes long and holds **two slots** of 4,096 bytes, at offsets 0 and 4,096. A writer writes the
slot that does *not* hold the current highest sequence, then syncs. A reader validates both slots and uses the valid
one with the higher `sequence`. If neither is valid, the open MUST fail.

| off | size | field | meaning |
|----:|-----:|-------|---------|
| 0   | 4  | `crc` | CRC32C over [4, 4096) of the slot |
| 4   | 8  | `magic` | ASCII `TERNCTL1` |
| 12  | 2  | `format_current` | the format every new page is written in (§7) |
| 14  | 2  | `format_min` | the oldest format any stored page may still have |
| 16  | 8  | `sequence` | incremented on every control write |
| 24  | 16 | `db_uuid` | random, fixed at creation; also in every segment header |
| 40  | 1  | `mode` | 0 = on-disk, 1 = in-memory |
| 41  | 1  | `durability` | memory mode only: 0 = none, 1 = snapshot, 2 = log. On-disk mode writes 2 |
| 42  | 2  | reserved | |
| 44  | 4  | `page_size` | 16384 |
| 48  | 4  | `generation` | the live `data.<gen>` / `undo.<gen>` |
| 52  | 4  | `timeline` | the WAL timeline being written (§6.11) |
| 56  | 8  | `checkpoint_lsn` | LSN of the last completed CHECKPOINT_END record (0 when none) |
| 64  | 8  | `redo_lsn` | that checkpoint's redo start |
| 72  | 8  | `next_txn_id` | a lower bound on the next transaction id; recovery raises it |
| 80  | 8  | `next_oid` | a lower bound on the next object id |
| 88  | 8  | `undo_low` | the oldest undo address still retained (§5.5) |
| 96  | 8  | `min_recovery_lsn` | a replica or restored backup is not consistent before this LSN |
| 104 | 8  | `snapshot_lsn` | memory mode: the LSN at which the live generation was captured |
| 112 | 1  | `state` | 0 = shut down cleanly, 1 = open, 2 = in recovery, 3 = replica |
| 113 | 7  | reserved | |
| 120 | 8  | `feature_flags` | bit 0 = columnar projections present (reserved, §3.10); others reserved |
| 128 | 3968 | reserved | |

*Rationale:* the two alternating slots give an atomic control update without a rename, as in InnoDB's doublewritten
header and SQLite's two-step journal header.

### 2.3 Segment files

A data segment is up to 65,536 pages (1 GiB). Page 0 of every data segment is its **allocation bitmap page**
(§3.7). A segment MAY be shorter than 1 GiB; it grows by `FILE_EXTEND` (§6.4) in steps of at least 64 MiB, which
SHOULD be preallocated with `fallocate`/`F_PREALLOCATE`.

An undo segment is exactly 4,096 pages (64 MiB) and has no bitmap, because the undo space is append-only.

A page that has been allocated but never written reads as all zeros. A zero page is **only** valid where it is
unallocated, or where the next redo record for it is a `PAGE_INIT` or `FPI` (§6.9).

---

## 3. Pages

### 3.1 The common page header (32 bytes)

Every page in either space begins with this header.

| off | size | field | meaning |
|----:|-----:|-------|---------|
| 0  | 4 | `crc` | CRC32C over [4, 16384) |
| 4  | 2 | `format` | the page's format version (§7) |
| 6  | 1 | `type` | the page type (§3.2) |
| 7  | 1 | `flags` | type-specific, below |
| 8  | 8 | `page_lsn` | LSN of the last WAL record applied to this page |
| 16 | 8 | `pid` | this page's own PID |
| 24 | 8 | `owner` | the object id of the tree or table this page belongs to; 0 = system |

1. A reader MUST reject a page whose `crc` does not match, or whose `pid` differs from the PID it asked for (a
   misdirected write), before using any other field.
2. The CRC is computed when the page is written and checked when it is read. It is not maintained in memory.

**The latch version is not persisted.** The optimistic latch word (version counter plus lock bits) and the
eviction state live in the in-memory **frame descriptor** beside the page, never in the page's bytes. *Rationale:*
the version is meaningful only to threads of one process, and persisting it would make every latch acquisition
dirty a page. LeanStore keeps the latch in the buffer frame for the same reason.

### 3.2 Page types

| type | page |
|-----:|------|
| 0x00 | invalid. Only a zero page has it |
| 0x01 | allocation bitmap |
| 0x02 | B+tree inner node |
| 0x03 | B+tree leaf node |
| 0x04 | overflow |
| 0x05 | undo |
| 0x40–0x4F | reserved for columnar projections (§3.10) |
| 0xF0–0xFF | reserved for tests; a production build MUST reject them |

Any other type MUST be rejected as corruption.

### 3.3 B+tree node header (bytes 32–127)

Tables, secondary indexes and the catalog all use one B+tree node format. Level 0 is a leaf.

| off | size | field | meaning |
|----:|-----:|-------|---------|
| 32  | 2 | `level` | 0 = leaf |
| 34  | 2 | `slot_count` | number of entries |
| 36  | 2 | `heap_top` | the lowest offset in use by the heap; the heap grows down from 16384 |
| 38  | 2 | `frag_bytes` | bytes inside the heap that belong to no live entry |
| 40  | 2 | `prefix_len` | the length of the common key prefix (§3.5) |
| 42  | 2 | `lower_fence_off` | heap offset of the lower fence key |
| 44  | 2 | `lower_fence_len` | |
| 46  | 2 | `upper_fence_off` | heap offset of the upper fence key |
| 48  | 2 | `upper_fence_len` | |
| 50  | 2 | reserved | |
| 52  | 4 | reserved | |
| 56  | 8 | `extra` | inner: the **upper child** reference (keys ≥ the last separator). Secondary leaf: `max_txn` (§4.9). Clustered leaf: 0 |
| 64  | 64 | `hints` | 16 × `u32` key heads (§3.5) |
| 128 | … | slot directory | `slot_count` × 12 bytes |

The page `flags` byte for a node:

| bit | meaning |
|----:|---------|
| 0 | lower fence is −∞ (`lower_fence_len` is then 0) |
| 1 | upper fence is +∞ |
| 2 | `hints` are valid. A writer that does not maintain the hints MUST clear the bit |
| 3 | secondary index node |
| 4–7 | reserved |

Free space is `heap_top − (128 + 12 × slot_count)`. A node is **compacted** (the heap rewritten, `frag_bytes` set
to 0) when an insert needs the fragmented space.

**Fence keys, not sibling links.** Every node stores the full lower and upper bound of its key range in its heap.
A node has no sibling pointers; a range scan finds the next leaf by descending again from the parent with the
current leaf's upper fence. *Rationale:* without sibling links a split or merge latches only the parent and its
children, which keeps optimistic latch coupling simple and the redo record single-page (§6.3). This follows
LeanStore and Umbra.

**Root stability.** A tree's root PID never changes. A root split moves the root's entries into two new children
and leaves the root, one level higher, in place. The catalog records a root once (§3.9). This is what InnoDB does.

### 3.4 The slot directory

Slots are sorted by key and grow upward from offset 128. Each slot is 12 bytes:

| off | size | field | meaning |
|----:|-----:|-------|---------|
| 0 | 2 | `offset` | heap offset of the entry. The key suffix starts there and the value follows it at once |
| 2 | 2 | `key_len` | length of the key **suffix** (the key minus the node prefix) |
| 4 | 2 | `value_len` | length of the value |
| 6 | 2 | `flags` | bit 0 = delete-marked (secondary index only, §4.9); others reserved |
| 8 | 4 | `head` | the first 4 bytes of the key suffix read **big-endian**, zero-padded when shorter |

In an inner node every value is exactly 8 bytes: a child reference (§3.8). Slot `i`'s child holds keys in
`[key(i−1), key(i))`, and the upper child holds keys ≥ the last key. In a leaf the value is a row (§4) or a secondary
index payload (§4.9).

### 3.5 Prefix compression and hints

1. `prefix_len` MUST equal the length of the longest common prefix of the lower and upper fence keys (0 when either
   fence is infinite). The prefix is not stored separately: it is the first `prefix_len` bytes of the lower fence.
2. Every key in the node MUST begin with that prefix, and is stored **without** it.
3. Inner-node separators MUST be **suffix-truncated** on split: the separator is the shortest byte string `s` with
   `left_max < s ≤ right_min`. *Rationale:* shorter separators raise fan-out (Bayer and Unterauer; LeanStore).
4. When `flags` bit 2 is set, `hints[j]` (for j = 0..15) MUST equal the `head` of slot
   `⌊(j + 1) × slot_count / 17⌋`. A search MAY use the hints to narrow the binary search before it reads slots.

### 3.6 Size limits

| quantity | limit | reason |
|----------|------:|--------|
| encoded key (full, before prefix removal) | 1,024 bytes | two fences plus three maximal entries fit a page |
| leaf entry inline (key + value) | 4,000 bytes | a leaf holds at least 3 entries |
| inner fan-out | ≥ 13 | from the key limit |

A key longer than 1,024 bytes MUST be refused with an error at insert time. It is never moved out of line.

### 3.7 Allocation bitmap pages (the free-space map)

Page 0 of each data segment:

| off | size | field |
|----:|-----:|-------|
| 0    | 32   | common header, `type` 0x01, `owner` 0 |
| 32   | 16   | `db_uuid` |
| 48   | 4    | `segment_no` |
| 52   | 4    | `allocated_count` |
| 56   | 8    | reserved |
| 64   | 8192 | the bitmap: bit `i` (byte `i >> 3`, bit `i & 7`) is 1 when page `i` of this segment is allocated |
| 8256 | 8128 | reserved |

1. Bit 0 (the bitmap page itself) MUST always be 1.
2. `allocated_count` MUST equal the population count of the bitmap.
3. A freed page MUST NOT be reused until (a) its `PAGE_FREE` record is durable and (b) no in-memory optimistic
   reader can still hold a reference to it (epoch-based reclamation, as in LeanStore). Condition (b) is in-memory
   state only. After a restart no reader exists, so the bitmap alone is the truth.

B+tree pages need no per-page free-space tracking, because placement in a clustered tree is decided by the key.
*Rationale:* this is why TernDB has no Postgres-style FSM fork. Only the allocation state is persisted.

### 3.8 Page references and swizzling

A **child reference** is the 8-byte value of an inner-node slot, or the inner node's `extra` field.

1. **On disk, a child reference MUST be a PID**, with bit 63 clear.
2. In memory, the buffer manager MAY replace it with a **swizzled** reference: bit 63 set and bits 62–0 the address
   of the child's frame. Frame addresses MUST therefore fit in 63 bits.
3. Every frame descriptor MUST record its page's PID, so unswizzling is one load from the frame, with no lookup.
4. A page image written to disk MUST have every swizzled reference replaced by its child's PID. The writer copies
   the page into the I/O buffer and unswizzles in the copy; the frame's own bytes keep their swizzled pointers.
5. No reference other than a child reference is ever swizzled. Overflow PIDs, undo addresses and the catalog's
   root PIDs always resolve through the page table.

*Rationale:* a single tag bit makes "is it in memory?" one test on the hot path and makes unswizzling a bit-clear
plus a load (LeanStore, Umbra). Clearing the tag in the write copy (rule 4) means a swizzled child never blocks
writing its parent.

### 3.9 Catalog pages

The catalog is a set of ordinary B+trees whose roots have fixed PIDs in data segment 0:

| PID | tree | key | 
|----:|------|-----|
| 1 | `sys.objects` | `oid` (u64) |
| 2 | `sys.columns` | (`table_oid` u64, `schema_version` u16, `ordinal` u16) |
| 3 | `sys.names` | (`namespace_oid` u64, `name` text) → `oid`. This is a unique secondary index of `sys.objects` |
| 4–15 | reserved for later system trees |

Catalog rows use the row format of §4 and are versioned by the undo log like any table, so DDL is transactional.
Object ids 1–1023 are reserved for system objects.

`sys.objects` non-key columns, in order:

| column | type | meaning |
|--------|------|---------|
| `kind` | i8 | 1 table, 2 secondary index, 3 sequence, 4 named type, 5 namespace; 0x40–0x4F reserved for projections |
| `namespace_oid` | i64 | |
| `name` | text | |
| `parent_oid` | i64 | for an index, its table; else 0 |
| `root_pid` | i64 | the tree's root PID; 0 when there is no tree |
| `schema_version` | i16 | the current version of a table's columns |
| `key_spec` | bytes | for a table or index: n × (`ordinal` u16, `flags` u8: bit 0 = descending, bit 1 = unique) |
| `options` | bytes | reserved, empty in v1 |

`sys.columns` non-key columns: `name` text, `type` bytes (a type descriptor, §4.3), `nullable` bool, `default`
bytes (an encoded `query-tree.md` expression, or empty), `dropped` bool.

Every column list (old versions included) stays in `sys.columns` while any stored row still carries that
`schema_version` (§4.1), so a row is always decodable by the version it was written in. This is what makes
`ADD COLUMN` and `DROP COLUMN` instant, as in InnoDB's instant DDL.

### 3.10 The columnar hook

v1 writes no columnar projection, and a v1 reader MUST refuse to open a database whose `feature_flags` bit 0 is set.
Page types 0x40–0x4F, object kinds 0x40–0x4F and WAL kinds 0x40–0x4F are reserved for projections. A projection's
design MUST place its pages in the data space and log them through the WAL kinds of §6, so recovery, replication
and backup need no new mechanism.

---

## 4. The row format

A clustered-leaf entry is (key, value). The **key** is the primary key in the order-preserving encoding (§4.6). The
**value** is a row, laid out as follows. Primary-key columns are stored in the key only and MUST NOT be repeated in
the value.

### 4.1 Row header (20 bytes)

| off | size | field | meaning |
|----:|-----:|-------|---------|
| 0  | 8 | `txn` | the transaction that wrote this version, or that holds the row's write lock (`LOCK_ONLY`) |
| 8  | 8 | `undo` | the undo address of the record that rebuilds the previous version; 0 = none |
| 16 | 2 | `flags` | below |
| 18 | 2 | `schema_version` | the column list this row was encoded with (§3.9) |

`flags`:

| bit | name | meaning |
|----:|------|---------|
| 0 | `DELETED` | a ghost: the row is deleted as of `txn`, and stays until purge (§5.6) |
| 1 | `LOCK_ONLY` | `txn` holds the write lock without having changed the row. The version's writer is the `prev_txn` of the LOCK undo record at `undo` |
| 2 | `HAS_EXTERNAL` | an external bitmap follows the null bitmap (§4.2) |
| 3–15 | reserved | |

**The write lock is the `txn` field.** A row is write-locked by transaction T exactly when `txn = T` and T is
active. There is no lock table. A writer that finds an active foreign `txn` MUST wait for that transaction to end
(or fail, per `isolation.md`). After recovery every id below `next_txn_id` is either committed or rolled back,
so a stored `txn` of a transaction that is not active never means "locked". *Rationale:* this is InnoDB's implicit
lock. It keeps lock state on the page the writer already latches, and it lets rows of a crashed transaction stay
locked while it is rolled back in the background (§6.10).

### 4.2 Row body

After the header, in order:

1. **Null bitmap**: ⌈n / 8⌉ bytes, where n is the number of non-key columns in `schema_version`. Bit i (byte
   `i >> 3`, bit `i & 7`) is 1 when column i is NULL. Columns are numbered in `ordinal` order, skipping the key.
2. **External bitmap**, present only when `HAS_EXTERNAL`: same size, and bit i is 1 when variable column i is stored
   out of line (§4.7).
3. **Fixed area**: every fixed-width column (§4.4), in ordinal order, at its full width. A NULL column still takes
   its width and MUST be zero-filled. *Rationale:* fixed positions make an in-place update of a fixed column a
   byte overwrite and a small redo record.
4. **Variable offsets**: one `u16` per variable-width column, in ordinal order: the end of that column's bytes,
   relative to the start of the variable area. A NULL column has length 0 (its end equals the previous end).
5. **Variable area**: the bytes of the variable columns, concatenated.

The same layout, with the header omitted and `u32` variable offsets in place of `u16`, is a **record**. Records
are the elements of nested relations (§4.5).

### 4.3 Type descriptors

A column type is stored in `sys.columns.type` as a descriptor, a tag byte followed by its parameters:

| tag | type | parameters |
|----:|------|------------|
| 0x01 | `bool` | |
| 0x02–0x06 | `i8`, `i16`, `i32`, `i64`, `i128` | |
| 0x07, 0x08 | `f32`, `f64` | |
| 0x09 | `decimal` | `precision` u8 (1–76), `scale` u8 |
| 0x0A | `text` | |
| 0x0B | `bytes` | |
| 0x0C | `date` | |
| 0x0D | `time` | |
| 0x0E | `timestamp` | |
| 0x0F | `timestamptz` | |
| 0x10 | `interval` | |
| 0x11 | `uuid` | |
| 0x12 | `enum` | `type_oid` u64 |
| 0x20 | `relation` | `ordered` u8 (0 = bag, 1 = list), `n` u16, then n × (`name_len` u16, name UTF-8, `nullable` u8, descriptor) |
| 0x21 | `union` | `n` u16, then n × (`name_len` u16, name UTF-8, descriptor) |

The type set is owned by `query-tree.md`. A type it gains MUST get a tag and an encoding here before a value of
that type is stored.

### 4.4 Scalar encodings (in the row value)

| type | width | encoding |
|------|------:|----------|
| `bool` | 1 | 0 or 1; any other byte is corruption |
| `iN` | N/8 | two's complement |
| `f32`, `f64` | 4, 8 | IEEE 754 binary32 / binary64 bits |
| `decimal` p ≤ 18 | 8 | `i64` unscaled value |
| `decimal` 19 ≤ p ≤ 38 | 16 | `i128` unscaled value |
| `decimal` p > 38 | variable | `i8` sign (−1, 0, 1), then base-10⁹ limbs as `u32`, most significant first |
| `text` | variable | UTF-8. A writer MUST store valid UTF-8 and a reader MAY verify it |
| `bytes` | variable | raw |
| `date` | 4 | `i32` days since 1970-01-01 (proleptic Gregorian) |
| `time` | 8 | `i64` microseconds since midnight |
| `timestamp`, `timestamptz` | 8 | `i64` microseconds since 1970-01-01T00:00:00 (UTC for `timestamptz`) |
| `interval` | 16 | `i32` months, `i32` days, `i64` microseconds |
| `uuid` | 16 | the 16 bytes in RFC 9562 order |
| `enum` | 4 | `u32` label index in the type's declaration order |
| `relation` | variable | §4.5 |
| `union` | variable | §4.5 |

### 4.5 Nested relations and unions

A **relation value** (a column of type `relation`, at any depth):

| off | size | field |
|----:|-----:|-------|
| 0 | 1 | `form`: 0 = inline records; 1 reserved for an out-of-line subtree (Open question 2) |
| 1 | 3 | reserved |
| 4 | 4 | `count` (u32) |
| 8 | 4 × count | end offset of each element (u32), relative to the first element |
| … | … | the elements, each a **record** (§4.2) of the element type |

1. A `list` relation keeps its elements in order. A `bag` relation stores them in insertion order and promises no
   order.
2. Element columns are stored in declared order. A nested relation has no key and no MVCC header: it is versioned
   with the row that holds it, and an update to any element is an update to the containing column.
3. Nesting is recursive: an element column of type `relation` is a relation value inside that record's variable
   area.

A **union value**: a `u16` tag (the variant's index in the descriptor), then the variant's encoding. A fixed-width
variant takes exactly its width; a variable variant takes the remaining bytes of the enclosing slot. A union column
is always variable-width. A relation whose element type is a union is how tagged-union elements are stored.

### 4.6 Key encoding (order-preserving)

Keys MUST compare correctly with `memcmp`, so that prefix compression, `head` and suffix truncation work on bytes.

| type | encoding |
|------|----------|
| `iN`, `date`, `time`, `timestamp*`, `decimal` ≤ 38 | big-endian with the sign bit inverted |
| `bool`, `enum` | big-endian unsigned |
| `f32`, `f64` | big-endian bits; when the sign is 1 invert all bits, else invert the sign bit. −0 MUST be stored as +0, and every NaN as the one canonical quiet NaN, which sorts last |
| `uuid` | the 16 bytes |
| `interval` | months, days, microseconds, each as `iN` above |
| `text`, `bytes`, `decimal` > 38 | the bytes with each 0x00 escaped as 0x00 0xFF, then the terminator 0x00 0x00 |

1. A nullable component of a secondary-index key is prefixed with 0x00 for NULL (nulls first) or 0x01 for a
   value. Primary-key columns are NOT NULL and have no prefix.
2. A descending component has every byte of its encoding inverted, prefix included.
3. A composite key is the concatenation of its components.
4. `text` compares by binary UTF-8 in v1 (Open question 1). `relation` and `union` columns MUST NOT be key
   columns.

### 4.7 In-place versus out-of-line

1. When the key plus encoded value of an entry is at most **4,000 bytes**, the entry MUST be stored inline.
2. Otherwise the writer MUST move variable columns out of line, **largest first**, considering only columns whose
   encoding is at least 256 bytes, until the entry is at most 4,000 bytes. If that cannot be achieved, the write
   MUST fail. (Fixed areas alone cannot exceed the limit for any table the catalog accepts; `CREATE TABLE` MUST
   refuse a table whose fixed area plus maximum key exceeds 3,000 bytes.)
3. An out-of-line column's bytes in the variable area are a 32-byte **stub**:

| off | size | field |
|----:|-----:|-------|
| 0  | 8 | `first_pid`: the head of its overflow chain |
| 8  | 8 | `raw_len`: the length of the value before compression |
| 16 | 8 | `stored_len`: the length in the chain |
| 24 | 4 | `raw_crc`: CRC32C of the uncompressed value |
| 28 | 1 | `codec`: 0 none, 1 LZ4 block, 2 Zstandard frame; others reserved |
| 29 | 3 | reserved |

4. A nested relation value is moved out of line whole. Its chain holds exactly the §4.5 encoding.

### 4.8 Overflow chains

An overflow page:

| off | size | field |
|----:|-----:|-------|
| 0  | 32 | common header, `type` 0x04, `owner` = the table's oid |
| 32 | 8  | `next_pid`, 0 at the end of the chain |
| 40 | 4  | `used`: payload bytes in this page (1–16,336) |
| 44 | 4  | reserved |
| 48 | 16336 | payload |

1. The value is compressed as a whole by `codec`, then cut into pages in order. A writer SHOULD compress a value of
   2 KiB or more with LZ4, and SHOULD store it uncompressed when compression saves less than 1/8.
2. **A chain is immutable.** It is written once, when its value is written, and is never modified. An update
   writes a new chain, and the old chain is freed by purge once the undo record that names it is truncated (§5.6).
   *Rationale:* an old version's stub in the undo log stays valid with no copy of the value, as with InnoDB's
   externally stored columns.
3. A reader MUST verify `raw_crc` after decompression.

### 4.9 Secondary indexes

A secondary index is a B+tree (node `flags` bit 3) whose entries **point at the primary key**, never at a page.

1. A non-unique index's key is (index columns ‖ primary key) and its value is empty. A unique index's key is the
   index columns and its value is the encoded primary key.
2. Secondary entries carry no transaction fields. An update that changes an indexed column delete-marks the old
   entry (slot `flags` bit 0) and inserts the new one. A reader that finds an entry fetches the clustered row,
   rebuilds the version its snapshot sees, and keeps the entry only when that version's index columns equal the
   entry's.
3. A secondary leaf's `extra` is `max_txn`: the highest transaction id that has modified the leaf. A reader whose
   snapshot sees every transaction ≤ `max_txn` MAY trust the leaf's non-delete-marked entries without the clustered
   lookup (an index-only scan).
4. A delete-marked entry is removed only by purge (§5.6).

*Rationale:* pointing at the key means a page split or a row's move never touches any secondary index (InnoDB,
LeanStore). The extra lookup is the price, and rule 3 avoids it for cold data, as InnoDB's `PAGE_MAX_TRX_ID` does.

---

## 5. The undo log

### 5.1 The undo space

The undo log is one append-only byte stream. An **undo address** is a `u64` byte position in it. Undo page n holds
addresses [n × 16384 + 32, (n + 1) × 16384). An address whose low 14 bits are below 32 is invalid. Undo addresses
increase monotonically and are never reused.

An undo page:

| off | size | field |
|----:|-----:|-------|
| 0  | 32 | common header, `type` 0x05, `owner` 0 |
| 32 | 16352 | records, packed |

1. Undo pages are ordinary pages: buffered, checksummed and written under the WAL rule. Every byte appended to
   one is covered by an `UNDO_APPEND` redo record (§6.4). **The undo log is never synced at commit**; it is
   recovered from the WAL. This is InnoDB's design, and it keeps one sync per commit group.
2. A record MUST NOT span undo pages. The unused tail of a page MUST be covered by a PAD record (kind 0).
3. Space is reserved by an atomic fetch-and-add on the undo high-water mark. A transaction's records need not be
   contiguous.

### 5.2 Undo record format

| off | size | field | meaning |
|----:|-----:|-------|---------|
| 0  | 2 | `len` | the total length of the record |
| 2  | 1 | `kind` | 0 PAD, 1 INSERT, 2 UPDATE, 3 DELETE, 4 LOCK, 5 SECONDARY (index-entry change); others reserved |
| 3  | 1 | `flags` | bit 0 = the old version had `DELETED` set; bit 1 = an old value is out of line |
| 4  | 2 | `old_schema_version` | |
| 6  | 2 | `key_len` | |
| 8  | 8 | `txn` | the transaction that made the change |
| 16 | 8 | `prev_version` | the old version's `undo` field: the next link of the **version chain** |
| 24 | 8 | `txn_prev` | this transaction's previous undo record: the link of the **rollback chain** |
| 32 | 8 | `prev_txn` | the old version's `txn` field |
| 40 | 8 | `oid` | the table |
| 48 | key_len | key | the row's encoded primary key |
| … | … | payload | by kind, below |

Payloads:

- **INSERT**: none. The previous version is "absent".
- **DELETE**, **LOCK**: none. The old row's contents are unchanged in the leaf; only its header changed.
- **UPDATE**: `n` u16, then n × (`ordinal` u16, `is_null` u8, `len` u32, the old encoded bytes). An out-of-line
  old value is recorded as its 32-byte stub, never copied.
- **SECONDARY**: `index_oid` u64, then the index entry's key. It records a secondary insert, so a rollback can
  remove the entry and purge can remove a delete-marked one.

### 5.3 Chaining

Each record sits on two chains:

1. The **version chain** runs from a row's `undo` field through each record's `prev_version`. Following it walks
   the row's versions from newest to oldest. Along the chain, `txn` strictly decreases in commit order.
2. The **rollback chain** runs from a transaction's newest record through `txn_prev`. Following it undoes the
   transaction in reverse order.

### 5.4 Reconstructing a version

A reader with snapshot S looks up the row in the leaf and copies it under the optimistic latch. Then, starting from
that copy:

1. If S sees `txn` (per `isolation.md`, with `LOCK_ONLY` reading `prev_txn` from the record at `undo`), the current
   version is the answer: absent if `DELETED`, else the row.
2. Otherwise, if `undo < undo_low` (§5.5), the database is corrupt. Rule D17 forbids this.
3. Otherwise, read the record at `undo` and apply it backward: INSERT gives "absent"; DELETE clears `DELETED`;
   UPDATE restores the listed columns and `old_schema_version`; LOCK changes nothing in the row. Then set
   `txn ← prev_txn` and `undo ← prev_version`, and repeat from step 1.

Reconstruction reads the undo log only. It never writes, and it never latches the leaf longer than the copy.

### 5.5 Truncation and the horizon

`undo_low` is the lowest undo address the engine retains. The **horizon** is the commit point before which every
live snapshot sees everything. The engine MAY advance `undo_low` to address A only when every record below A
belongs to a committed transaction that every live snapshot sees, and, on a primary with feedback enabled, every
replica snapshot (Open question 4).

1. Truncation deletes whole undo segment files below `undo_low`. It is logged as `UNDO_TRUNCATE` and recorded in
   CONTROL at the next checkpoint.
2. A row whose `undo` is below `undo_low` is visible to every snapshot as it stands. The truncation of its undo
   never rewrites the row. *Rationale:* monotonic addresses make "too old to need history" a single comparison,
   with no per-row cleanup.

**A long transaction pins undo, not tables.** A snapshot that stays open holds `undo_low` back, so the undo space
grows while it lives. The tables do not grow: old versions never sit in a leaf, so scans and index sizes are
unaffected. When undo bytes held above `undo_low` exceed the setting `undo_retention_limit` (default 25% of free
disk), the engine SHOULD cancel the oldest snapshot with a "snapshot too old" error rather than run out of space.
This is the trade InnoDB and Oracle make, and it is why TernDB has no VACUUM.

### 5.6 Purge

Purge walks committed transactions' records in address order, at or above `undo_low` and below the horizon, and
for each one:

- **DELETE**: removes the ghost row from the clustered leaf, if its `undo` still equals this record's address
  (else a later writer has reused the key and the ghost is gone).
- **UPDATE** with an out-of-line old value: frees that value's old overflow chain.
- **SECONDARY** whose entry is delete-marked: removes the entry.

Purge then advances `undo_low` past the records it has finished. Purge touches only the rows the log names: it is
work proportional to writes, never a table scan.

---

## 6. The write-ahead log

### 6.1 LSNs

An **LSN** is a `u64` byte position in the WAL stream of one timeline. WAL segment k of a timeline covers LSNs
[k × 2²⁶, (k + 1) × 2²⁶), and LSN L is at byte offset `L mod 2²⁶` in segment `L >> 26`. The first 4,096 bytes of
each segment are its header, so records start at offset 4,096 and the first LSN of a database is 4,096.

### 6.2 Record header (32 bytes)

| off | size | field | meaning |
|----:|-----:|-------|---------|
| 0  | 4 | `crc` | CRC32C over [4, `len`) of the record |
| 4  | 4 | `len` | total length, header included, a multiple of 8 |
| 8  | 8 | `lsn` | this record's own LSN |
| 16 | 8 | `txn` | the transaction, or 0 |
| 24 | 1 | `kind` | §6.4 |
| 25 | 1 | `flags` | bit 0 `MTR_CONT`: more records of this mini-transaction follow. Bit 1 `COMPENSATION`: written by rollback (§6.10) |
| 26 | 2 | `rec_format` | the WAL record format version |
| 28 | 4 | reserved | |

1. Records are 8-byte aligned, padded at the end with zeros that are counted in `len`.
2. A record MUST NOT span WAL segments. A record that does not fit in what remains of a segment is preceded by a
   PAD record that fills the segment.
3. The `lsn` field lets a reader reject a stale record left in a recycled segment, whose position does not match
   its stored LSN.

### 6.3 Mini-transactions and the one-page rule

1. **Every redo record modifies at most one page**, and names it in its payload's first 8 bytes (`pid`).
2. A change that touches several pages (a split, an insert plus its allocation, an overflow chain) is a
   **mini-transaction**: its records are reserved contiguously in the WAL buffer, every record but the last has
   `MTR_CONT`, and recovery applies a mini-transaction only when its last record is present and valid.
3. Latches held for a mini-transaction are released only after its records are in the WAL buffer.

*Rationale:* the one-page rule is what makes redo partitionable by page (§6.9). Grouping is what makes a structure
change atomic without logical redo. The scheme is InnoDB's mini-transaction.

### 6.4 Record kinds

Page records (payload starts with `pid` u64):

| kind | name | payload after `pid` |
|-----:|------|---------------------|
| 1  | `PAGE_INIT` | `type` u8, `flags` u8, `owner` u64, `format` u16, `level` u16, plus the fences for a node |
| 2  | `FPI` | `codec` u8 (0 none, 1 LZ4), `len` u16, the image: the full page **before** the change that follows it |
| 3  | `NODE_INSERT` | `slot` u16, `slot_flags` u16, `key_len` u16, `value_len` u16, key suffix, value |
| 4  | `NODE_DELETE` | `slot` u16 |
| 5  | `NODE_PATCH` | `slot` u16, `n` u16, n × (`value_offset` u16, `len` u16, bytes): an in-place byte change |
| 6  | `ROW_HEADER` | `slot` u16, `txn` u64, `undo` u64, `flags` u16 |
| 7  | `NODE_SLOT_FLAGS` | `slot` u16, `flags` u16 |
| 8  | `NODE_TRUNCATE` | `from_slot` u16, new upper fence (len u16, bytes), new `extra` u64: the left half of a split |
| 9  | `NODE_BULK` | `count` u16, then the entries as `NODE_INSERT` bodies: fills a new node |
| 10 | `NODE_SET_EXTRA` | `extra` u64 |
| 11 | `NODE_COMPACT` | none: deterministic compaction |
| 12 | `OVERFLOW_WRITE` | `next_pid` u64, `used` u32, the payload |
| 13 | `UNDO_APPEND` | `offset` u16, `len` u16, bytes. `pid` is in the undo space |
| 14 | `PAGE_ALLOC` | `page_index` u16 (on a bitmap page) |
| 15 | `PAGE_FREE` | `page_index` u16 |
| 16 | `FORMAT_PAGE` | the whole page rewritten in `format_current` (§7.3) |

Other records (no page):

| kind | name | payload |
|-----:|------|---------|
| 0  | `PAD` | zeros |
| 32 | `TXN_COMMIT` | `commit_time` i64 (µs since epoch, wall clock), `last_undo` u64 |
| 33 | `TXN_ABORT` | none; written after a rollback completes |
| 34 | `CHECKPOINT_BEGIN` | none |
| 35 | `CHECKPOINT_END` | `begin_lsn` u64, `redo_lsn` u64, `next_txn_id` u64, `next_oid` u64, `undo_low` u64, `undo_high` u64, `n_txn` u32, n_txn × (`txn` u64, `last_undo` u64, `undo_next` u64) |
| 36 | `FILE_CREATE` | `space` u8, `segment_no` u64 |
| 37 | `FILE_EXTEND` | `space` u8, `segment_no` u64, `new_page_count` u32 |
| 38 | `FILE_DELETE` | `space` u8, `segment_no` u64 |
| 39 | `UNDO_TRUNCATE` | `new_undo_low` u64 |
| 40 | `FORMAT_UPGRADE` | `from` u16, `to` u16 |
| 41 | `FORMAT_MIN` | `new_min` u16 |
| 42 | `TIMELINE_SWITCH` | `old_timeline` u32, `new_timeline` u32, `switch_lsn` u64 |
| 43 | `RESTORE_POINT` | `name_len` u16, name |

Kinds 0x40–0x4F are reserved for projections. A COMPENSATION record carries, after its normal payload, `undo_next`
u64: the rollback chain position still to be undone.

Every page record's effect MUST be a deterministic function of the page's bytes and the record. A record MUST NOT
depend on another page's contents, on memory addresses, or on the time.

### 6.5 The WAL protocol and full-page images

1. **WAL before data:** a page MUST NOT be written to its segment until the WAL is durable up to its `page_lsn`.
2. **Commit:** a transaction is committed when its `TXN_COMMIT` record is durable, and not before. The commit
   order of transactions is the LSN order of their commit records.
3. **Full-page images:** when a mini-transaction first modifies a page whose `page_lsn` is below the current
   checkpoint's `redo_lsn`, it MUST log an `FPI` of the page before the page's first change record, in the same
   mini-transaction. A freshly initialized page needs none, because `PAGE_INIT` is a full description.
4. Applying a record sets the page's `page_lsn` to the record's `lsn`.

*Rationale:* FPIs on the first change after a checkpoint are Postgres's torn-page defence. They cost one image per
page per checkpoint interval, where InnoDB's doublewrite buffer costs a second write on every flush.

### 6.6 WAL segments

Segment files are named `<timeline:08x>-<first lsn:016x>.twal` and are exactly 64 MiB. The header block:

| off | size | field |
|----:|-----:|-------|
| 0  | 4  | `crc`: CRC32C over [4, 4096) |
| 4  | 8  | `magic`: ASCII `TERNWAL1` |
| 12 | 2  | `wal_format` |
| 14 | 2  | reserved |
| 16 | 16 | `db_uuid` |
| 32 | 4  | `timeline` |
| 36 | 4  | reserved |
| 40 | 8  | `first_lsn` |
| 48 | 8  | `prev_segment_crc`: the CRC of the previous segment's header (0 for the first) |
| 56 | 4040 | reserved |

A segment MAY be recycled (renamed and rewritten) only once it is below every retention bound: the checkpoint's
`redo_lsn`, the archiver, every replication slot and any running backup.

### 6.7 Group commit and the flush rule

1. Writers reserve WAL space by atomic fetch-and-add and copy their records into the shared buffer. A flusher
   writes the filled, contiguous prefix and syncs it. `durable_lsn` is the end of the longest contiguous prefix
   known to be synced.
2. A commit MUST NOT be acknowledged until `durable_lsn` ≥ the end of its commit record.
3. **No flush rewrites a durable block.** A flush that ends in the middle of a 4 KiB block MUST first pad to the
   block boundary with a PAD record, and the next flush starts in a fresh block. *Rationale:* a torn rewrite of a
   partly durable block would destroy commits already acknowledged, which Postgres and InnoDB risk by rewriting
   their last log page. Padding costs at most 4 KiB per sync, which a group amortizes.
4. Several in-flight writes of one flush MAY complete in any order. `durable_lsn` advances only over a prefix whose
   every block has been written and synced.

### 6.8 Checkpoints

A checkpoint is fuzzy, in the Postgres manner: work continues while it runs.

1. Write `CHECKPOINT_BEGIN` at LSN B. From then on the current `redo_lsn` is B, and rule 6.5.3 is measured
   against it.
2. Write every page that was dirty at B (`page_lsn` < B and not yet written), at a rate that spreads the I/O.
   Then sync every segment that was written.
3. Write `CHECKPOINT_END` with `begin_lsn` = B and `redo_lsn` = B, and the active-transaction table as of B, and
   make it durable.
4. Write CONTROL with `checkpoint_lsn` = the END record's LSN, `redo_lsn` = B, `undo_low` and the counters, and
   sync it.

A checkpoint is **complete** only once step 4 is durable. Recovery always starts from the last complete one.

### 6.9 Recovery

Recovery runs on open whenever CONTROL `state` is not 0 (clean). It has three phases.

**Analysis** (one thread). Read `CHECKPOINT_END` at `checkpoint_lsn` (or start at LSN 4,096 when there is none).
Load its transaction table. Scan forward from `redo_lsn`, maintaining for each transaction its `last_undo` (from
`UNDO_APPEND`) and its `undo_next` (from COMPENSATION records), removing a transaction at its COMMIT or ABORT, and
raising `next_txn_id`, `next_oid` and `undo_high` over every value seen. **The end of the log** is the first
position where any of these holds: the CRC fails; `lsn` ≠ the position; `len` is 0, not a multiple of 8, or runs
past the segment; the segment file is missing; or its header's `db_uuid` or `timeline` is wrong. A trailing
mini-transaction whose last record lies past the end is discarded whole. Records beyond the end MUST NOT be
applied, even if some of them would validate.

**Redo** (parallel). The dispatcher reads records from `redo_lsn` to the end. A page record goes to worker
`hash(pid) mod N`, so each page's records are applied in LSN order by one worker. File records (`FILE_*`) are
applied by the dispatcher itself after a barrier, before any later page record is dispatched. For each page record
the worker:

1. reads the page, or takes a zero page if the segment is shorter than it;
2. if the page fails its checks (§3.1) — a torn or misdirected write — and the record is `FPI` or `PAGE_INIT`,
   replaces the page from the record; if the page fails and the record is any other kind, recovery MUST stop with a
   corruption error that names the PID;
3. otherwise applies the record only if `page_lsn` < the record's `lsn`.

**Undo.** Every transaction still in the table is a loser. For each, follow its rollback chain from `undo_next`
(when it has one) or `last_undo`, apply each record's inverse as in §5.4, and log that as ordinary page records
flagged COMPENSATION carrying the next `undo_next`. Then write `TXN_ABORT`. Losers' rows stay write-locked by
their `txn` throughout, so the database MAY open for new transactions as soon as redo finishes, with undo running
in the background (as InnoDB does). A crash during undo leaves COMPENSATION records whose `undo_next` keeps undo
from being repeated (ARIES's CLRs).

After recovery the engine MUST write a PAD from the end of the log to the next block boundary before it appends,
MUST delete or ignore every later segment, and MUST take a checkpoint before CONTROL's `state` reads 1 again.

### 6.10 Rollback at run time

A rollback at run time is the undo phase for one transaction: the same chain walk, the same COMPENSATION records,
and then `TXN_ABORT`.

### 6.11 Backup, PITR and replication

All three consume the WAL defined above, with no second format.

1. **Online backup** copies CONTROL, every segment of the live generation and the WAL from the backup's starting
   checkpoint `redo_lsn` to the stop LSN. A restore sets `min_recovery_lsn` to the stop LSN, and the database MUST
   NOT accept connections before redo passes it. FPIs make the torn copy of a page that was being written during
   the copy harmless, exactly as for a crash.
2. **PITR** replays archived WAL up to a target: an LSN, a `RESTORE_POINT` or a `commit_time`. It then starts a new
   timeline with `TIMELINE_SWITCH`, so the abandoned future and the new history cannot be confused.
3. **A replica** receives the WAL byte for byte, applies redo continuously and never runs the undo phase. A
   replica's snapshots read undo pages that redo produced, so a replica serves the same versions the primary did.
   `UNDO_TRUNCATE` arrives in the stream. A replica that must keep a snapshot past it either has fed its horizon
   back to the primary or cancels the snapshot.

---

## 7. Format versioning

### 7.1 What carries a version

| carrier | field | meaning |
|---------|-------|---------|
| every page | `format` | the format this page's bytes follow |
| CONTROL | `format_current`, `format_min` | the database's format range |
| every WAL segment | `wal_format` | the record format of the segment |
| every WAL record | `rec_format` | the record's own format |

The v1 format is 1. A format number increases with every change to any byte layout in this document.

### 7.2 The compatibility promise

1. A release that writes format N MUST read every page in formats N, N−1 and N−2, and MUST replay WAL of formats N
   and N−1. That is enough to recover a crash of the previous release and upgrade in place.
2. A release MUST refuse to open, even read-only, a database whose `format_current` is higher than it knows.
3. A release opening a database with `format_current` < N MUST open it **read-only** unless the operator asked for
   the upgrade (`terndb open --upgrade-format`, or the equivalent C-API flag). It is the operator, not the binary,
   who decides when an old release can no longer open the data.
4. There is never a dump and restore between formats.

### 7.3 The in-place upgrade

1. Recover fully under the old WAL format, and take a checkpoint.
2. Log `FORMAT_UPGRADE(from, to)`, then write CONTROL with `format_current` = N. From here on every new page is
   written in format N, and every WAL segment in the new `wal_format`.
3. **Lazy conversion:** a page read in an older format is converted to format N in memory. Its first modification
   logs a `FORMAT_PAGE` (or an FPI of the converted image), so the page reaches disk in format N.
4. **Background sweep:** a low-priority task walks every allocated page in PID order and converts each one that is
   still old, through the same `FORMAT_PAGE` path, while the database is online.
5. When the sweep finishes, log `FORMAT_MIN(N)` and set CONTROL `format_min` = N.

A crash at any step is ordinary recovery: the WAL records make each step redoable, and a page is in either format
with its own `format` field saying which.

Schema versions (§3.9) are separate from format versions. An `ALTER TABLE` never changes `format`.

---

## 8. The in-memory mode

### 8.1 What differs

1. Every page lives in a frame allocated from memory. There is no buffer manager on the path: no eviction, no page
   table lookup for child references (every child reference is swizzled), and no page I/O except a snapshot.
2. The frame descriptors, latches, page formats, row format, undo log and MVCC are **identical** to on-disk mode.
   PIDs are still assigned by the allocation bitmaps, which live in memory, so every page has the PID it would have
   on disk.
3. Checksums are computed only when a page is written to a snapshot.
4. Overflow PIDs and table roots resolve through a per-database **page map** (PID → frame). It MUST be a persistent
   radix tree, path-copied on write, so a clone can share it (§8.4).

Per durability setting:

| durability | files written | survives a crash with |
|------------|---------------|-----------------------|
| `none` | none (an explicit `SNAPSHOT TO <dir>` may still write one) | nothing |
| `snapshot` | CONTROL + periodic generations | the last complete snapshot |
| `log` | CONTROL + generations + WAL | every acknowledged commit |

### 8.2 The snapshot format

**A snapshot is a data generation**: a `data.<g>` and an `undo.<g>` directory in exactly the on-disk layout of §2,
holding every allocated page of both spaces as of one instant. There is no separate snapshot file format.

To take one, fork-free:

1. **Barrier.** Briefly stop page mutation (every writer holds a shared mutation guard for the duration of a
   mini-transaction; the snapshotter takes it exclusively), and record the capture instant: `snapshot_lsn` (the
   WAL end, in `log` mode), the active-transaction table, `next_txn_id`, `next_oid` and `undo_low`. Increment the
   global snapshot epoch E and release the barrier.
2. **Copy-on-write.** Every frame records the epoch at which it was last captured. A writer about to modify a frame
   whose captured epoch is below E, and which the snapshotter has not yet written, MUST first copy the frame's
   bytes to the snapshotter (or write it itself) and stamp it E.
3. **Stream.** The snapshotter writes every allocated page in PID order into the new generation's segments, taking
   each page's pre-E copy where one exists and the live frame (stamped E) where none does. The page is unswizzled
   and checksummed as in §3.8.
4. **Seal.** It writes a `CHECKPOINT_END` record holding the barrier's active-transaction table and counters, with
   `redo_lsn` = `snapshot_lsn`: appended to the WAL in `log` mode, or as the only record of a fresh WAL segment in
   `snapshot` mode. It syncs every file of the generation, the WAL and the directories, and only then writes CONTROL
   with `generation` = g, `snapshot_lsn`, `checkpoint_lsn`, `redo_lsn`, the counters and `undo_low`. The previous
   generation is deleted after CONTROL is durable.

The pages of one generation are therefore the state at a single instant, uncommitted changes included, along with
the undo that can remove them. *Rationale:* a transaction-consistent dump would need a quiescent database; an
instant image plus undo is exactly what crash recovery already handles. HyPer's fork snapshots were rejected
because `fork` stalls on large page tables and doubles memory under heavy writes.

**In `log` mode a snapshot is the checkpoint.** WAL below `snapshot_lsn` MAY be removed once the generation is
sealed. No FPI is ever needed in memory mode, because a generation is written to fresh files and is never live
until it is synced in full, so no page of it can be torn.

### 8.3 Opening, and the promotion invariant

Opening a memory-mode database (in memory mode) loads generation g into frames, then runs recovery exactly as
§6.9 describes: redo from `redo_lsn` (empty in `snapshot` mode), then undo of the transactions in the active table.

**Promotion invariant:** a directory written in memory mode, under `snapshot` or `log` durability, MUST open in
on-disk mode with no conversion step, and MUST then show exactly the committed state memory mode would have
recovered. It holds because the generation is the §2 layout, its pages are in the §3 formats with PIDs and no
swizzled bits, the WAL is the §6 format, and redo over an untorn image with `page_lsn` checks is idempotent. The
only change on promotion is CONTROL `mode` (0) and `durability` (2), written by the first on-disk open. The reverse
(an on-disk database opened in memory mode) MUST also work, as a load of every page into frames.

### 8.4 Clones

A **clone** is a new database that starts as an exact copy of another and then diverges.

**Memory mode clones in O(1).** The clone takes the snapshot barrier (§8.2 step 1), shares the source's page map
root and frames, and increments the root's reference count. A frame is immutable while its reference count is
above 1. A writer to a shared frame copies it, and every shared ancestor on its path, before writing (copy-on-write
path copying with reference counts, as in Rodeh's CoW B-trees and LMDB). The copy keeps its PID: the two databases
have separate page maps over the same PID numbers. The clone then rolls back the transactions that were active at
the barrier, as recovery would. The clone's cost is the barrier plus a constant, which is microseconds for a test
fixture of any size.

**On-disk mode clones in O(files).** An on-disk clone MUST take a checkpoint that leaves no dirty page, then
reflink every segment, the WAL from `redo_lsn` and CONTROL into a new directory (`clonefile` on APFS, `FICLONE` on
XFS and Btrfs), with a fresh `db_uuid` written into the clone's CONTROL and segment headers through ordinary logged
page writes on its first open. This is milliseconds, not microseconds, and it fails with an error on a filesystem
without reflink. A clone of a memory-mode database MAY be promoted like any other.

---

## 9. Durability invariants

These are the properties a deterministic simulation MUST check. The simulation can crash at any instruction, tear
any write at any 4 KiB boundary, drop the unsynced suffix of any file, return an error from any read, write or
sync, and reorder writes issued before one sync. "Recover" means run §6.9 (or §8.3) on what persisted.

**Commit and atomicity**

- **D1.** Every transaction whose commit was acknowledged is present in full after recovery.
- **D2.** No effect of a transaction that did not commit is visible after recovery, including undo records,
  index entries and overflow chains, which purge or rollback MUST reclaim.
- **D3.** After recovery, the committed state equals the state produced by applying the committed transactions in
  the LSN order of their commit records.
- **D4.** A mini-transaction is applied either wholly or not at all. In particular, every B+tree after recovery
  satisfies D20.

**Logging**

- **D5.** No page reaches its segment with a `page_lsn` above the durable WAL end at the time of the write.
- **D6.** `durable_lsn` never decreases, and no block wholly below it is ever written again.
- **D7.** For every page modified after a checkpoint's `redo_lsn`, the first record touching it at or after
  `redo_lsn` is an `FPI` or a `PAGE_INIT`.
- **D8.** Every page record touches exactly one page and depends on no other page's contents.
- **D9.** Redo is idempotent: recovering the same persisted state twice, or crashing during recovery and
  recovering again, gives byte-identical pages.
- **D10.** Parallel redo with any worker count N ≥ 1 gives byte-identical pages to N = 1.

**Torn writes and I/O errors**

- **D11.** A torn data page is always detected (by checksum or PID) before any of its fields is used, and is
  either restored from an FPI or `PAGE_INIT` in the redo range or reported as corruption naming its PID. A torn
  page is never silently used.
- **D12.** A torn or partially persisted WAL tail ends the log at the first invalid record, and loses no
  acknowledged commit.
- **D13.** A torn CONTROL write leaves the other slot valid, and the open uses the newest valid slot.
- **D14.** **A failed sync is fatal.** After any sync of the WAL, a segment or CONTROL returns an error, the engine
  MUST NOT acknowledge any commit that the sync covered, and MUST stop and recover from storage rather than retry
  the sync. (A retried sync may report success over pages the kernel has already dropped.)
- **D15.** A read error or checksum failure during normal operation fails the operation that needed the page, with
  an error naming the PID, and never returns wrong data.

**MVCC and undo**

- **D16.** For every snapshot S and every row, reconstruction (§5.4) gives the same version before and after any
  crash and recovery that S survives (a replica snapshot), and the same version at any worker count.
- **D17.** No live snapshot ever needs an undo address below `undo_low`.
- **D18.** After recovery, no row's `txn` names a transaction that is neither committed, nor active, nor a loser
  being rolled back.
- **D19.** Undo addresses, LSNs, transaction ids and object ids are strictly increasing, and survive every crash
  without reuse. (Recovery raises each counter above every value it sees.)

**Structure**

- **D20.** Every B+tree satisfies: keys strictly increasing within a node; every key within its node's fences;
  `prefix_len` per §3.5; each child's fences equal its parent's bounding separators; all leaves at level 0; and
  every child reference on disk is a PID with bit 63 clear.
- **D21.** Every allocated page is reachable from exactly one place (a tree, an overflow chain, a live undo
  segment, or a bitmap page itself), and every reachable page is allocated. A page lost by a crash between
  allocation and use is a leak that the check MUST report.
- **D22.** Every secondary index entry that is not delete-marked corresponds to a row whose visible version, for
  some live snapshot, has those index columns; and every visible row has its entries.

**Modes and versions**

- **D23.** (Promotion.) For every crash point of a memory-mode database in `snapshot` or `log` durability, opening
  the directory in memory mode and in on-disk mode gives the same committed state.
- **D24.** A sealed generation is never modified, and CONTROL never names a generation that is not fully synced.
- **D25.** A crash at any point of a format upgrade (§7.3) recovers to a database that opens, every page of which
  has a `format` in [`format_min`, `format_current`].
- **D26.** A clone and its source never observe each other's writes made after the clone.

---

## 10. Open questions

1. **Collations in keys.** v1 compares `text` keys by binary UTF-8. Should a collated key store an ICU sort key
   (correct order, larger keys, and an ICU dependency that has to be sysl) or stay binary, with collation applied
   only in the executor?
2. **Large nested relations.** v1 stores an oversized nested relation as one immutable overflow chain, so updating
   one element rewrites the whole value. Is form 1 (a private B+tree per value, keyed by element ordinal) wanted in
   v1, or only once a workload shows the cost?
3. **Overflow values in the WAL.** v1 logs overflow chains in full (`OVERFLOW_WRITE`), which doubles the writes of
   a large value, as in Postgres and InnoDB. The alternative is to write the chain directly and sync it before the
   commit record, logging only its PIDs. Which?
4. **Replica feedback.** Should a replica's oldest snapshot hold back the primary's `undo_low` (the analogue of
   Postgres's `hot_standby_feedback`), or should replica snapshots be cancelled on `UNDO_TRUNCATE`? This decides
   whether a reporting replica can stall the primary's undo.
5. **Writing the previous format.** §7.2 opens an un-upgraded database read-only. Should a release also be able to
   *write* format N−1, so a binary upgrade and a format upgrade can be rolled back separately, at the cost of two
   page writers per release?
6. **The undo stream is one global append point.** Is a per-core undo lane (with the horizon taken as the minimum
   over lanes) wanted in v1, or only once the fetch-and-add shows contention?
7. **On-disk clones.** §8.4 gives on-disk mode reflink clones in milliseconds. Are in-engine copy-on-write clones
   for on-disk mode wanted too? They would need reference counts on disk.
8. **Asynchronous commit.** Should a session be able to accept commit without waiting for the sync (Postgres's
   `synchronous_commit = off`), losing a bounded window on a crash but never consistency?
9. **Encryption at rest.** Not designed. Reserving a CONTROL feature flag and a per-page key-version field now
   would avoid a format bump later. Should v1 reserve them?
