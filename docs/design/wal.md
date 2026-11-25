# The write-ahead log

`sh/sysl/terndb/wal_record.sysl`, `wal_segment.sysl`, `wal.sysl`, `wal_reader.sysl`. The format, the writer and the
reader of `on-disk-format.md` §6.1–§6.7, and the end-of-log rule of §6.9. Recovery itself — analysis, redo, undo —
is built on top of this and is not here.

## Layers

- **Records** (`wal_record.sysl`). LSN arithmetic; a typed `WalBody` with one variant per §6.4 kind, every field
  as the table names it; `wal_encode` writes the 32-byte header, the payload, `undo_next` for a compensation
  record and zero padding to a multiple of 8, and seals the CRC over [4, len); `wal_stamp` sets the LSN and
  `MTR_CONT` once the appender knows where the record goes, and seals it again; `wal_frame_valid` is the §6.9
  frame check; `wal_decode` parses a frame that passed it.
- **Segments** (`wal_segment.sysl`). File names, the 4 KiB header block and its check, and `WalDir`, the seam a
  log's directory goes through, as `BlockDevice` is for one file: `FileWalDir` is the host's directory and
  `SimWalDir` holds a `SimDisk` per file, so every fault of `isolation.md` §7.3 can be injected into the log.
- **The writer** (`wal.sysl`). `Wal`: appends, group commit, segment creation and recycling, reopening.
- **The reader** (`wal_reader.sysl`). `WalReader`: a forward scan from an LSN.

Every write is `BlockDevice.write_blocks`, whole 4 KiB blocks within one page, added to the seam for the log:
writing the whole 16 KiB page a flush ends in would rewrite blocks already durable, which §6.7 rule 3 and D6
forbid. The simulator tracks which sectors each cached write covers, and a crash, a tear or a failed sync brings
only those to the media.

## Appending

- **One lock for the buffer, one for the files.** `state` guards the bytes of LSNs [base, end) not yet written and
  the counters; it is held for the moment an append reserves its place and copies its records in, which has the
  effect of §6.7 rule 1's fetch-and-add. `io` guards the directory and the open segment and is held by the one
  flusher while it writes and syncs, with `state` free, so appends go on during a sync.
- **Records are encoded outside the lock** with LSN 0, then stamped with their LSN and `MTR_CONT` under it.
- **A mini-transaction is contiguous in one segment** (§6.3 rule 2): it is reserved in one hold of the lock, and
  when the rest of the segment cannot hold all of it a pad fills the segment and it starts the next. No record,
  pad included, ever comes between two records of one mini-transaction. A mini-transaction longer than a segment
  holds (2²⁶ − 4,096 bytes) is refused with `D2003`, as is a single record that long.
- **The caller's `MTR_CONT` and LSN are ignored**; the appender sets both. A caller may not append a pad: the log
  writes its own.

## Where a record can start

- **Never with less than 32 bytes left in its segment.** A record that ends 8, 16 or 24 bytes before the segment's
  end leaves too little room for a pad's header, so those bytes are left zeros, not a record, and the next record
  is the next segment's first. The reader skips them the same way (`wal_next_record`). §6.2 rule 2 says this now.
- **A flush ending fewer than 32 bytes before a block boundary pads through the next block too**: its pad is the
  rest of this block plus the whole of the next, since a pad cannot be shorter than its header. §6.7 rule 3 says
  this now. The pad always fits: a segment ends on a block boundary and a record never starts with fewer than 32
  bytes left in its segment, so fewer than 32 left in a block means a whole block follows it in the segment.

## Group commit

A committer whose LSN is not yet durable takes `io`. When it has it, it looks again: the flush that held `io` may
already have covered its record, and then it returns with no I/O of its own. Otherwise it is the flusher: it pads
to the block boundary, takes the whole buffer, lets `state` go, writes block runs page by page into every segment
the range crosses, syncs each segment it wrote, and only then advances `durable_lsn` (§6.7 rule 4). So every commit
appended before a flush begins shares that flush's one sync, and the committers waiting meanwhile queue on `io`
and mostly find their work done.

The spec's `durable_lsn` is the name used (`durable_lsn()`); it is the "flushed LSN" recovery and the buffer
manager ask for.

## Failure

A write or a sync that fails is fatal (D14): the log records the error, `durable_lsn` stays where it was, and
every later call — append, wait, flush, recycle, even a wait for an LSN that was already durable — answers the
same `D4004` naming the segment. Nothing is retried. The way on is to reopen from storage.

## Segments

- **Named for the LSN of their start**, `k × 2²⁶`, header included, which is the header's `first_lsn` too.
- **Created ahead of their first record and preallocated**: the header is written, every page the file does not
  reach is written as zeros, and the file and the directory are synced before a record goes in, so a later sync
  of the segment never changes its size. A new log creates segment 0 at once.
- **`prev_segment_crc`** is written from the previous segment's header (read from its file when the writer did not
  write it). The reader does not check it: §6.9 does not list it among the ends of the log.
