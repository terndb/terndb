# TernDB transaction isolation

**Status: normative.** This document defines what a TernDB transaction may observe, when it aborts, and what
survives a crash. Code follows this text; a change to observable behaviour changes this text in the same commit.
The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as in RFC 2119. Every MUST in this document is
meant to be checked by one of the properties in §7. A rule that no property can check is a defect in this document.

References used throughout:

- **[Adya]** A. Adya, *Weak Consistency: A Generalized Theory and Optimistic Implementations for Distributed
  Transactions*, PhD thesis, MIT, 1999. The history model, the DSG and the phenomena G0–G2.
- **[Berenson]** H. Berenson, P. Bernstein, J. Gray, J. Melton, E. O'Neil, P. O'Neil, *A Critique of ANSI SQL
  Isolation Levels*, SIGMOD 1995. Lost update (P4), read skew (A5A), write skew (A5B), snapshot isolation.
- **[Fekete]** A. Fekete, D. Liarokapis, E. O'Neil, P. O'Neil, D. Shasha, *Making Snapshot Isolation
  Serializable*, TODS 2005; and A. Fekete, E. O'Neil, P. O'Neil, *A Read-Only Transaction Anomaly Under Snapshot
  Isolation*, SIGMOD Record 2004.
- **[Cahill]** M. Cahill, U. Röhm, A. Fekete, *Serializable Isolation for Snapshot Databases*, SIGMOD 2008 (SSI).
- **[HyPer]** T. Neumann, T. Mühlbauer, A. Kemper, *Fast Serializable Multi-Version Concurrency Control for
  Main-Memory Database Systems*, SIGMOD 2015. Precision-locking validation.
- **[Elle]** K. Kingsbury, P. Alvaro, *Elle: Inferring Isolation Anomalies from Experimental Observations*, VLDB
  2020. History checking from list-append and register workloads.
- **[Cerone]** A. Cerone, A. Gotsman, *Analysing Snapshot Isolation*, PODC 2016. SI as "no cycle with adjacent
  anti-dependencies" (G-nonadjacent).

## 1. Summary of the guarantees

| Level | Name in SQL | Guarantee on the primary | Cost over a snapshot read |
|---|---|---|---|
| **Serializable** (default) | `SERIALIZABLE` | Strict serializability (Adya PL-3 plus real-time order) | predicate log + commit-time validation |
| Snapshot | `SNAPSHOT` | Strong snapshot isolation (PL-SI plus real-time snapshots) | none |
| Read committed | `READ COMMITTED` | PL-2, with each statement reading one real-time snapshot | none |

On a replica every transaction is read-only and sees a consistent prefix of the primary's commit order (§3.6).
`REPEATABLE READ` is accepted as a synonym of `SNAPSHOT`; `READ UNCOMMITTED` is accepted as a synonym of
`READ COMMITTED`. No level ever exposes uncommitted data.

The four choices that shape everything below:

1. **The serialization order is the commit order.** Commit sequence numbers are assigned in one total order, and
   every level's guarantee is stated against that order. This makes serializable transactions strict, makes
   read-only transactions abort-free without deferral, and makes replica snapshots serializable (§3, §5).
2. **Write-write conflicts are first-updater-wins** at snapshot and serializable, because the in-place newest
   version admits at most one uncommitted writer per row (§4.2).
3. **The unit of isolation is the top-level row.** A nested relation is part of its row's version, so a read of
   a nested result is one read of each top-level row, and predicates over nested levels are evaluated against
   whole row versions (§4.5).
4. **A crash loses at most a suffix of the commit order** under every `synchronous` setting, so the recovered
   database is always the state after some prefix of committed transactions (§8).

## 2. The history model

This section is written so that a checker can be built from it alone. §2.1–§2.4 are the abstract model of
[Adya]; §2.5 is the concrete history a test harness records, and how it maps onto the abstract model.

### 2.1 Objects and versions

- An **object** is a top-level row identity: the pair *(table, primary key)*. A table without a declared primary
  key has a hidden row id that serves as its key and is never reused. A nested relation inside a column is **not**
  an object; it is part of the value of its row (§4.5).
- Every object `x` has an initial version `x_init`, which is *unborn* (the row does not exist).
- When transaction `Ti` writes `x` it creates version `x_i`. An insert creates a *visible* version, an update a
  visible version with new values, and a delete a *dead* version. A dead version may be followed by a visible one
  (delete, then re-insert of the same key). Within one transaction only the last write to `x` is the version `x_i`;
  earlier writes in the same transaction are *intermediate versions* `x_i.1, x_i.2, …`.
- Every committed history carries a **version order** `<<_x` per object: a total order over the committed
  versions of `x`, with `x_init` first. **TernDB's version order MUST equal the commit order of the installing
  transactions** (property I5).

### 2.2 Operations

A transaction `Ti` is a sequence of events:

| Event | Meaning |
|---|---|
| `b_i` | begin; at snapshot and serializable this fixes the snapshot `s_i` (§4.1) |
| `r_i(x_j)` | item read: `Ti` reads version `x_j` of object `x` |
| `r_i(P: Vset(P))` | predicate read: `Ti` evaluates predicate `P` over tables `T(P)`; `Vset(P)` names, for every object of `T(P)`, the version selected (unborn and dead versions included) |
| `w_i(x_i)` | write (insert, update or delete) of `x` |
| `c_i` | commit; assigns the commit sequence number `c_i` (§4.1) |
| `a_i` | abort |

An item read is the predicate read `pk = k` restricted to one object; a checker MAY treat it either way. Every
query is decomposed into item and predicate reads per base table (§4.4); a statement's reads all share one
`Vset` snapshot.

