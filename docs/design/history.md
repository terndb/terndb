# The history recorder and checker

`sh/sysl/terndb/history.sysl`, `history_check.sysl` and `history_workloads.sysl` are the Elle-style harness of
`isolation.md` §7.1–§7.2, run against the in-memory store (`store.md`). The recorder writes the history of
§2.5; the driver interleaves logical clients against the store by a seed; the checker infers each object's
version order from the recorded values and finds the phenomena of §2.4 that a transaction's level forbids.
Their tests are `history_tests.sysl` (recorder, JSON lines, driver), `history_check_tests.sysl` (the checker
on hand-built histories) and `history_workloads_tests.sysl` (the workloads against the real store, and
against each fault-injected store).

## The recorder

- **`HEvent`** is one record of §2.5: index, time, process, node, type (`invoke`, `ok`, `fail`, `info`),
  level, sync, the white-box `txid`, `snapshot` and `commit` (0 where absent), `error` (the §5.2 code's
  name) and `value`, a list of **`Mop`**s.
- **`Mop`** is one micro-operation: `append`, `r` (an item read: `null` for no row, an integer, or a list),
  `w` (a write; `["w", k, null]` is a delete), `pr` (a predicate read and the keys it returned) and one
  operation §2.5 does not list, `["add", k, d]`, a relative update `SET v = v + d` whose result the client
  does not see (W2's increments, W3's transfers).
- **`history_jsonl`** writes one JSON object per line and **`parse_history`** reads it back; a `null` read
  result is a missing row in a completion and not yet known in an `invoke`.
- **`SimClock`** is the harness's clock in simulated nanoseconds; as an engine `Clock` it reads
  milliseconds. The driver moves it 1000 ns per scheduling step and the recorder 1 ns per record, so times
  are strictly increasing and a history is a function of its seed.

## The driver

`drive` runs a list of `Plan`s per client against a `Bench`, a fresh store with three tables (`lists` for
list keys, `regs` for registers, counters and accounts, `grp(k, g, v)` for W4's rows). A plan is a level, a
read-only flag, whether a retryable failure re-runs it (§5.3), and `HStep`s. `SAppend`, `SWrite` and
`SInsert` take a fresh unique value when the transaction is invoked, so a retried attempt writes its own
values; `SThen` is replaced, when its turn comes, by the steps its function makes of what was read so far
(a transfer that refuses an overdraft, a doctor going off call).

At each scheduling step a seeded generator picks one client whose wait, if any, is over; it takes one
action: begin (recording `invoke` with the operations known in advance), one statement, or commit. A `Wait`
answer parks the client until the holder ends, and the statement is then called again; a deadlock aborts
the waiter in the store. A failure records `fail` with the attempted operation included; a retryable one
re-runs the plan up to `MaxAttempts` times. A commit records `ok`, or, for `lost_acks` commits in a
thousand, `info`, whatever the store did. Setup plans run alone first and a final serializable reader of
every key runs alone last, so every committed write is observed.

## The checker

`check_history` gathers each transaction from its invoke and completion. An `info` transaction whose write
a committed transaction read is resolved as committed; one nobody read is left unknown: under §5.4 it may
have committed, but its writes sit nowhere in a version order, so neither reading adds an edge, and its
writes are never counted as an aborted read.

**Black box** (never reads `txid`, `snapshot`, `commit`; `strip_white_box` proves it):

- *List keys.* A read by a transaction has its own earlier appends taken off its end (they must be there:
  `internal`). Every read must be a prefix of the longest (`incompatible-order`); the longest gives the
  version order, consecutive writers its ww edges. A read's last element names its wr edge, the next
  element its rw edge. An element nobody appended is `garbage`, one appended by an aborted transaction
  `G1a`, a non-final append of another transaction without its final one `G1b`, an element twice
  `duplicate-elements`.
- *Registers.* Written values are unique, 0 (or no row) is the initial version. Version order is inferred
  from: the initial version first; a transaction's read followed by its write of the same key; a delete
  after the row's insert (keys are never reused). A read names its writer (wr) and anti-depends on every
  version known to follow it (rw); an rw edge to a later-than-next version is in the closure of the true
  ones and has the same number of rw edges, so cycle classes are unchanged.
- *Predicate reads* `g = n` over rows whose group is set at insert: §7.2 W4's edges. A returned row
  read its insert (wr) and anti-depends on its delete (rw); a committed insert into the group the result
  lacks, of a row never deleted, anti-depends on the read. A lacking row that was deleted is ambiguous and
  gives no edge.
- *P4*: two committed transactions read the same version of a key (their last read before their first write
  of it) and both wrote it.
- *G-stale*: a writer acknowledged before the reader was invoked, whose version is known to follow the one
  read (per transaction; per statement at read committed is not recorded, see below).
- *Counters* (I14): a counter read lies between the increments acknowledged before the reader began and
  every increment that may have committed before it ended (`lost-increment`).
- *I16*: a transaction with no writes, or one at read committed, aborted with `serialization_failure`.
- *Duplicates*: two transactions that did not abort writing one value to one key (`duplicate-write`); a
  retry reusing an aborted attempt's value is not one.

**Cycles.** The DSG's ww, wr and rw edges (predicate ones marked), so edges between consecutive committed
transactions of a process, and rt edges between serializable transactions. Each class is searched only among
the transactions whose level forbids it (§3.4): G0 and G1c among all, G-single and G-nonadjacent among
snapshot and serializable, G2-item (named A5B when it is two rw edges between two transactions) and G2 among
serializable. Session cycles take each level's strength (no rw at read committed, at most one at snapshot,
any at serializable); G-rt is serializable only. The same classes are then searched among every committed
transaction and reported as *permitted* where the cycle includes a transaction whose level allows it.
Tarjan's components prune the search to edges inside a cycle; a breadth-first search over (transaction, rw
edges so far, last edge rw) then finds one cycle per class through a seed edge, keeping to the class's rule.
A transaction in a reported cycle seeds no further one.

