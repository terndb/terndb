# The buffer pool

`sh/sysl/terndb/buffer_pool.sysl` is the page cache between the engine and page I/O (`page_io.sysl`), the first
piece of the durable store. It holds a fixed number of 16 KiB frames; a page is **fixed** into a frame under a
latch, read through its segment's `PageIo` on a miss, used, and **unfixed**. Nothing in the engine's statement path
uses it yet: the in-memory store (`store.md`) still runs on `mem_store`. Its tests are `buffer_pool_tests.sysl`
(each edge case by name) and `buffer_pool_property_tests.sysl` (a seeded model test under injected faults).

## Structure

- **Frames.** `buffer_pool(capacity, source, wal)` allocates `capacity` frames (at least one) in one buffer, and a
  **frame descriptor** beside each: the PID it holds, its pin count, whether its one fix is exclusive, whether it
  is dirty, its `rec_lsn`, and the clock's reference bit. The latch and the eviction state live in the descriptor,
  never in the page's bytes (`on-disk-format.md` §3.1). A page table maps each resident PID to its frame.
- **Segments.** A PID names its page space and page number (§2.1); `segment_of(pid)` is the key
  `space << 60 | segment`, the segment being the data page number `>> 16` or the undo page number `>> 12`, and
  `pid_slot` the page within it. The pool asks a `SegmentSource` for each segment's device the first time it needs
  it and keeps one `PageIo` per segment. `FileSegments(dir, generation)` is the source over the host's files, named
  as §2 gives: `data.<gen>/<n:08x>.tseg` and `undo.<gen>/<n:012x>.tund` (`segment_path`). It opens files that
  exist; creating a segment, and syncing its directory (§2 rule 3), belong to the code that extends the file set.
- **The log.** The pool sees the WAL only through `WalHorizon`: `durable_lsn`, and `flush_to(lsn)`, which makes the
  log durable at least that far or fails.

## Fixing and latching

- `fix(pid, mode)` answers a `PageFix` (PID, frame, mode). A hit sets the reference bit and adds a pin. A miss
  takes a victim frame (below), reads the page into it through `PageIo.read`, which checks the checksum and the PID
  (§3.1 rule 1), and enters it in the page table.
- `fix_new(pid)` is for a page being created: it takes a frame without reading, zeroes it, writes the PID into the
  header (`PageIo.write` requires the header to name the PID it is written as) and fixes it exclusively. A page
  that has been allocated but never written reads as zeros, which fail the checksum, so a new page must come
  through here and not through `fix`. A PID already resident is a contract violation.
- **Latches.** `LatchShared` admits any number of fixes; `LatchExclusive` admits one. The pool is single-threaded,
  like the store (`store.md` § Concurrency), so a conflicting request comes from the thread that holds the latch
  and would wait for ever: it **stops the program** rather than deadlocking. This covers a shared or exclusive fix
  of a page fixed exclusively, and an exclusive fix of a page fixed at all.
- `page(h)` is the page's bytes; `page_mut(h)` is for an exclusive fix only.
- `unfix(h)` hands back a fix that changed nothing. `unfix_dirty(h, lsn)` hands back an exclusive fix that applied
  the log record at `lsn`: the page's `page_lsn` becomes `lsn` (§6.5 rule 4), the frame is dirty, and `rec_lsn`
  is set if the frame was clean, so `rec_lsn` is the LSN that first dirtied the page since it was last written (a
  checkpoint's dirty page table). An LSN below the page's own is a contract violation, since LSNs only increase
  (§9 D19).
- Handing back a fix the pool does not hold stops the program: a frame past the pool, a frame now holding another
  page, a frame with no pins, or a mode the frame is not latched in. Pins are counted, not tracked per fix, so a
  second unfix of one shared fix while another shared fix of the page is live is not detected.

## Eviction

The policy is the **clock**: the spec names none, and the clock keeps the eviction state to one bit in the
descriptor. The hand sweeps the frames from where it last stopped. It passes over a fixed frame, so **a fixed page
is never evicted**. It takes a free frame at once, clears a set reference bit and moves on (a second chance), and
otherwise takes the frame. Two full sweeps that find nothing mean every frame is fixed, and the fix fails with
**`resource_exhausted` (`T1008`)** and detail `frames`: the spec has no waiting, and a single thread waiting for its
own pins would wait for ever. A refused fix reads nothing and evicts nothing.

A dirty victim is written before its frame is reused. If that write fails, or the log refuses to flush for it, the
fix fails with that error and the victim stays resident and dirty.

## Writing pages back

- **The WAL rule.** Before any page is written, the pool makes the log durable up to its `page_lsn` (§6.5 rule 1,
  §9 D5): if `durable_lsn` is below it, it calls `flush_to`. A failed flush fails the write, which is not attempted:
  the page stays dirty. A log that reports success but is still short stops the program.
- The write is made from a copy of the frame, and the copy is what `PageIo` seals with the checksum, so the frame's
  own bytes are never changed by a write (§3.1 rule 3). The same copy is where swizzled references would be
  replaced by PIDs (§3.8 rule 4).
- `flush_page(pid)` writes one page if it is resident and dirty. `flush_all()` first makes the log durable to the
  highest `page_lsn` among the dirty pages (one log flush), then writes every dirty page in frame order, then syncs.
  Flushing a page fixed exclusively stops the program: its caller is in the middle of changing it. A shared fix
  does not stop a flush.
- `sync()` syncs every segment written since its last sync, in segment-key order. A write leaves its segment owing
  a sync even when the write fails, since a device may have taken part of it.

## Errors

| what | code | details |
|---|---|---|
| a read the device failed, a segment that cannot be opened, a write that failed | `D4004` `io_error` | `errno`, `file`, and `pid` for a page |
| a page whose checksum or PID is wrong, a swizzled PID, a PID of a reserved space | `D1001` `data_corrupted` | `pid` |
| every frame fixed | `T1008` `resource_exhausted` | `frames` |

A read that fails, either way, leaves the frame free and the page not resident; the next fix of it tries again
(§9 D15). A PID that names no segment is refused before any victim is chosen.

**A failed sync is fatal** (§9 D14). From then on every I/O of the pool, on every segment and not only the one that
failed, is refused with `io_error`: a miss, an eviction that must write, a flush and a sync. A hit needs no I/O and
still succeeds, and a new page may still take a free frame; neither can reach the disk. The engine is to stop and
recover from storage.

## Swizzling

Left out at this level. §3.8 makes swizzling a MAY: the buffer manager *may* replace a child reference by its
frame's address. The pool never does, so every page in a frame holds PIDs and rule 4 holds trivially; a tree on top
of the pool finds its children through `fix`. Adding it later is local: the write already goes through a copy,
which is where unswizzling happens, and the descriptor already records its PID (§3.8 rule 3).

## Deferred

- Threads: shared latches that really wait, optimistic latching with a version word, and a concurrent page
  table. The pool is single-threaded, as the store is.
- A background writer and checkpoints: `rec_lsn` is kept for the dirty page table, but nothing reads it yet.
- Creating segments (`FILE_EXTEND`, §6.4) and opening the file set from `CONTROL` (§2.2).
- Wiring the pool into the store and the B+tree.