A version `x_k` **matches** `P` if `x_k` is visible and satisfies `P`. A version `x_k` **changes the matches** of
`P` relative to `x_j` if exactly one of `x_j`, `x_k` matches `P`.

### 2.3 Dependencies and the direct serialization graph

For committed transactions `Ti ≠ Tj`, following [Adya]:

- **ww (write-depends):** `Ti` installs `x_i` and `Tj` installs the next version `x_j` of `x` in `<<_x`.
- **wr (read-depends):** item: `Tj` reads `x_i`. Predicate: `Tj` performs `r_j(P: Vset(P))`, `x_i ∈ Vset(P)`,
  and `x_i` changes the matches of `P` relative to the version preceding it in `<<_x`.
- **rw (anti-depends):** item: `Ti` reads `x_k` and `Tj` installs the version of `x` that immediately follows
  `x_k`. Predicate: `Ti` performs `r_i(P: Vset(P))`, `x_k ∈ Vset(P)`, and `Tj` installs a later version `x_j`
  (`x_k <<_x x_j`) that changes the matches of `P` relative to `x_k`.
- **rt (real-time):** `Ti`'s commit was acknowledged to its client before `Tj`'s begin was invoked (§2.5). Not an
  Adya edge; used only for the strict and strong variants.
- **so (session):** `Ti` and `Tj` ran on the same session and `Ti` completed before `Tj` began.

The **DSG** of a history is the graph whose nodes are its committed transactions and whose edges are its ww, wr
and rw edges. `DSG+rt` adds the rt edges; `DSG+so` adds the so edges.

### 2.4 Phenomena

| Id | Phenomenon | Definition |
|---|---|---|
| G0 | write cycle | a DSG cycle of ww edges only |
| G1a | aborted read | a committed `Tj` reads a version written by an aborted `Ti` |
| G1b | intermediate read | a committed `Tj` reads an intermediate version `x_i.n` of another transaction |
| G1c | circular information flow | a DSG cycle of ww and wr edges only |
| G-single | single anti-dependency cycle | a DSG cycle with exactly one rw edge |
| G-nonadjacent | nonadjacent anti-dependency cycle | a DSG cycle with at least one rw edge in which no two rw edges are adjacent [Cerone] |
| G2-item | item anti-dependency cycle | a DSG cycle with at least one item rw edge |
| G2 | anti-dependency cycle | a DSG cycle with at least one rw edge, item or predicate (phantoms included) |
| P4 | lost update | `Ti` reads `x_k`, `Tj` installs `x_j` after `x_k`, then `Ti` installs `x_i` after `x_j`; both commit. A special case of G-single |
| A5B | write skew | a G2-item cycle of exactly two rw edges between two transactions |
| G-rt | real-time violation | a cycle in `DSG+rt` that is not a cycle in the DSG |
| G-stale | stale snapshot | `Tj` began after `Ti`'s commit was acknowledged, `Ti` wrote `x`, and `Tj` reads a version of `x` earlier than `x_i` |
| G-session | session violation | a cycle in `DSG+so` that is not a cycle in the DSG |

### 2.5 The recorded history

A test harness records one record per operation, in the [Elle] style. Each record is one JSON object per line:

```
{"index": 17, "time": 1093822100, "process": 3, "node": "primary",
 "type": "invoke" | "ok" | "fail" | "info",
 "level": "serializable" | "snapshot" | "read_committed", "sync": "on" | "off",
 "txid": 4711, "snapshot": 9000, "commit": 9012,
 "error": "serialization_failure",
 "value": [["append", "k3", 41], ["r", "k1", [1, 5, 41]], ["pr", "t.g = 2", ["k1", "k7"]], ["w", "k2", 9]]}
```

- `time` is the harness's monotonic clock in nanoseconds (in simulation, simulated time). One process is one
  session and issues one transaction at a time.
- `invoke` is recorded before the begin is sent; `ok` after the commit acknowledgement arrives; `fail` when the
  server reports that the transaction aborted (§5); `info` when the outcome is unknown (connection lost, timeout,
  `commit_unknown`). An `info` transaction may or may not have committed and the checker MUST consider both.
- `value` lists the transaction's operations in program order with observed results: `["r", k, v]` item read,
  `["w", k, v]` write, `["append", k, e]` append of element `e` to the list in `k`, `["pr", P, rows]` predicate
  read with the keys it returned. A key may name a nested relation inside a row as `"k3.items"` (§7.2, W5).
- `txid`, `snapshot` and `commit` are the white-box fields TernDB returns: the transaction id, the snapshot
  sequence number `s_i`, and the commit sequence number `c_i` (§4.1). `snapshot` is returned by begin, `commit` by
  the commit acknowledgement.

**Mapping.** Committed transactions are the `ok` records plus whichever `info` records the checker resolves as
committed (by the transaction-status query of §5.4, or by finding their writes). The rt edge `Ti → Tj` holds when
`Ti`'s `ok` time is less than `Tj`'s `invoke` time. Black-box checks infer `<<_x` from values: list-append keys
give it directly; register keys use unique written values. White-box checks take `<<_x` and the serial order
from `commit`. **Every black-box check MUST also run without the white-box fields**, so that a bug in the
sequence numbers cannot hide a bug in the data (§7.1).

## 3. The levels

### 3.1 Serializable (the default)

On the primary, a serializable transaction MUST satisfy **strict serializability**: in every history, `DSG+rt`
restricted to transactions that committed at serializable has no cycle; in particular G0, G1a, G1b, G1c,
G-single, G2-item, G2 and G-rt MUST NOT occur among them. The serial order MUST be the commit order `c`.