- **Recycling** (`Wal.recycle(bound)`, the caller passing the lowest retention bound of §6.6) renames every
  segment wholly below the bound, never the one being written, to the next number past every segment there is,
  and syncs the directory. Its records stay; when the log reaches it its header is rewritten and synced, and a
  stale record past the log's end fails the reader's LSN check (§6.2 rule 3). Until then its header names its
  old place, so it counts as missing.
- **The format's segment is 64 MiB.** `wal_config_sized` takes a smaller power of two, down to one page, so the
  tests can roll segments over and recycle them in a few records; the size is not recorded in the segment, and
  a log is written and read with one size.

## Reading

`wal_reader(dir, cfg, from, must_reach)` scans forward from `from`, which must be where a record starts.

- **The end of the log** is the first position where `len` is under 32 (0 included), not a multiple of 8 or
  past the segment; the CRC fails; `lsn` is not the position; the segment file is missing, too short to hold the
  bytes, or its header fails its CRC, magic, database, timeline, term or first LSN. §6.9 lists `len` 0; a `len`
  of 8 to 24 can hold no header and is treated the same.
- **Whole mini-transactions only.** Records are held back until the last record of their mini-transaction is
  read and valid; a trailing one cut by the end is dropped whole, and `end()` is where it began. Pads are never
  handed out.
- **What is corruption** (`D1001`): a frame whose CRC and LSN hold but that does not parse (an unknown kind, a
  payload that ends early or runs on, padding that is not zeros, a flag its kind cannot carry, a
  `TIMELINE_SWITCH` naming another LSN, a pad inside a mini-transaction), and an end met before `must_reach`.
  §6.9 cannot tell a torn tail from a flipped bit in the middle of the log by the bytes alone; the caller can,
  because it knows a position that was durable — the end of the checkpoint it starts from, or a backup's stop LSN
  — and passes it as `must_reach`. With 0 every invalid frame is the end.
- **What is refused**: a segment header or a record of a `wal_format`, `key_version` or `rec_format` this release
  does not read is `D3001`; a read that fails is `D4004`. Both are errors, not the end of the log.

## Reopening

`wal_open(dir, cfg, from, must_reach)` scans to the end E as the reader does, then, before anything is appended:

1. removes every segment past E's, spares included, and syncs the directory — one of them may hold records that
   validate at their own LSN past the end (a later segment written before a crash tore an earlier one);
2. if E's segment is missing or not this log's, removes it too; the writer makes it afresh when the log reaches
   it, and `durable_lsn` is E;
3. otherwise writes a pad **from E to the end of E's segment**, rewriting E's block with its bytes before E as
   they were, syncs it, and starts appending in the next segment.

§6.9 asked for a pad only to the next block boundary. That is not enough: a block written by a flush that crashed
can sit past a lost one, whole and at its own LSN, and a pad to the boundary leaves it there to be read as the
log's once the new records reach it. Padding the whole segment overwrites every such block; the cost is one write
of at most a segment per reopening. §6.9 says this now. A trailing mini-transaction the scan dropped is overwritten
by the pad as well, so its stale `MTR_CONT` records are never joined to new ones.

## The record payloads

- **`PAGE_INIT` always carries both fences**, each as `len` u16 and the bytes, empty for a page that is not a
  node. §6.4 said only "plus the fences for a node"; a length-based "fences present or not" would be ambiguous
  against the padding and a compensation record's `undo_next`. §6.4 says this now.
- **`TIMELINE_SWITCH`'s `switch_lsn` is the record's own LSN**: the body has no field for it, the appender writes
  it, and a reader refuses one that differs.
- **`FPI` codec** is 0 or 1; any other is refused when writing and corruption when reading. `FORMAT_PAGE` carries
  exactly one page.
- **Compensation** is allowed only on page records (§6.9 "ordinary page records flagged COMPENSATION"), never on a
  pad; `undo_next` follows the payload.
- **Width limits** — a key, value, fence, image, patch, undo append or restore point past a u16, more than 65,535
  entries or patches, more than 255 standbys or a name past 255 bytes, a payload past a u32 — are `D2003`
  (`wal_record_unwritable`, added to `errors.md`) when writing.
- **Reserved bytes and flag bits 2–7 are written zero and ignored when read** (§1 rule 3). `rec_format` and
  `wal_format` are 1.

## Tests

`wal_record_tests.sysl` (every kind, at its largest, the layout, every refusal and every malformed frame),
`wal_segment_tests.sysl` (names, headers, both directories, a log over host files), `wal_tests.sysl` (appending,
padding, mini-transactions, rollover, recycling, every end of the log, torn, lost and failed writes, reopening),
`wal_crash_tests.sysl` (a crash before, during and after a flush under every `CrashKeep`), `wal_group_tests.sysl`
(threads), `wal_property_tests.sysl` (random appends, commits, flushes, crashes, recycling and drawn faults over
80 seeds, against a model of what is durable), and `block_writes_tests.sysl` (`write_blocks` on both devices).
