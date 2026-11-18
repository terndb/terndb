# The B+tree

`sh/sysl/terndb/btree.sysl` is the B+tree of the in-memory mode, over pages laid out byte for byte as
`on-disk-format.md` §3 specifies; `page.sysl` holds the frame, the page header, the node layout and the data space
with its allocation bitmaps, and `btree_check.sysl` the structural check. Keys are already-encoded,
order-preserving byte strings (§4.6 is a separate layer) compared bytewise; values are bytes. Nothing calls it yet:
wiring it under the store (`store.sysl`) is a later item. Its tests are `btree_tests.sysl` (each edge case by name)
and `btree_model_tests.sysl` (a seeded model test).

## Structure

- **`PageFrame`** is a page's frame and descriptor (§3.1, §8.1 rule 1): the page's own PID, the optimistic latch
  word, and the 16,384 bytes. The latch word is kept and never advanced (§ Concurrency).
- **`PageSpace`** is the data space of one database: a bitmap page per segment (§3.7), the page map from PID to frame
  (`page-map.md`), and the lowest page number that may be free. `allocate` answers the lowest free page, opening a segment (whose
  page 0 is its bitmap and marks itself) when every one is full; `release` clears the bit and drops the frame.
  The PIDs are exactly the ones disk would assign (§8.1 rule 2). PID 0 is segment 0's bitmap, so "none" (§1 rule 8)
  never names a node.
- **`BTree`** is a root PID, which never changes (§3.3, root stability), the owner object id written into every
  page's header, the space, and `BtEvents`, the count of each kind of shape change, which the tests read.
- A child reference is **swizzled** in memory, always (§3.8 rule 2, §8.1 rule 1): bit 63 set and the frame's
  address in the rest, made and followed through `sysl.raw.ptr_cast`. The page map holds the counted reference
  that keeps each frame alive, so an address in a page is valid exactly while its page is allocated. The root is
  found through the page map (§8.1 rule 4).
- **Checksums are not computed in memory** (§3.1 rule 3, §8.1 rule 3): `crc` stays 0, as do `page_lsn` and
  `key_version`. Every page is `format` 1.

## How each operation maps to §3

- **Search.** Inside a node, `node_search` finds the first slot whose key is not below the target. The hints
  narrow the range first (§3.5 rule 4): a hint whose head is below the target's head proves its slot's key is below,
  one above proves it above, since the zero padding of `head` never reverses a comparison. A slot's `head` then
  settles most comparisons before its suffix is read. A target that does not begin with the node prefix lies wholly
  to one side of the node.
- **Descent** takes, at each inner node, the child whose range `[key(i−1), key(i))` holds the key (§3.4), recording
  the path, since nodes have no parent links either.
- **Insert, update, delete in place.** An entry goes to the top of the heap and its slot into the sorted directory.
  When only the fragmented space has room, the node is compacted first (§3.3); when nothing has room, it splits.
  A removed entry, and the tail of a value an update shrank, become `frag_bytes`. An update of equal length
  overwrites in place; a longer one is a remove and an insert. The hints are rewritten on every change.
- **Split.** The contents are taken as an image (whole keys and values), the new entry included, and cut where the
  larger half is smallest. A leaf's separator is the shortest string between the halves (§3.5 rule 3); an inner
  node's middle separator moves up whole. The node keeps the left half and its PID and a new page takes the right
  half, as `NODE_TRUNCATE` and `NODE_BULK` describe (§6.4); the parent's pointer to the node moves to the new page and
  the separator goes in before it, pointing at the node. The root moves both halves to new pages and becomes an
  inner node one level up, in place. Each half's fences are its bounds, and its prefix their common prefix.
- **Merge and redistribution.** After a delete or a shrinking update, a node using under a quarter of the page
  is joined with its left sibling (its right one when it is the first child). If the two, with the separator
  between them taken down for an inner pair, fit one page, they merge: the right node keeps its PID and takes
  everything, the left page is freed, and the separator leaves the parent, which is checked in turn. Otherwise
  the pair is cut again as a split would cut it, and the new separator replaces the old in the parent, which may
  split the parent (only growth follows, so the walk up stops). A root left with no separator absorbs its one child
  and frees it, dropping a level and keeping its PID; an empty tree is an empty root leaf.