**Why strict.** Precision-locking validation already yields commit-order serializability (§4.6), and the commit
point lies inside every transaction's real-time interval; read-only transactions are serialized at their snapshot
point, which also lies inside it. Strictness is therefore free on a single primary: it costs no extra wait and
no extra abort, and it gives clients the guarantee they assume anyway (a transaction that began after another
was acknowledged sees it). Giving it up would buy nothing that a replica does not already offer (§3.6).

### 3.2 Snapshot

A snapshot transaction MUST satisfy **strong snapshot isolation**: G0, G1a, G1b, G1c, G-single, G-nonadjacent,
P4 and G-stale MUST NOT occur. G2-item (write skew, A5B), G2 with predicates (phantoms) and the read-only
anomaly of [Fekete] MAY occur. All reads of the transaction come from the one snapshot `s_i`.

### 3.3 Read committed

A read-committed transaction MUST satisfy PL-2: G0, G1a, G1b and G1c MUST NOT occur. In addition each
*statement* MUST read from one snapshot taken when the statement starts, and that snapshot MUST include every
transaction acknowledged before the statement started (no G-stale at statement granularity). Within one
statement, therefore, read skew (A5A) cannot occur. Across statements G-single, P4, A5B and G2 MAY occur. An
`UPDATE` or `DELETE` statement MUST NOT lose an update made by a concurrent committed transaction to a row that
the statement modifies (§4.2): a single-statement `UPDATE t SET n = n + 1 WHERE …` is never a lost update.

### 3.4 Mixed levels

Transactions at different levels may run together. Each transaction's guarantee is judged on the sub-history of
the transactions at its level or stronger: a serializable transaction is in no `DSG+rt` cycle all of whose nodes
are serializable; a snapshot transaction is in no forbidden SI cycle all of whose nodes are snapshot or
serializable. A weaker transaction MAY make a stronger one observe an anomaly that involves the weaker one.

### 3.5 Session guarantees on the primary

Because every snapshot on the primary includes every acknowledged commit, the following hold at every level,
across sessions as well as within one, and MUST be checked (no G-session, no G-stale):

- **read-your-writes:** a transaction sees every write of every transaction its session committed earlier;
- **monotonic reads:** a later transaction never sees an earlier version of an object than an earlier one did;
- **monotonic writes** and **writes-follow-reads:** follow from version order equal to commit order.

### 3.6 Replica reads

A replica accepts read-only transactions only; a write is refused with `read_only_transaction`. A replica applies
the primary's WAL in order and MUST satisfy:

- **R1 consistent prefix.** Every replica transaction (and, at read committed, every statement) reads the state
  after a prefix of the primary's commit order: all of `T1 … Tn`'s writes and none of `Tn+1`'s, for some `n`. Its
  `snapshot` field reports `c_n`. Consequently the combined history of the primary and all replicas is
  serializable: insert each replica transaction into the commit order right after `Tn`. It is **not** strict: a
  replica read may miss a commit acknowledged before it began.
- **R2 monotonic per connection.** Within one connection to one replica, successive snapshots are
  non-decreasing.
- **R3 causal token.** Every commit acknowledgement returns `commit`; every begin returns `snapshot`. A begin MAY
  carry `after = n`; the replica MUST then wait until it has applied `c ≥ n` before taking the snapshot, or fail
  with `replica_lag_timeout` after `replica_wait_timeout`. A client that passes its session's highest observed
  `commit`/`snapshot` gets read-your-writes and monotonic reads across the primary and every replica.
- **R4 no unflushed data.** A replica MUST NOT apply WAL that is not durable on the primary. A replica therefore
  never shows a transaction that a primary crash could lose.

Replica transactions are serializable whatever level they request, because a read-only transaction on a prefix
of the commit order is serializable (§5.5). A replica transaction never aborts with `serialization_failure`.

## 4. The mechanism and why it gives the guarantee

### 4.1 Sequence numbers and snapshots

- **Transaction ids** are 64-bit, assigned at begin, and MUST NOT be reused or wrap. An uncommitted version
  carries its writer's id in the row header; there is no wraparound maintenance.
- **Commit sequence numbers** `c` are 64-bit, drawn from one counter, and MUST be assigned in the order of the
  transactions' WAL commit records. `c` is the serialization order of §3.
- **Publication.** A commit is *published* when its versions become visible to new snapshots. Commits MUST be
  published in `c` order: the **visible horizon** `h` is the largest `c` such that every commit `≤ c` is
  published.
- **Snapshot.** A snapshot is a value `s = h` read at a moment; it sees exactly the commits with `c ≤ s`, plus the
  transaction's own writes. Snapshot and serializable transactions take `s_i` at begin; read committed takes one
  per statement; a replica's `h` is the last commit it has applied.
- **Acknowledgement.** A commit MUST be published before it is acknowledged. Hence any transaction (or statement)
  that begins after an acknowledgement has a snapshot that includes it: this is what excludes G-stale and makes
  rt edges point forward in `c` order.
- **Reading a version.** A reader reads the in-place newest version and follows the undo chain back to the first
  version whose commit is `≤ s` (or its own). Readers MUST take no lock and no latch held across the read; a row
  read concurrently with a writer MUST return one whole version (validated optimistically and retried), never a
  mixture of two.

### 4.2 Write-write conflicts: first-updater-wins

The row header holds the write lock: the id of the uncommitted transaction that owns the in-place version, if
any. When `Ti` writes object `x`:

1. If `x` is locked by another live transaction `Tj`, `Ti` MUST wait until `Tj` commits or aborts (subject to
   deadlock detection and `lock_timeout`, §5).
2. Let `x_k` be the newest committed version. At snapshot and serializable, if `c_k > s_i` (it is not in `Ti`'s
   snapshot), `Ti` MUST abort with `serialization_failure`. Otherwise `Ti` locks `x`, pushes `x_k` onto the undo
   chain and installs its version in place.
3. At read committed, `Ti` instead re-reads `x_k`, re-evaluates the statement's predicate on it, and if it still
   matches applies the statement's change to `x_k` (not to the version its snapshot saw); if it no longer
   matches the row is skipped.
4. An insert of key `k` that finds a visible committed version of `k` in `Ti`'s snapshot fails with
   `unique_violation`; one that finds `k` only through rule 2 (committed after `s_i`) fails with
   `serialization_failure`, because the conflict is a race, not a duplicate.

**Why first-updater-wins rather than first-committer-wins.** Undo-based MVCC keeps exactly one in-place newest
version per row, so two uncommitted writers of one row cannot both have installed their version; first-committer-
wins would need private write buffers and a second copy at commit. First-updater-wins also aborts the loser as
early as possible, before it does more work.

**Consequence (W1).** Every write of `x` by `Ti` is installed directly over the newest committed version, that
version is in `Ti`'s snapshot (`c_k ≤ s_i`), and no other writer can commit a version of `x` between `Ti`'s write
and `Ti`'s commit. So `<<_x` is the commit order of the writers, and for every ww edge `Tk → Ti`: `c_k ≤ s_i`.

### 4.3 Serializable: the predicate log and validation

A serializable transaction `Ti` keeps a **predicate log** `L_i`: one entry per base-table access, each a table
and a predicate `L` over that table's rows (§4.4). At commit, if `Ti` has written anything, it MUST validate:

```
validate(Ti):
    assign c_i                                   -- the next commit sequence number
    wait until every commit with c < c_i is published or aborted
    for each committed Tj with s_i < c_j < c_i:   -- committed during Ti's lifetime
        for each write w of Tj to a table of L_i:
            for each entry (table, L) of L_i on that table:
                if before(w) matches L or after(w) matches L:
                    abort Ti with serialization_failure
    publish Ti
```

`before(w)` is the version `w` replaced (none for an insert), `after(w)` the version it installed (none for a
delete); a dead or absent image matches nothing. Validation and publication of `Ti` MUST be equivalent to running
atomically with respect to other committing transactions; an implementation MAY validate in parallel and MAY
validate early against commits already published, provided the outcome equals the procedure above.

- **Retention.** The write images of every committed `Tj` MUST be retained until no active serializable
  transaction has `s < c_j`. It MAY retain only the columns some logged predicate reads.
- **Precision.** Only writes whose images match a logged predicate abort `Ti`; a concurrent write to a disjoint
  part of a table, or one that touches neither image's match, never aborts it. This is the difference from
  [Cahill]'s SSI, which aborts on a dangerous structure of two rw edges whether or not a cycle closes, and
  tracks reads at page or relation granularity under memory pressure.
- **A transaction with an empty write set** skips validation and is serialized at `s_i` (§5.5).
- **Savepoints.** Rolling back to a savepoint undoes the writes after it, but the reads after it stay in `L_i`:
  the client has already observed them.

### 4.4 What is logged

For each read, the logged predicate `L` MUST be **sound**: every row whose version could change the read's result
satisfies `L`. Formally, if `P` is the membership predicate of the read (the condition that decides whether a row
contributes to the result), then `P ⇒ L`. `L` MAY be wider than `P`; it MUST NOT be narrower.

| Read | Logged predicate |
|---|---|
| point lookup by key `k`, found or not | `key = k` |
| index range scan over `[a, b)` with residual filter `F` | `key ∈ [a, b) ∧ F`, or `key ∈ [a, b)` |
| full scan with filter `F` | `F` (`true` if none) |
| scan that stops early (`LIMIT`, `EXISTS`, a min/max probe) | `F` restricted to the key range actually traversed, or `F` |
| aggregate over a scan | the scan's predicate |
| join, semi-join or anti-join | each input's own scan predicate; an index-nested-loop probe logs each probe key, or the probed side's restriction |
| catalog lookup of table `t` (every access to `t`) | the catalog entry of `t` (§5.6) |

When `L_i` exceeds `max_predicate_log` entries, the implementation MAY coarsen entries (merge ranges, drop
residual filters, widen to `true` on a table). Coarsening MUST only widen.

### 4.5 Nested relations

A column may hold a relation. The rules:

- **N1 one version.** A nested relation is part of its row's value. A write to any element of it, at any depth,
  is a write of the top-level row: it takes the row's lock, creates one new row version, and is one `w_i(x_i)`.
- **N2 one read.** Reading a top-level row reads one version of it, whole: every nested level returned for that
  row comes from that one version. A nested result never combines elements from two versions of a row, and every
  nested level of every row in one statement comes from the statement's one snapshot.
- **N3 predicates over nested levels** are predicates over the top-level row. The logged `L` is the membership
  predicate of the top-level rows, which may refer to nested levels (`WHERE EXISTS (SELECT … FROM r.items WHERE
  price > 10)`, `ANY`, an aggregate over a nested relation). Validation evaluates `L`, nested parts included, on
  the whole before- and after-images of each written top-level row.
- **N4 shaping filters are not logged.** A filter that only selects which nested elements are returned for a row
  already in the result (`SELECT id, (SELECT … FROM r.items WHERE price > 10) FROM r WHERE region = 'EU'`) does
  not change membership, and need not be logged: any write to a member row has a before-image matching the
  membership predicate (`region = 'EU'`) and is caught by it.
