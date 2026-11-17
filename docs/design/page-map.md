# The page map

`sh/sysl/terndb/page_map.sysl`. The in-memory mode resolves overflow PIDs and table roots through a per-database
page map from PID to frame, which `on-disk-format.md` §8.1 rule 4 requires to be a persistent radix tree,
path-copied on write, so that a clone can share it (§8.4). `PageSpace` keeps its frames in one.

## Shape

- **A `PageMap` is a small value**: one root per page space (bits 62–60 of the PID, §2.1), each space's height in
  levels, and the count of mapped PIDs. Copying it copies eight counted references, which is `clone`.
- **Fan-out 64.** A node consumes 6 bits of the 60-bit page number, so 10 levels cover a whole space exactly (60 =
  10 × 6); 256 would leave a half level (7.5) and 16 would need 15 levels. Data segment boundaries (2¹⁶ pages) fall
  inside level 2, so a database of a few GiB is 3 levels deep. A node is 64 counted references, about 520 bytes:
  copying one per level on a write is cheap next to the 16 KiB page that write touches.
- **A tree per space** keeps the data space shallow: an undo PID has bit 60 set, and one tree over 63 bits would
  make every data lookup walk ten levels once any undo page were mapped.
- **Variable height.** A space's height is the least that covers its largest page number (1 for 0–63, 2 below
  4,096, … 10). A set beyond the height puts the old root under slot 0 of new nodes; the old tree is untouched.
- **Nodes are never written once built.** `PmInner` holds 64 child slots and `PmLeaf` 64 frame slots, each with the
  count of occupied slots.
- **Canonical shape.** No node is empty, and a root is a leaf or has a child other than slot 0. A remove prunes
  every node it empties and lowers the root while its one child is in slot 0, so the shape depends on the set of
  PIDs alone, never on the history that built it. `check_page_map` checks this, every node's count, each frame's
  own PID (§3.8 rule 3) and `len`.

## Operations

`get`, `set` (answers a new map), `remove` (answers a new map, or the same map when the PID is absent), `clone`,
`seek` (the least mapped PID at or after a PID), `last`, and `iter` / `iter_from`, a cursor in PID order over the
map it was made from. A set copies exactly one node per level of the path and shares every other node with the old
map; a set that grows the tree adds the new levels on both paths. A PID with bit 63 set is never found, and setting
one fails the precondition.

## What §8.4 needs from it

A clone copies the `PageMap` value: O(1), and the two maps then diverge by path copying, neither seeing the other's
writes. The frames are shared too, by counted reference. Copy-on-write of a *frame* (a frame is immutable while
its reference count is above 1) is the clone item's: the writer copies the frame, sets the copy at the same PID in
its own map, and rewrites the swizzled child reference in each B+tree ancestor, which are themselves copied on the
same rule. Nothing in the map's structure stands in the way of that.

## Deferred

- Reference counts read by a writer to decide copy-on-write (§8.4), and clones themselves.
- Latching: the map is single-threaded, like the store. Readers of an old root need no latch, since nothing they
  reach is ever written.