**White box** (`CheckOptions.white_box`): I5, every pair of versions the black box ordered is in `commit`
order; and at snapshot and serializable, every read is exactly the newest version committed at or before
its `snapshot` (`wb-read`), for lists, registers and predicate reads.

## The workloads

W1 list-append, W2 rw-register with counters, W3 bank (single-statement total, I13; one statement per account,
exact at snapshot and serializable; overdraft), W3b write skew (a negative pair), W4 phantom and its on-call
shape (an empty group), and W6 the read-only anomaly [Fekete]. W3, W3b and on-call add invariants of their
own to the checker's report; W3 and W3b do not use register inference, since balances repeat.

The fault runs use the store's `Faults`: `skip_validation` (W2, W3b, W4, on-call and W6 red at serializable),
`skip_first_updater_wins` (W1, W2, W3 red at snapshot), `si_statement_snapshot` (W1, W2 red at snapshot,
while W3's one-statement total stays green), and `stale_snapshot`, a snapshot one commit behind the horizon,
which is what publication out of order or acknowledgement before publication look like to a single-threaded
store (G-stale at every level, G-rt at serializable). Each is caught on some seed, and the same seed on the
real store is clean.

## Not yet

- W5 (nested lists and predicates over a nested level) needs list-append objects inside a row's relation and
  predicate edges over the nested level; W7 (replicas), W8 (crash, I22–I25, I28) and W9 (DDL) need replicas,
  the WAL and recovery, and concurrent DDL in the driver. None of those exists in the in-memory store yet.
- I5 has no fault that breaks it: in a single-threaded store commit stamps are taken in publication order.
  The check is proven on hand-built histories only. Likewise I27's error-code rules beyond I16.
- G-stale and the white-box read rule at read committed are per transaction, not per statement: the
  history records one time and one snapshot per transaction.
- Predicate reads are checked for the one shape W4 issues, `g = n` over rows whose `g` never changes.
- The fsync fault (I23) belongs to the disk store.