- **N5 nested results built from other tables** (a nest over a join, a relation-valued subquery over another
  table) are one statement and one snapshot; each base table contributes its own entries under §4.4.

Soundness of N3–N4: a write `w` to row `x` can change the read's result only if `x` is a member before or after
`w`, or its nested content changes while it is a member; in each case `before(w)` or `after(w)` satisfies the
membership predicate. The cost of N1 is granularity: two transactions that change different elements of the same
row's nested relation conflict under §4.2 (see Open questions).

### 4.6 Proof sketches

Write `Ti → Tj` for a DSG edge, and let all transactions be committed.

**Lemma A (wr and ww point back to the snapshot).** If `Tk →ww Ti` or `Tk →wr Ti` then `c_k ≤ s_i` (for read
committed: `≤` the snapshot of the reading statement). For ww this is W1. For wr, `Ti` read `x_k` from its
snapshot, which contains only commits `≤ s_i`; `Ti` read no uncommitted version of another transaction (§4.1).

**Lemma B (rw points past the snapshot).** If `Ti →rw Tj` then `c_j > s_i`. `Ti` read `x_k` (item) or `Vset(P)`
containing `x_k` (predicate); `Tj` installs a version after `x_k`. Had `c_j ≤ s_i`, that version or a later one
would be in `Ti`'s snapshot, contradicting `Ti` having read `x_k`.

**PL-2 at every level.** G1a and G1b: snapshots contain only published (committed) versions, and only final ones,
since an intermediate version is never published. G0 and G1c: by Lemma A and `s_i < c_i`, every ww and wr edge
`Tk → Ti` has `c_k < c_i`, so a cycle of them would need `c` to decrease around it.

**Snapshot forbids G-single and P4.** Suppose a cycle `Ti →rw Tj → … → Ti` whose other edges are ww or wr. By
Lemma A along the path from `Tj` back to `Ti`, `c_j ≤ s_… < c_… ≤ … ≤ s_i`, so `c_j ≤ s_i`; Lemma B gives
`c_j > s_i`. P4 is such a cycle. G-nonadjacent: with snapshots equal to start points and versions ordered by
commit, the history is in the SI class of [Adya]/[Cerone], which forbids cycles without adjacent rw edges. G-stale:
§4.1 acknowledgement rule.

**Serializable forbids G2 (with predicates).** Claim: every DSG edge `Ti → Tj` among serializable transactions
with writes has `c_i < c_j`. ww and wr: shown above. rw: let `Ti →rw Tj`. By Lemma B, `c_j > s_i`. Suppose
`c_j < c_i`. Then `Tj` committed in `(s_i, c_i)`. *Item read* of `x_k`: `Tj`'s write `w` has `before(w) = x_k`.
If `x_k` is visible it matches `key = k`; if it is dead or unborn, `w` is an insert and `after(w)` matches. *Predicate
read*: `x_j` changes the matches of `P` relative to `x_k`. Let `x_m` be the first version after `x_k` in `<<_x`
whose match differs from `x_k`'s; its writer `Tm` has `s_i < c_m ≤ c_j < c_i`, and exactly one of its images
`x_{m-1}`, `x_m` matches `P`, hence (soundness, `P ⇒ L`) matches `L`. In both cases `validate(Ti)` examines that
write and aborts `Ti`, contradicting its commit. So `c_j > c_i`. With every edge increasing in `c`, the DSG is
acyclic and the commit order is a serial order.

**Read-only transactions.** A transaction `R` with an empty write set has no incoming rw edge (it installs
nothing) and its outgoing rw edges go to `Tj` with `c_j > s_R` (Lemma B); its incoming wr edges come from
`c_k ≤ s_R`. Placing `R` in the serial order immediately after the commit `s_R` keeps every edge forward. This
holds whatever the level of the other transactions, as long as they are serializable among themselves, and it
holds for a replica's prefix snapshot equally.

**Strictness.** If `Ti`'s acknowledgement precedes `Tj`'s begin, `Ti` was published first (§4.1), so `c_i ≤ s_j
< c_j`; and if `Tj` is read-only it is placed at `s_j ≥ c_i`. So rt edges also increase in the serial position,
and `DSG+rt` is acyclic.

The mechanised version of these arguments is the model of §7.4.

## 5. Aborts, errors and retry

### 5.1 Rules

- Any error inside a transaction aborts the whole transaction unless it occurred inside a savepoint that the
  client rolls back to. A statement never commits partially.
- An aborted transaction's writes MUST never have been visible to any other transaction (G1a), and MUST NOT
  survive a crash (I22, I24).
- An abort MUST be reported to the client with exactly one code from §5.2.

### 5.2 Error codes

| Code | Raised when | Levels | Retryable |
|---|---|---|---|
| `serialization_failure` | first-updater-wins conflict (§4.2 rule 2 or 4); validation failure (§4.3) | SI, SER | yes |
| `deadlock_detected` | the wait-for graph of row locks has a cycle; the victim aborts | all | yes |
| `lock_timeout` | a row-lock wait exceeded `lock_timeout` | all | yes |
| `schema_changed` | a DDL on a table this transaction used committed during its lifetime (§5.6) | all | yes |
| `replica_lag_timeout` | `after` not reached within `replica_wait_timeout` | replica | yes |
| `replica_conflict` | WAL replay needed to remove a structure this replica transaction uses (§5.6) | replica | yes |
| `cancelled` | the client, an administrator or a shutdown cancelled it before commit | all | yes |
| `resource_exhausted` | memory, undo space or disk full; the transaction is aborted | all | yes, after back-off |
| `commit_unknown` | the server cannot say whether the commit happened (e.g. it lost the WAL device mid-commit) | all | **no** — resolve first (§5.4) |
| `unique_violation`, `check_violation`, `foreign_key_violation`, `not_null_violation` | a constraint fails on data visible to the transaction | all | no |
| `read_only_transaction` | a write in a read-only transaction or on a replica | all | no |
| `data_corrupted` | a checksum or structural check failed; the server stops serving the object | all | no |