- **Scans** are a cursor: forward from the first key at or above the lower bound, backward from the last at or
  below the upper. At a leaf's end the next leaf is found by descending again with the leaf's upper fence; going
  backward, by descending with its lower fence towards the keys just below it (§3.3, fence keys, not sibling
  links).
- **Limits** (§3.6). A key over 1,024 bytes is `key_too_long` (`D2001`); a key and value over 4,000 bytes together
  are `entry_too_large` (`D2002`), since the tree moves nothing out of line; the row layer does that first (§4.7).
  An insert of a key already present answers false and changes nothing; an update or delete of a missing key
  answers false.

## The invariant checker

`check_btrees(space, trees)` walks every tree and fails with `data_corrupted` (`D1001`) naming the page at the
first broken rule: the common header (no checksum, LSN, key version or reserved bits; its own PID; the tree's owner);
type and level agreeing, and each child exactly one level down, so every leaf is at one depth; the fences equal
to the bounds the parent's separators give, infinite fences flagged with offset and length 0, and `prefix_len`
their common prefix; keys strictly ascending, inside the fences, within the key and entry limits; each slot's
`head`; entries and fences inside the heap without overlap, and `frag_bytes` equal to the rest of it; the hints;
a non-root node never empty; a leaf's `extra` 0; every child reference swizzled and naming a frame of the space.
It then checks each bitmap page (header, own bit, `allocated_count` its population) and that the pages reached,
with the bitmaps, are exactly the allocated pages: a page reached twice, a free page reached, an allocated page
reached by no tree, and a frame left on a free page each fail. `check_btree(t)` is the one-tree case.

## Differential tests

`btree_fuzz_tests.sysl` drives seeded random operation sequences against the tree and a sorted model, comparing
every answer and running `btree_check` every 1 to 4 operations. It complements `btree_model_tests.sysl` (mixed
operations over four key distributions) with shapes that stress structure: a tiny key space with heavy churn,
ascending and descending inserts, entries at the key and entry size limits, keys that are prefixes of one another,
and repeated drains and refills down to an empty tree. Each run is a function of `(seed, mode)`, so a failure
message naming the seed, the mode and the operation index reproduces exactly by calling `fz_run(seed, mode, ops,
every)`. A failing sequence is shrunk by lowering `ops` to the failing index and then becomes a fixed regression
test in `btree_tests.sysl`.

## Spec gaps

- **Hints of an empty node.** §3.5 rule 4 names slot `⌊(j + 1) × slot_count / 17⌋`, which does not exist when
  `slot_count` is 0. The rule now says such a node clears bit 2 and zeroes its hints.
- **Inner-node separators on split.** §3.5 rule 3's shortest separator is computable only from the keys on both
  sides, which an inner node does not hold; its middle separator moves up unchanged. The rule now says so.
- **When nodes merge.** The spec sets no fill policy. The tree merges below a quarter of a page when the pair fits
  one page and otherwise redistributes, preferring the left sibling. A node may therefore stay under a quarter
  full; the check requires only that no node but the root is empty. Random runs seldom leave an inner node full
  enough to force an inner redistribution, so a test builds that tree by hand.
- **The offset of an infinite fence.** §3.3 says its length is 0; the offset is written 0 too, and the check
  requires it.
- **`NODE_TRUNCATE` and the prefix.** Truncating a node's upper fence can lengthen its common prefix, so the left
  half's stored suffixes change. The tree rewrites the page whole; the WAL item must say whether the record's replay
  re-encodes the suffixes or logs the page another way.

## Concurrency

Single-threaded, like the store. The latch word sits in every frame descriptor (§3.1, §8.1 rule 2), but no
optimistic latch coupling reads or advances it, and a freed page is reused at once.

## Deferred

- The optimistic latching protocol (version validation, latch coupling on split and merge) and epoch-based
  reclamation, without which §3.7 rule 3's condition (b) does not hold once readers run beside a writer.
- WAL records for every page change (§6.4), so also condition (a) of §3.7 rule 3: a freed PID is reused before
  any `PAGE_FREE` could be durable. Undo and MVCC in the tree.
- Overflow chains (§4.8), secondary-index leaves (`max_txn`, delete marks, §4.9) and the node flag bit 3.
- Snapshots (§8.2), with checksums computed and references unswizzled in the write copy (§3.8 rule 4).