A retryable code MUST guarantee that the transaction is aborted and had no effect. `serialization_failure` MUST
NOT be raised for a transaction with an empty write set at serializable or snapshot (§5.5), and MUST NOT be raised
at read committed.

### 5.3 When retry is safe

**A transaction that received a retryable code MAY be re-run from its beginning, and the resulting history is
the history in which the failed attempt never ran** (property I19). This covers the database only; a client
that performed external side effects during the attempt owns them. Retry from the failed statement is not safe:
the whole transaction re-runs with a new snapshot. Retrying cannot succeed by repeating the same reads of the
same snapshot, because the new attempt's snapshot includes the conflicting commit.

### 5.4 Unknown outcomes

A connection lost after the commit was sent, or `commit_unknown`, leaves the outcome unknown to the client. The
server MUST answer `transaction_status(txid)` with `committed(c)`, `aborted` or `in_progress` for every `txid`
younger than `status_horizon` (at least the last 24 hours of ids); after a crash, every transaction that was not
recovered as committed MUST report `aborted`. A client MUST resolve an unknown outcome this way before retrying
a non-idempotent transaction. The harness uses the same query to resolve `info` records.

### 5.5 Read-only transactions

- A transaction declared `READ ONLY`, or one that commits with an empty write set, MUST NOT abort with
  `serialization_failure` at any level, keeps no predicate log when declared, and is serialized at its snapshot
  (§4.6). TernDB therefore has no `DEFERRABLE` mode and no safe-snapshot wait: unlike SI-based SSI [Cahill],
  where the read-only anomaly of [Fekete] forces a read-only transaction to wait for a safe snapshot, here every
  snapshot is safe, because the serial order is the commit order and a snapshot is a prefix of it.
- A long-running read-only transaction holds back undo garbage collection. TernDB MUST NOT abort a reader to
  reclaim undo space unless `max_snapshot_age` is set, in which case it aborts with `resource_exhausted`.

### 5.6 DDL

- The catalog is versioned under MVCC like data. Every statement reads the catalog at its snapshot, and every
  access to table `t` logs a point read of `t`'s catalog entry (§4.4).
- A DDL statement on `t` writes `t`'s catalog entry and MUST take its write lock, waiting for every live
  transaction that has written `t` (DML holds a shared lock on the catalog entry of every table it writes until
  it ends). DDL never waits for readers.
- A transaction at any level that accessed `t` under a catalog version replaced by a DDL committed during its
  lifetime MUST abort with `schema_changed` at commit (at read committed: at its next statement on `t` if that
  statement would plan against the dropped or changed definition, else at commit). This is validation of the
  catalog read, applied at every level.
- A DDL statement that checks existing data (adding a constraint, building a unique index) MUST validate its
  scan as a serializable read whatever the transaction's level, so that no concurrent commit can slip a row past
  the check.
- On a replica, replay of a DDL that drops or rewrites a structure a replica transaction is using MUST wait up to
  `max_replay_delay` and then abort that transaction with `replica_conflict`. DML replay never conflicts with
  replica readers: a replica keeps its own undo chains for its own snapshots.

## 6. Commit and replication

- Commit appends the transaction's commit record to the WAL; group commit MAY flush many commit records with one
  fsync. Commit records are in `c` order (§4.1).
- **`synchronous = on`** (the default): the commit MUST NOT be acknowledged until the WAL is durable through its
  commit record. A read-only transaction at `on` MUST NOT be acknowledged until the WAL is durable through its
  snapshot `s`. Hence an acknowledged `on` transaction never depends on data a crash can lose.
- **`synchronous = off`** (per transaction): the commit MAY be acknowledged once published, before the fsync.
  Publication before durability is allowed at both settings: an unflushed commit may be visible to other
  transactions, but none of those can be acknowledged at `on` before it is durable, because their commit records
  (or snapshots) follow it in the WAL.
- An fsync error MUST be treated as a crash of the WAL: the server stops accepting commits and recovers from the
  last durable state. It MUST NOT retry the fsync and continue.
- Replication streams WAL that is durable on the primary (R4), in WAL order.

## 7. Checkable properties

This section is the contract that the model, the simulator and the Jepsen-style harness test against. Each
property names its workload (§7.2) and expected outcome per level: **F** = the checker MUST find no instance;
**P** = permitted (the checker reports it, the run is green); **—** = not applicable.

### 7.1 Properties

| # | Property | Workload | SER | SI | RC |
|---|---|---|---|---|---|
| I1 | no G0 | W1, W2 | F | F | F |
| I2 | no G1a (aborted read) | W1, W2 | F | F | F |
| I3 | no G1b (intermediate read) | W1 (multi-append to one key per txn) | F | F | F |
| I4 | no G1c | W1 | F | F | F |
| I5 | version order equals commit order: for each key, the list order of appends equals `commit` order | W1 (white box) | F | F | F |
| I6 | no G-single; no P4 (lost update) | W1, W3 | F | F | P |
| I7 | no G-nonadjacent | W1 | F | F | P |
| I8 | no G2-item; no A5B (write skew) | W1, W3b | F | P | P |
| I9 | no G2 with predicates (phantom) | W4 | F | P | P |
| I10 | no G-rt: `DSG+rt` acyclic | W1, W4 | F | — | — |
| I11 | no G-stale on the primary (per transaction; per statement at RC) | W1, W2 | F | F | F |
| I12 | no G-session on the primary | W1 | F | F | F |
| I13 | statement snapshot: within one statement, reads come from one prefix (bank totals exact in one `SELECT sum`) | W3 | F | F | F |
| I14 | single-statement increment never loses an update | W2 (`SET v = v + 1`) | F | F | F |
| I15 | read-only anomaly [Fekete] does not occur | W6 | F | P | P |
| I16 | no `serialization_failure` for read-only transactions; none at RC | W1, W6 | F | F | F |
| I17 | nested: N1 and N2 — each row's nested list is a list-append object obeying I1–I8 as the row does; no read mixes two versions of one row | W5 | as I1–I8 | as I1–I8 | as I1–I4 |
| I18 | nested predicates: no G2 through a predicate over a nested level | W5 | F | P | P |
| I19 | retry: a run in which each retryable failure is retried is checked as if the failed attempts never ran; no `fail` record's writes are ever read | W1, W3 | F | F | F |
| I20 | replica: R1 every replica read is a prefix; the combined primary+replica history is serializable | W7 | F | F | F |
| I21 | replica: R2 monotonic per connection; R3 with `after`, read-your-writes and monotonic reads across nodes | W7 | F | F | F |
| I22 | atomicity: after recovery each transaction's writes are all present or all absent | W8 | F | F | F |
| I23 | durability: every transaction acknowledged at `sync = on` is present after recovery | W8 | F | F | F |
| I24 | prefix: the recovered state is the state after a prefix of `commit` order; the lost set is a suffix, and contains only transactions unacknowledged or acknowledged at `off` | W8 | F | F | F |
| I25 | status: `transaction_status` after recovery agrees with the recovered state for every txid | W8 | F | F | F |
| I26 | DDL: no row violating a constraint added concurrently survives; no transaction commits a write planned against a dropped definition | W9 | F | F | F |
| I27 | error codes: every abort carries one §5.2 code; every retryable-coded transaction left no trace (I2, I19); every `unique_violation` names a key visible in the snapshot | all | F | F | F |
| I28 | torn writes and corrupt pages are detected (`data_corrupted` or recovery from the WAL), never returned as data | W8 + fault injection | F | F | F |

**Black box first.** I1–I4, I6–I12, I17–I18 MUST be checked from values alone (Elle inference), and separately
with the white-box `snapshot` and `commit` fields as the claimed version and serial orders. The white-box check
MUST verify that every read returns exactly the version that was newest at its snapshot, and that the
commit order is a valid serial order (for SER) — a disagreement between the two checks is itself a failure.

**Prove the checker can fail.** Each anomaly check MUST have a fault-injected build in the simulator that is
expected to fail it: validation disabled (I8, I9, I15 red at SER), first-updater-wins disabled (I6 red at SI),
snapshot taken at statement start for SI (I13 still green, I6 red), publication out of order (I10, I11 red),
acknowledgement before publication (I11 red), and fsync result ignored (I23 red). A check that a broken build
passes is not evidence.

### 7.2 Workloads

- **W1 list-append** [Elle]. Keys hold lists (a row `(k, list)`; an append is an `UPDATE` appending a unique
  element). Transactions of 1–8 random `append` and `r` ops over a small key space (Zipfian), at each level, with
  multiple appends to the same key in some transactions (G1b). Elle recovers `<<_x` from list prefixes.
- **W2 rw-register.** Keys hold integers; writes are unique values; some transactions are single-statement
  increments (`SET v = v + 1`) whose final value is checked against the count of `ok` increments (I14).
- **W3 bank.** N accounts with a fixed total. Transfers read two balances, refuse an overdraft, and write both.
  Readers read all balances in one statement (I13) and, in a second transaction shape, one statement per account
  (total exact at SER/SI; may be wrong at RC). **W3b write skew:** pairs of accounts with the constraint
  `a + b ≥ 0`; a withdrawal reads both and debits one. A negative pair is F at SER, P at SI and RC.
- **W4 predicate/phantom.** Table `t(k, g, v)`. A transaction runs `SELECT k FROM t WHERE g = $g` (and
  `SELECT count(*) …`), then inserts a fresh unique `k` into group `g` or `g'`, or deletes one. The checker
  derives predicate edges: `U →wr T` if `T`'s result contains a row `U` inserted (or lacks one `U` deleted);
  `T →rw U` if `U` committed an insert into `g` that `T`'s result lacks, or a delete of a row `T`'s result
  contains. Including the "on-call" shape: each transaction removes itself from group `g` only if `count ≥ 2`;
  an empty group is F at SER.
- **W5 nested.** Rows `(k, tag, items RELATION(e, price))`. Ops: append an element to `k.items`; read `k` with
  its whole `items`; read rows `WHERE EXISTS (SELECT 1 FROM items WHERE price > $p)`; read with a shaping inner
  filter. Each `k.items` is a list-append object (I17); the `EXISTS` reads are W4 predicates over the nested
  level (I18).
- **W6 read-only anomaly** [Fekete]: checking `x`, savings `y`; `T2` deposits into `y`; `T1` withdraws from `x`
  with an overdraft penalty judged on `x + y`; read-only `T3` reads both. Run concurrently at random
  interleavings; the [Fekete] cycle is F at SER, and `T3` never aborts.
- **W7 replica.** W1 and W3 writers on the primary; readers on 1–3 replicas, with and without `after`, with
  replicas paused, restarted and lagged by the simulator.
- **W8 crash.** W1 and W3 with a mix of `sync = on` and `off`, and crashes, torn writes, fsync errors and read
  errors injected (§7.3). After each recovery the harness reads every key and checks I22–I25 and I28. For I24 a
  list-append key makes the check direct: after recovery the recovered lists MUST be explained by the appends of
  exactly the transactions with `commit ≤ n` for some `n`.
- **W9 DDL.** W2 traffic while other sessions add and drop columns, add a `CHECK`/`UNIQUE` constraint, and build
  indexes.

### 7.3 Deterministic simulation

Every source of non-determinism — the clock, the scheduler, network and disk I/O, randomness — MUST go through
one seam that the simulator replaces, so that a seed reproduces a run exactly (the FoundationDB and TigerBeetle
approach). The simulator MUST be able to inject, at any I/O: a crash of the process; a torn write (a page written
partly, at sector granularity, in any order of sectors); a lost write that the device acknowledged without
`fsync`; an fsync failure; a read error; a flipped bit in a page at rest; and arbitrary delay and reordering of
replication messages. Every CI run executes the W1–W9 checks over a fixed seed set plus fresh random seeds, and a
failing seed is kept as a regression seed.

### 7.4 The model

An executable model of §4 MUST be model-checked for the properties of §3: snapshots and the visible horizon,
first-updater-wins with waits, the predicate log and `validate`, read committed's re-evaluation, `synchronous`
and crash recovery to a WAL prefix, and a replica applying a durable prefix. The checked property is that every
reachable history has an acyclic `DSG+rt` among serializable transactions, no forbidden SI cycle among snapshot
and serializable ones, PL-2 for all, and I22–I24 across crashes. It MUST be checked at least for 3 transactions, 2
objects and 2 predicates (one over a nested level), and MUST fail when validation, first-updater-wins or ordered
publication is removed.

## 8. Crash semantics

A crash of the primary, under any mix of `synchronous` settings, MUST leave a database whose recovered state
satisfies:

- **Atomicity (I22).** For every transaction, either all of its writes are present or none are.
- **Prefix (I24).** There is an `n` such that the recovered state is exactly the state after committing
  `T1 … Tn` in `c` order, and no transaction with `c > n` is present. This is why commit records are in `c` order
  and why a crash may lose only a WAL suffix.
- **Durability (I23).** `n ≥ c` of every transaction acknowledged at `synchronous = on`, and `n ≥ s` of every
  read-only transaction acknowledged at `on`.
- **What `off` may lose.** A transaction acknowledged at `off` MAY be lost, together with every transaction after
  it in `c` order (including ones that read it, none of which was acknowledged at `on`). It MUST NOT be lost
  while a later commit survives.
- **Consistency.** The recovered state satisfies every constraint, and every index agrees with its table.
- **Status (I25).** A lost transaction reports `aborted`; a recovered one reports `committed(c)` with its original
  `c`.
- **Corruption (I28).** A torn or corrupt page MUST be detected by checksum and rebuilt from the WAL or reported
  as `data_corrupted`; it MUST NOT be read as data.

A replica crash loses nothing the primary has; on restart the replica resumes applying from its last durable
position, and its first snapshot after restart MAY be older than one it served before the crash (R2 is per
connection). Failover to a replica (promotion) yields the replica's applied prefix: commits acknowledged on the
primary but not yet applied on the promoted replica are lost, at every `synchronous` setting (see Open
questions).

## Open questions

1. **Wait or abort on a locked row.** §4.2 waits for the lock holder and then aborts if it committed (as
   Postgres does). HyPer aborts at once. Waiting saves the abort when the holder rolls back, at the cost of
   blocked writers and a deadlock detector. Which should be the default, and should it be a setting?
2. **Nested-element granularity.** N1 makes two writers of different elements of one row's nested relation
   conflict. Should large nested relations get element-level versioning and locking, at the cost of a second
   versioning scheme and a weaker meaning of "one read"?
3. **Projection-aware validation.** Validation aborts on any matching image, even when the concurrent write
   changed only columns the reader never looked at. Restricting the test to read columns would abort less, but
   breaks the "all edges increase in `c`" argument of §4.6 and needs its own proof. Pursue it?
4. **Synchronous replication.** Should there be `synchronous = remote` (acknowledge after a replica has the WAL
   durable), so that acknowledged commits survive failover? Without it, failover can lose acknowledged `on`
   commits.
5. **Fencing on failover.** How is an old primary prevented from acknowledging commits after a replica is
   promoted: an operator duty, a lease, or an epoch in the WAL?
6. **Bounded staleness on replicas.** Should a replica begin accept `max_staleness = t` in addition to
   `after = n`?
7. **`max_snapshot_age` default.** Unlimited (as specified) protects readers; a limit protects undo space. Which
   default, and is `resource_exhausted` the right code for it?
8. **The model's language.** Is the §7.4 model written in TLA+ (checked with TLC or Apalache), or in sysl and
   explored by the simulator's own scheduler? TLA+ is not database code, but the project rule is sysl only.
9. **Online DDL.** §5.6 makes DDL wait for writers of the table. Rewrite-free DDL could avoid that; table-rewriting
   DDL needs a design of its own.
10. **Synonyms.** Should `REPEATABLE READ` and `READ UNCOMMITTED` be accepted as synonyms, or refused, since
   TernDB's dialect is its own?
