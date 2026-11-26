# The vectorised executor — design note

This note records how TernDB's executor is built. It is **design, not specification**: what a node
or an expression *means* is fixed by `docs/spec/query-tree.md`, and this note only says how the
executor computes it. Where it states a rule of meaning it cites the section it relies on; where
`query-tree.md` was silent about something the executor has to decide at run time, the rule was
written into `query-tree.md` itself, and this note points at it.

The code is `sh/sysl/terndb/exec_*.sysl`. It covers column vectors, expression evaluation, and the
operators `Values`, `ParamScan`, `Filter`, `Project`, `Sort`, `Limit`, `Distinct`, `Aggregate`,
`Tag`, `Join` (§4.1), and `Nest`, `Unnest`, `Apply` with the subquery expressions (§4.2), `Window`
(§4.3), `SetOp`, `With` and `CteRef` with recursive CTEs (§4.4), `Scan` over the in-memory MVCC
store and the DML statements `Insert` (with upsert), `Update` and `Delete` run in a store
transaction (§4.5), a source over in-memory batches standing in for `Scan` where no store is given,
and the conversion of an operator's output into result batches, views, sequences and identity
columns, and the zone conversions. Disk storage and the unnesting planner come later.

## 1. Column vectors

A `ColumnVector` holds `len` slots of one query-tree type `ty` (nullability included). Its layout
is **the result format's column tree** (`wire-protocol.md` §6.2–§6.3, `columns.sysl`), node for
node, with each buffer held as a typed `Buf` instead of raw octets:

| type | `Lanes` variant | wire buffers it stands for |
|---|---|---|
| `bool` | `Bools(Buf[bool])` | `values`, one octet `00`/`01` per slot |
| `int8` … `int64`, `uint8` … `uint64` | `I8s` … `I64s`, `U8s` … `U64s` | `values` |
| `int128`, `decimal(p, s)` | `I128s` (a decimal's unscaled value) | `values` |
| `date` | `I32s` (days) | `values` |
| `time`, `timestamp` (`datetime`), `timestamptz` | `I64s` (microseconds) | `values` |
| `float32`, `float64` | `F32s`, `F64s` | `values` |
| `interval` | `Intervals(Buf[Ival])` | `values`, 16 octets |
| `uuid` | `Uuids(Buf[u128])` | `values` (RFC 9562 order on the wire) |
| `text`, `bytes`, `json` | `Bins(offsets: Buf[u32], data: Buf[u8])` | `offsets`, `data` |
| record | `Members(Buf[ColumnVector])`, one child per field, each `len` slots | — |
| union | `Tags(tags: Buf[u16], offsets: Buf[u32], children)` | `discriminants`, `offsets` |
| `bag<E>`, `list<E>` | `Elems(offsets: Buf[u32], child)` | `offsets` |

As with `Value`, **the type gives a variant its meaning**: `I128s` is an `int128` or a decimal's
unscaled integer, `I64s` an `int64` or a time type. A fixed-width `Buf[T]` on a little-endian host
has exactly the octets of the wire's `values` buffer, and `Bins`, `Elems` and the union's `offsets`
are the wire's buffers verbatim, so a direct encoder can copy them. The one difference is the union
discriminant, held as `u16` whatever the variant count; the wire narrows it to one octet when
`k ≤ 256`.

**Validity is a bitmap, as on the wire** (§6.3 rule 1: bit `i mod 8` of octet `⌊i/8⌋`), and it is
**empty when no slot is null**, which is also the wire's rule for a `null_count` of 0. A kernel tests
`validity.len() == 0` once per batch and runs a loop with no null test at all on that path, which is
the common case. A bitmap rather than a byte mask, because it is what the result carries and it is
an eighth of the size; the price is that turning a kernel's per-row `bool` into bits is a scalar
loop today (*gap: SIMD mask → bits*, §6). A `bool` **column** is one octet per slot, which is both
the wire's layout and the byte mask a predicate kernel produces, so a `Filter` reads its predicate
with no conversion.

**A null slot holds an unspecified value of its physical type**, not necessarily the §6.5
placeholder. Kernels never fail on a null slot's contents: a kernel that can raise an error (an
overflow, a division) tests validity before it tests the value. The result encoder writes the
placeholders.

**Nested relations and unions are dense, as on the wire.** A relation slot is a range of its child;
a union slot is a discriminant and an offset into the child of its variant, with the offsets
canonical (§6.3 rule 6), so the rows of variant `j`, in order, are exactly the slots of child `j`,
and a null union slot occupies no child slot. This is what makes `Match` cheap (§3.4).

**Compaction, not selection vectors.** A `Filter` keeps the rows its predicate holds for by taking
them into new vectors (`take`), so every vector an operator sees is dense and every kernel is a
plain loop over `0..<len`. A selection vector would save the copy for a chain of filters but put an
indirection into every kernel downstream. *Gap: compress* (§6) — `take` is a scalar gather loop.

Four structural operations are written once for every variant: `take(v, rows)` (gather, used by
filtering, sorting, and evaluating an expression on a subset of rows), `concat(ty, parts)`,
`vector_of(ty, values)` and `value_at(v, i)` (the bridge to `Value`, for tests, parameters, literals,
and the value-at-a-time paths of §3.5).

## 2. Batches and the operator interface

**A batch is at most `BatchSize` = 1024 rows** — small enough that a batch of fixed-width columns
stays in L1/L2 across a chain of kernels, large enough that the per-batch dispatch (one `match` on
the type and the function per expression node) is noise. A batch is **one `ColumnVector`**: for a
node exposing one binder (§6, "Exposes"), of the node's element type `E` — a record, a union
(§6.15) or a scalar; for a join, which exposes several, a composite of its binders' rows (§4.1).

Operators are **pulled**: `trait Operator { next_batch(*self) -> Result[Option[ColumnVector],
CodecError] }`. `Some` is a batch of at least one row — an operator never hands up an empty batch,
which matches §6.4 rule 1 for the result — and `None` is the end. An operator holds its input as an
`&Operator`, so a tree is built from trait objects and dispatches once per batch. `Sort`,
`Distinct` drain their input on their first call, and every join its right input (a hash join its
left input too when that is the smaller); the rest stream.

Expressions are evaluated over a batch against an `Env`: the binders visible at the node (§8.3),
each a name and a vector, and the execution's context — the parameter values (§10.1) and the
statement timestamp (§7.12). `Col`, `Row`, `Param`, `Null`, `Literal` produce a vector of the
batch's length; a constant is broadcast.

The executor takes a typed tree: `type_envelope` (`qtree_types.sysl`) gives the type of every `Rel`
and `Expr` node in pre-order (`Typing.nodes`), and `exec_plan.sysl` walks the tree in the same
order to attach each type to its node. **The executor never re-derives a type**: an arithmetic
call's result type, a comparison's common type and an `If`'s common type are the type checker's,
and the operands are brought to them with the coercion kernel.

## 3. Kernels

### 3.1 How they are specialised

One **generic function per operation over a type parameter**, monomorphised by the compiler, with
the type chosen by one `match` on the `Lanes` variant per batch: `int_arith[T: Integer + Checked]`
is compiled once for each of the nine integer widths, `cmp_lanes[T: Ord]` for every ordered
fixed-width type, `take_lane[T]` for every fixed-width type. The `match` is outside the loop, so
the loop body is straight-line code over `Buf[T]` that LLVM can unroll and auto-vectorise. Where a
type needs its own rule — floats' total order, a decimal's rescaling, octet strings — it has its own
function. No kernel is written in C, SIMD intrinsics or assembly.

### 3.2 Arithmetic, comparison and logic

- **Integers** (§4.4 rule 1): both operands are converted to the result type, then `checked_add`,
  `checked_sub`, `checked_mul`, `checked_div`, `checked_neg` (the machine's overflow flag). `div`
  truncates and `mod` follows the dividend, which is sysl's `/` and `%`. `MIN mod −1` is 0, not an
  overflow.
- **Decimals** (§4.4 rule 2): unscaled `i128`s, computed in `i256` (`i512` for `div`, whose
  dividend is scaled by up to 10⁷⁶) and checked against the result type's `10ᵖ` bound. `div` rounds
  half away from zero.
- **Floats** (§4.4 rule 3): IEEE in the operands' common type; `mod` is `fmod`. Never an error.
- **Comparisons** (§3.1–§3.3): the operands are coerced to their common type (`common_type` of
  `compare.sysl`, the same rule the type checker used), then a scalar kernel per physical type
  computes `eq` and the orderings with NaN equal to NaN and above `+∞`, and `−0 = +0`. Records,
  unions and relations are compared row by row through `compare.sysl`'s own `eq`, `lt`… and `cmp`,
  so there is one definition of their semantics; the scalar kernels are tested against the same
  functions on generated value pairs (`exec_agree_tests.sysl`).
- **Logic** (§7.12): `and`, `or`, `not` on `Bools` and validity, Kleene's three-valued tables.
  Both operands are evaluated for every row: `and` and `or` are non-strict in their *values*, not
  lazy in their *evaluation* (written into §7.12).

### 3.3 Errors inside kernels

A kernel answers `Result`; the first failing row of a batch stops the statement with the
`errors.md` code, and no partial batch is handed up:

| condition | code |
|---|---|
| integer or decimal overflow, a result outside its type's range, a coercion that does not fit (`compare.coerce`'s rule), calendar arithmetic past `int64` | `R0001` |
| `div`/`mod` by zero, integer or decimal | `R0002` |
| `assert_not_null` of null | `R0004` |
| a `strict` cast that does not convert; a negative `Limit` count or offset; a malformed `like` escape | `R0006` |

A `try` cast turns `R0006` into null for that row and nothing else. A row that is null is never
tested, so `div(null, 0)` is null.

### 3.4 Evaluation on a subset of rows

`If` must not evaluate an arm's value for a row whose condition did not select it (§7.5). The
executor evaluates each condition on the rows still undecided, takes the selected rows of every
binder into a smaller `Env`, evaluates the value there, and puts the pieces back in row order with
one `concat` and one `take`. `Match` (§7.7) does the same per arm, and its payload binder needs no
gather at all: the rows of variant `j` are exactly child `j`'s slots (§1). `coalesce` evaluates an
argument only on the rows that are still null, as `If` would (written into §7.12).

### 3.5 Value-at-a-time paths

Some work is per value whatever the layout: `Cast` to and from `text` (the forms of §3.5, through
`text_form.sysl`), casts of records, unions and relations, `like` and the case mappings. These
read and write vectors through `value_at`/`vector_of` around one function per value; they are
correct and slow, and are the first candidates for kernels when a benchmark asks.

## 4. Operators

- **Values** (§6.2): each row's expressions over a one-row `Env`, coerced to the column types, cut
  into batches. **ParamScan** (§6.3): the parameter's relation value cut into batches.
- **Filter** (§6.4): the predicate's `Bools`, AND its validity, become a row list for `take`.
- **Project** (§6.4): one vector per item, assembled into the binder's record.
- **Sort** (§6.9): drains its input, evaluates each key over all rows, and computes a permutation
  with the library's stable merge sort; the comparator compares key by key with `nulls_first` at
  the top level, `desc` reversing the non-null order, and `cmp` (§3.3) below the top level. A
  scalar key is compared on its typed lane; a record, union or relation key through `compare.cmp`.
  The sort is stable for every input, which §6.9 requires for a list and permits for a bag.
- **Limit** (§6.10): `count` and `offset` are evaluated once, before the first batch (they cannot
  see the input's binders); a negative one is `R0006`. Batches are sliced across their boundaries;
  `ties` keeps reading while a row is `same` on every key of the `Sort` below it.
- **Distinct** (§6.11): sorts every row by `cmp` over its fields (stable) and keeps the first of each
  run of rows that compare equal, which is `same` by §3.3. It is blocking; a hash-based, streaming
  Distinct waits on a fast hash (the survey's gap).
- **Aggregate** (§6.7, `exec_agg.sysl`): sort-based. It drains its input, evaluates the grouping
  keys over every row, orders the rows by them with `Sort`'s stable comparator (`cmp`, nulls first),
  and takes each run of rows that compare equal as a group, which is `same` by §3.3; with no keys every
  row is one group, empty or not. A call's `filter` becomes a row list and its arguments and `order`
  keys are evaluated on those rows only (§6.7 rule 5); each group is then a list of positions in the
  call's own vectors, sorted by the call's `order` and thinned by `distinct` (a stable sort of the
  group by its arguments, keeping the first of each run). `count`, `sum` and `bool_and`/`bool_or`
  run on the typed lanes; integer and decimal `sum` accumulates in `i256` and checks the result type's
  bound once per group (`R0001`, §6.7 rule 7), so the error does not depend on the rows' order; `min`
  and `max` choose a row per group with `slot_order` and gather it with `take`; `collect` is one
  `take` and the group bounds as offsets; `string_agg` concatenates the `Bins` directly. Hash
  aggregation, which streams and needs no sort, is intended once sysl has a fast seeded hash (the
  survey's gap, as for `Distinct`).
- **Tag** (§6.15): the rows coerced to the payload, wrapped as variant `tag`.
- **MemSource**: a list of batches, standing in for `Scan`.

**Output.** `result_batches` pulls the root and turns each batch into a `Batch` payload with
`encode_batch` (`encode.sysl`) through `value_at`, so the output decodes with `decode_batch`. A
direct encoder that copies the lanes (§1) is the fast path the layout was chosen for, and it is
not written yet. A root that exposes several binders is flattened first (§4.1).

### 4.1 Joins

The code is `exec_join.sysl`. Every kind of §6.5 — `inner`, `left`, `right`, `full`, `semi`,
`anti`, `mark`, `single` — is one operator, `JoinOp`, with two ways of finding the pairs of rows
whose `on` it tests.

**Composites.** A join exposes several binders (§6.5 rule 3, §8.2), so its batch is a *composite*:
a record with one member per exposed binder, named after it, holding that binder's rows as their
element type (made nullable on an outer join's null-supplying side, so that `Row(b)` of a
null-extended row is null). It is handed up under the binder name `Composite`, the empty string,
which no binder can have (§8.2 rule 2), and `Env.binder` looks a name up among a composite's
members. A join of a join flattens the inner composite into its own, so composites never nest.
`Filter`, `Sort` and `Limit` pass composites through untouched. Two consumers need the flat record
of every binder's columns in §8.4 order instead, which `flatten` builds: the result at the root
(and `Tag`, which coerces whole rows), and `Distinct`, whose `same` is over output columns.
`CompositeDistinctOp` compares rows by their flat columns but hands up composites, so the binders
stay visible above it.

**Hash join**, used whenever `on` has a conjunct `eq(a, b)` or `same(a, b)` (through any nesting
of `and`) with `a` reading only left binders and `b` only right ones; one of the two may instead be
a literal or a parameter, one value on every row (`same(true, d.k)` joins an uncorrelated subquery
back to its one-row domain). Each key is evaluated over each side and coerced to the two operands'
common type, and each column of keys is hashed a vector at a time (`value_hash.sysl`, §3.4): rapidhash
(`sysl.checksum.hash64`) over each scalar slot's canonical words — every NaN alike, `-0.0` as
`+0.0`, a number by its value with trailing decimal zeros dropped, so that the integer widths and the
decimal scales agree — a record's fields in order, a union's discriminant and payload, a list's
elements in order and a bag's as a wrapping sum, which no order changes. A row's hash combines its
keys'. **The table goes on the smaller side**: the right input is drained, and the left read ahead
until it holds as many rows; the right rows are then built on and the left input probes the table a
batch at a time as it streams, or, when the left input ended with fewer rows, the left rows are built
on and each right row probes them. A table groups its rows by `same` keys (open addressing on the
hash; keys whose hashes agree are compared with `same_slots`, so a collision costs a comparison and
never a wrong match), and every pair of a probing row and the group it finds is a candidate. **`eq`
never matches a null**: under `eq`, a row whose key holds a null at any depth (a null field of a
record key, a null payload of a union key) is neither built on nor looked up, since `eq` of it is null
or `false` and never `true` (§3.2); under `same` nulls are ordinary values and match each other
(§3.1). The other conjuncts of `on`, the residual, are tested on the candidates only.

**There is no merge join.** One would sort both sides by their keys only to find the runs of equal
keys, draining both inputs to do it, where the hash join finds the same pairs with no sort and drains
one side; and a join's output is a bag (§6.5 rule 7), so no consumer could use the order a merge
leaves.

**Block nested-loop join**, for every other `on` (and an absent one, a cross join). The right input
is drained; each left batch is paired with every right row, row-major.

**Common to both.** Candidates are tested `BatchSize` pairs at a time: the pairs' rows are gathered
into one composite with `take` and `on` (or the residual) is evaluated over it like any expression.
Per left row the operator counts matches (to two) and notes whether `on` was null for some pair; per
right row it notes a match. Once a left row's pairs are all tested — at the end of its batch for the
nested loop and for a hash join probing with the left rows, after the last right row has probed when
the left rows are built on — it is finished: null-extended (`left`, `full`,
`single` with no match), kept or dropped (`semi`, `anti`), or marked. A second match for one left
row under `single` is `R0003` at once. After the last left row, `right` and `full` null-extend the
right rows nothing matched. Output is queued as pieces of at most `BatchSize` rows and handed up in
batches of exactly `BatchSize` until the last, never empty.

**Semi, anti and mark joins whose `on` is keys alone** test no pair: a left row whose keys find a
group of right rows has a right row making `on` `true`, which is all they ask; built on the left, a
group a right row finds answers each of its left rows at once.

**A mark's null** (§6.5 rule 5) needs to know whether `on` was null for some pair, which a hash
join's keys do not say for a left row they did not match. When `on` is `bool` it never is. Otherwise
a pair can have `on` null only where its `same` keys agree (`same` is never null), some `eq` key
holds a null on one side, and the `eq` keys holding none on either side agree (two non-null keys that
differ are `false`; a pair whose keys all agree was a candidate and was tested). So the right rows
are grouped by which `eq` keys hold a null (a mask), and each unmatched left row looks up, in each
group, the rows agreeing with it on the `same` keys and on the `eq` keys neither holds a null in, in a
hash table of the group by those keys, built once per pair of masks and kept for every later left
batch. With scalar keys and no residual a row found decides the mark (null); otherwise `on` is
evaluated on the pairs found. A `not in` over a subquery with nulls is therefore a lookup per row and
mask, not a pair per right row.

**Work counts.** Every operator of an execution shares one `Work` (`exec_eval.sysl`, in `Ctx`): the
pairs a join tested `on` on, the rows a hash join built tables of and looked up, and the key
comparisons agreeing hashes cost. They depend only on the plan and the rows, so a test bounds them to
catch a plan that does quadratic work, whatever the machine's speed (`unnest_work_tests.sysl`,
`exec_hash_join_tests.sysl`). `Work` also carries the hash the joins use, `key_hash()`, which a test
narrows to force collisions and prove that the keys are re-checked.

**When `on` is evaluated** is §6.5 rule 8, written into the spec by this work: only on pairs, so
never when a side is empty (the hash join evaluates no key then), and a conjunct may go unevaluated
on a pair another conjunct already makes not `true` — which is what lets the hash join skip the
pairs whose keys differ.

**An empty right input ends an `inner` or `semi` join** before its left input is read, since neither
can emit a row (§6.5 rule 10). The unnester relies on it: it joins each uncorrelated subtree to the
outer side's domain with the domain on the right, so over an empty outer side the subtree is never
evaluated and raises nothing (§6.6 rule 4).

**A hash join built on the right streams its left input**: it reads ahead only as many left rows as
the right has, and hands up output as each left batch is probed. Built on the left, it has read the
left input whole, which then held fewer rows than the right.

### 4.2 Nested relations and correlation

The code is `exec_nest.sysl`.

**Correlation is unnested first.** `run_query` hands every query to the unnester (`plan_unnest.sysl`,
`docs/design/unnesting.md`) before planning it, which replaces each `Apply` and subquery expression it
can decorrelate by joins against the outer side's distinct bindings (§6.6 rule 2). The executor meets
rule 2 except for the cases `unnesting.md` §3 lists, which keep the per-row form below.

**What is kept is per row, by re-planning.** A correlated subtree — an `Apply`'s right input, the
`rel` of a `Relation`, `Scalar`, `Exists` or `In` — is kept as a `SubPlan`: the `Rel`, the position
of its types in `Typing.nodes`, and the outer rows already bound where it stands. The planner plans it
once where it stands, only to step over its types and learn its output's shape. At run time it is
planned again for every outer row, with that row's binders (one-row vectors, a composite searched by
member) as `Planner.outer`; every `Col` or `Row` of one of them becomes a `Param` whose value is
appended to that plan's own copy of the parameters. The subtree's operators therefore never see a
binder they do not expose, and a nested subquery captures the outer rows of every level above it.
This is §6.6 rule 3's per-row meaning, the reference the unnester is checked against: its tests run
every query both ways.

- **Apply** (§6.6): per left row, the right input re-planned for the row and joined to that one row
  by the nested-loop `JoinOp` of the corresponding kind (`cross` as `inner`, `outer` as `left`), so
  `semi`, `anti`, `mark` and `single`, `on`'s three values and `R0003` are §4.1's code. With an empty
  left input the right input is never planned or run (§6.6 rule 4, written into the spec by this work).
- **Relation, Scalar, Exists, In** (§7.9–§7.11): per row of the batch, the subtree is run to the end
  and flattened. `Relation` is offsets and one `concat` of every row's rows; `Scalar` is the one
  column of the one row, null for none, `R0003` for two; `Exists` whether a row came; `In` compares
  the row's tuple with each row's columns under `eq` in their common type, value at a time. An `If`
  evaluates them only on the rows its arm selects (§3.4).
- **Nest** (§6.14 rule 1): planned as the `Aggregate` the rule says it equals — the same keys and one
  `collect` of a `Record` of the items with the `order` — so it is sort-based, like `Aggregate`.
- **Unnest** (§6.14 rule 2): per input batch, `value` evaluated, and each slot's range of the
  relation's child taken with one `take`; a padded row (`outer`, empty or null relation) takes a null
  slot appended after the child. The output is a composite, the input's binders and `as` (the element,
  or with an ordinality a record of the element's columns and the 1-based position), cut into
  `BatchSize` batches.

### 4.3 Windows

The code is `exec_window.sysl`. **Window** (§6.8) is sort-based and blocking: it drains its input and
computes each call over every row on its own. A call evaluates its partition values and `order` keys
on every row and orders the rows with `Sort`'s stable comparator (`KeyOrder`, `sorted_rows`), the
partition values first (ascending, nulls first) and its own keys after, so a partition is a run of
positions and a peer group (§6.8 rule 1) a run within it; a `Layout` records each position's
partition bounds and peer group. A call's value is computed position by position and gathered back
into input order with `take`, which the output keeps. The output is a composite (§4.1): the input's
binders and the window's record, one member per call.

- **Ranking** is arithmetic on the layout: the position, the first peer's position and the peer group
  number, each less the partition's first.
- **lag/lead** evaluate `x`, `n` and `d` on every row; each row takes one slot of `x`, of `d`, or a
  null by one `take` over their concatenation.
- **Frames** become a pair of positions per row. `rows` and `groups` bounds are arithmetic on the
  position and the peer group, clamped to the partition. A `range` offset bound is a binary search of
  the partition for the first row not before (a start) or after (an end) the bound's value, the
  comparison placing nulls per `nulls_first`; the value is computed exactly in `i256` for integers and
  decimals, in `float64` for floats, and by the calendar `add_interval` for date and time keys.
  `exclude` cuts a frame into at most three runs of positions.
- **Aggregates over a frame slide** (`exec_window_slide.sysl`). Within a partition a frame's start
  and end never move back from one position to the next, and an exclusion leaves at most two runs
  (before and after the excluded row or peers) and the current row `exclude ties` keeps, each run's
  bounds moving forward too. Each run keeps an accumulator that the positions passed by its end enter
  and the positions passed by its start leave; a run that moves back or jumps past its own end, as at
  a partition's first position, is emptied and refilled. So the work is linear in the rows, whatever
  the frames' width:
  - `count`, an exact `sum` or `avg` (an `i256` sum and a count) and `bool_and`/`bool_or` (counts of
    values and of trues) are removable; the runs' states and the kept row add, and `avg` rounds the
    total once, with `Aggregate`'s own rounding and `R0001` bounds.
  - `min`/`max` keep a monotonic deque of positions per run: a value entering pops the candidates it
    strictly beats, equal ones stay, and the start passing a candidate drops it, so the front is the
    earliest of the best, the pick `Aggregate` makes. The runs' fronts and the kept row combine in
    frame order.
  - a float `sum`/`avg` adds in the frame's order from its first value, as `Aggregate` does, and IEEE
    addition can not be taken back exactly: it slides only by adding, and sums the frame again
    whenever its start passes a value. A running or `unbounded preceding` frame is linear; a sliding
    one costs the sum of its frames' sizes, as from scratch. Under an exclusion a float sum, and
    `collect` and `string_agg` always (whose results are as large as the frames), take the scratch
    path: each row's frame, as input row numbers in the partition's order, is one group of a `Seen`
    for `aggregate_column`, handed over in runs of about 65 536 positions.

  Every sliding result is bit for bit the scratch path's, which the tests check slot by slot over
  every frame shape and exclusion, and the work is counted in positions moved.
- **first_value/last_value** take the first or last position of the frame's runs, or a null.

Hash partitioning, which would need no sort of the partition values, waits on a fast seeded hash
(the survey's gap, as for `Distinct` and `Aggregate`).

### 4.4 Set operations and CTEs

The code is `exec_setop.sysl`, with the shape check for recursion in `qtree_recursion.sysl`.

- **SetOp** (§6.12) coerces every input's batches positionally to the output element type
  (`drain_coerced`). `union all` streams its inputs in order, one batch at a time. Every other
  combination is sort-based and blocking: the inputs are drained and laid end to end, the rows are
  ordered by `Sort`'s stable comparator over every column (`row_orders`, shared with `Distinct`), and
  each run of `same` rows keeps a count of its first rows computed from how many each input holds
  (`copies_of`). Because the sort is stable and the inputs are laid in order, the rows kept by
  `intersect` and `except` are the first input's. Hashing waits on the survey's fast seeded hash.
- **With** frames each CTE as a `CteStore` in scope while its later CTEs (or, for a recursive `With`,
  all of them) and its body are planned. A store holds the CTE's planned operator and drains it on the
  first read only, so every `CteRef` sees one value (§6.13 rule 2) and an unread CTE is never run.
  **CteRef** is a `CteScanOp` over its store's batches, coerced to the reference's element type.
- **Recursive CTEs** are a `RecursiveOp`: the seed is planned once; each recursive input is kept as a
  `SubPlan` and planned again at every step, with the CTE's frame bound to a store holding only the
  previous step's new rows. Without `all`, a step's rows are cut by a set `except` against every row
  found so far. The loop ends on a step with no new rows, and the 10001st step that yields new rows is
  `R0005` (`RecursionSteps`).
- **Operators keep what they read.** A store is an `&Operator` reachable from a `SubPlan`, so the
  `Operator` trait's `next_batch` carries `@keeps(self)`: an operator keeps the children and stores it
  reads through `self` beyond the call.

### 4.5 The store: Scan and DML

The code is `exec_scan.sysl` and `exec_dml.sysl`; `run_statement` runs an envelope in a store
transaction (`docs/design/store.md`) and answers `Ran` (the row count and, with a result, its batches)
or `Blocked` on a row lock. At read committed it first takes the statement's snapshot. Any error —
validation, runtime, a constraint, the store's — aborts the transaction with its code
(`isolation.md` §5.1); a statement on an ended transaction is `W2001` or `W2003`.

- **Scan** (§6.1): `ScanOp` reads the rows visible at the snapshot in its access path's key range
  (§4.6) from the store on its first pull, in key order, and hands them up in `BatchSize` batches. A
  correlated scan is planned again per outer row (§4.2) and so read and logged once per row. The
  store decodes the rows straight into the batches' vectors (`Store.scan_vectors`,
  `store_scan.sysl`): a row whose current image the reader sees, in the table's current schema
  version, is read where its page holds it, its key components decoded as values and each scalar or
  octet-string column from its stored octets into its lane, with no image, record or per-column
  value built; every other row (rebuilt through the undo log, `LOCK_ONLY`, an older schema version)
  takes the general path and is appended as values. A non-key column the statement does not read
  through the scan's binder (the logged predicate's columns, below) is not decoded at all, and its
  slots hold its type's placeholder; so a damaged value in an unread column is found by the first
  read that decodes it, while the row's layout is checked on every read. A DML target decodes every
  column, since an updated row is written back whole. `Store.reads` counts the work (`ReadWork`).
- **The logged predicate** (`isolation.md` §4.4). A `Filter` directly over a `Scan` is the scan's
  logged predicate unless it holds a subquery: the store keeps it as a closure that evaluates the
  typed filter on a one-row vector of a write image, counting an error or an image of another schema
  version as a match, which only widens it. The filter itself still runs vectorised as a `FilterOp`.
  Before planning, the statement is walked once for what each binder reads (`Uses`): the columns
  named through it (`C`), and whether its rows reach a result, a whole-row operator or a subquery
  whole, which reads every column. A projection, aggregate or nest above a scan narrows it to the
  columns their expressions name.
- **DML** (§9). A statement reads everything first and writes last (§9.1). `Update`/`Delete` scan the
  target (logging `filter` when there is no `from`/`using`, its key range chosen from `filter` as a
  `Filter`'s is, §4.6), join `from`/`using` by `filter` with
  `plan_join` or filter the scan, refuse a target key matched twice (`R0007`), evaluate `set` over the
  matched rows vectorised, and coerce each value into its column. `Insert` drains its source, places
  each column by name with its default (evaluated per row) or null (`Q3005` for a NOT NULL column with
  neither). An upsert walks the source rows in order: a key this statement already wrote conflicts with
  that write, any other is looked up in the store (a logged point read), and `where` and `set` are
  evaluated over the one row with `as` and `excluded` bound. Generated columns are computed for
  inserted rows and for updated rows when they read a set column; check constraints run on inserted
  rows and on updated rows when they read a changed column (`T3002`), and NOT NULL on every written
  row (`T3004`). Definition expressions run as a `Project` over the rows passed as a relation-valued
  parameter, type-checked against the catalog. `returning` runs before the writes, grouped by action
  so that `Action()` is one parameter per group, and is put back in statement order with one `take`.
  The writes go to the store as one `write_rows` call, removals first (§9.4 rule 4). An upsert's
  target may be any unique key; its conflict is looked up with `lookup_unique`. The statement's foreign
  keys then act and are checked (`fk_effects`, `ddl.md` §2.6 and `docs/design/store.md`): cascades
  write the referencing tables as further `write_rows` calls, and a statement that must wait part-way
  is taken back with `undo_statement`.
- **Views** (`exec_view.sysl`, `ddl.md` §2.9): `scan_op` of a view answers a `ViewOp`, which types
  and plans the stored query at its first pull with a planner of its own — its own binders, CTEs and
  logged reads — over the same transaction. The privileges a query or DML statement needs are checked
  there too, at the engine's validation (`statement_privileges`).
- **Sequences** (`exec_sequence.sysl`, `ddl.md` §2.7): `Ctx.draws` holds the store and the role a
  `next_value` by name is checked for; a draw changes the catalog's sequence in place, outside every
  transaction. An insert draws identity values per row left without one. **Zones**
  (`exec_zone.sysl`): `at_zone` and `from_zone` read the host's zone database through
  `sysl.posix.time`.

Not yet: translation of a row of an older schema version (`ddl.md` §5.3).

### 4.6 Access paths: the primary key and secondary indexes

The code is `exec_access.sysl`. A scan of a table that a `Filter` sits directly over, and the target
scan of an `Update` or `Delete`, read only the range of the table's B+tree that the filter's
comparisons of the **primary key** allow; the filter itself still runs over every row read.

- **Which conjuncts count.** The filter is split into its `and` conjuncts at any nesting. A conjunct
  counts when it is `eq`, `lt`, `le`, `gt` or `ge` between a key column of the scan's own binder (a bare
  `Col`, on either side) and an operand fixed for the statement: a `Literal`, a `Null`, a `Param` or a
  `Cast` of these. An outer row's value is a `Param` by the time the filter is planned (§4.2), so a
  correlated subquery's key comparison counts, looked up afresh per outer row. `BETWEEN` arrives as
  `ge` and `le`. An `or`, a `not`, `ne`, `is_null`, a call and a subquery never narrow the scan.
- **Values.** The operand is evaluated once at planning. A null makes the conjunct, and so the
  filter, never true: the scan reads nothing (`NoRows`). An operand that fails to evaluate is dropped,
  so its error is raised by the filter on the rows it reads, exactly as without the access path. The
  operand is encoded under the key column's §4.6 encoding when its type is the column's, or when both
  are integers and the column's type holds the value (an `int32` against an `int64` key, a
  non-negative `int64` against a `uint32` one); any other pair, a value out of the column's range and
  a column whose type in the tree is not the catalog's leave the conjunct to the filter.
- **The range.** Equalities on a leading run of the key's columns fix a prefix of the key's encoding,
  which no other component's encoding extends (§4.6 rule 3), so the keys beginning with it are exactly
  the rows holding those values: `KeyLookup` when every column is fixed, a range otherwise. The column
  after the run may take one bound on each side, the tighter one where several are given:
  `>= v` starts at prefix·v, `> v` at the successor of prefix·v (the least string above every string
  beginning with it), `< v` ends before prefix·v and `<= v` before its successor. A condition on a
  later column, or on a column after a gap, is left to the filter. Inverted bounds make an empty range,
  which the B+tree cursor ends at once.
- **Semantics.** The range holds every row the filter keeps, so the result is the full scan's, in the
  same key order. The read is logged with the same predicate as before (`isolation.md` §4.4: the
  filter, which implies the range, so `key ∈ [a, b) ∧ F` is `F`), and it is logged even when the range
  is empty or a null made it `NoRows`, so a phantom inserted into the range still fails a serializable
  commit. Visibility is per slot and every version of a row lives at its key's slot, so a range read
  sees exactly the versions a full scan would. The rows outside the range are never filtered, so a
  runtime error the filter would raise only on them does not occur (`query-tree.md` §6.4 rule 3).
- **Measured, not timed.** `Store.slots_read` counts the leaf entries the executor's scans visit; the
  tests assert each access path visits only its range, and a differential test compares random
  predicates with the same predicate under `or false`, which is never narrowed.

#### Secondary indexes

The same conjuncts may instead narrow the scan to a range of a secondary index's entries
(`store_index.sysl`, `Store.scan_index`); the tests are `exec_access_index_tests.sysl`.

- **Which indexes.** Only an index whose state in the transaction's catalog is `ready` (`ddl.md` §2.5
  rule 5), unique or not, a `unique` constraint's included; one still `building` — in the transaction
  that created it, or a constraint's whose validation is deferred — is never read through.
- **Which conjuncts count.** As for the primary key, a comparison `eq`, `lt`, `le`, `gt` or `ge`
  whose one operand is fixed for the statement (a `Literal`, `Null`, `Param` or `Cast` of these) and
  whose other is a component of the index: a bare `Col` of the scan's binder for a column
  component, or, for an expression component, an expression that **is** the definition node for
  node — the same columns of the binder, literals, calls, casts and field accesses (`matches_def`).
  An equivalent but different expression (`lower(lower(s))`, `200 / d` for `100 / d`) does not count.
  The operand's value is converted to the component's type as a key column's is (same type, or
  integers by value); any other pair is left to the filter.
- **Partial indexes** are used only when every conjunct of the index's predicate (split at its `and`s)
  is, node for node, one of the filter's conjuncts. This is an exact-match check, which §2.5 rule 3
  allows: `k > 1` does not count as implying `k > 0`, and a predicate missing a conjunct is not used.
- **The range.** Entries are compared as values in the total order (`query-tree.md` §3.3), so a
  component's direction does not matter. Equalities on a leading run of the components fix a prefix of
  the entry's value; the component after them may take one bound on each side, the tighter one where
  several are given. A bound on that component also excludes its null entries, which sort first and
  satisfy no comparison. An entry's value holds its null components (the index of a nullable column
  must find `b = 'x'`'s rows whose next component is null).
- **Reading.** The store visits the entries in the range, fetches each row they name by key (each
  once, in key order, so the result is the full scan's, order included), reads its version visible at
  the snapshot and keeps it unless that version's own entry lies outside the range — an entry is never
  removed, so it may name a row whose version has left the value, or that an aborted transaction
  inserted. A row whose visible version has no entry (a partial index's predicate not true of it) is
  handed up for the filter to reject. A row one of whose versions the index's definition could not be
  evaluated over has no entry and is kept in `unindexed`; every read through the index fetches it, so
  the filter raises the error a full scan would raise on it.
- **Decoding** (`IndexScanOp`, `Store.scan_index_vectors`). The rows fetched are built into vectors
  as a `ScanOp`'s are (§4.5): through a plain index (every component a column, no predicate), a row
  whose current image the reader sees, in the current schema version, has its entry read from the
  index's columns of that image, and of its other non-key columns only those the statement reads
  are decoded. A row rebuilt through the undo log, a `LOCK_ONLY` one, one of an older schema version,
  and every row read through an index over expressions or with a predicate take the general path
  (the visible version rebuilt, its entry evaluated, translated) and are appended as values. A DML
  target reading through an index decodes every column, as through the primary key.
- **Logged.** The read is logged with the same predicate as a full scan's, the filter (`isolation.md`
  §4.4: `key ∈ [a, b) ∧ F` is `F`, since `F` implies the range), found or not, so a phantom inserted
  into the range, a row updated into or out of it, and write skew through it fail a serializable
  commit exactly as they do through a full scan.
- **The choice** (`access_of`), one rule and no cost model:
  1. a conjunct comparing any column of the binder with a fixed null reads nothing (`NoRows`);
  2. else a lookup of the whole primary key;
  3. else a `ready` unique index with every component fixed by an equality, the first in catalog order;
  4. else the primary-key range or index range fixing the most leading components, then the one whose
     next component is bounded, ties going to the primary key and then to the earlier index; a path
     fixing and bounding nothing is no path, and with none the scan reads every row.
- **Measured.** `Store.entries_read` counts the index entries visited and `Store.rows_fetched` the rows
  fetched by key; a read through an index visits no leaf entry (`slots_read`). A differential test
  compares random predicates over random data, with stale entries, against the same predicate under
  `or false`, in a reader's snapshot and in a writer's own uncommitted state.

The referencing side of a foreign-key check (`Store.referencing`, `store.md`) reads through a `ready`
index with no predicate whose leading components are the referencing columns in order, when the table
has one, and makes a pass over the whole referencing table otherwise.

### 4.7 Predicate pushdown

The code is `plan_pushdown.sysl`. SQL's comma joins and `CROSS JOIN` arrive as a cross join under a
`Filter`; after unnesting, the tree is rewritten so that each conjunct of a `Filter` over joins, and of
a join's `on`, is evaluated as low as it may be, and the rewritten tree is type-checked again before it
is planned. The join itself chooses its keys from its `on` (§4.1), so the rewrite targets "an inner
join of these inputs with these conjuncts in its `on`", whatever the equi-join's algorithm.

- **Safe conjuncts.** Only a conjunct that can raise no runtime error and draws nothing volatile
  moves below a join: one built of `Col`, `Row`, `Field`, literals, nulls and parameters with `eq`,
  `ne`, `lt`, `le`, `gt`, `ge`, `same`, `is_null`, `is_not_null`, `and`, `or`, `not` and `coalesce`
  (`query-tree.md` §4.1's implicit coercions are lossless, so a comparison's raise nothing). Evaluating it on more
  rows or fewer changes no error. Any other conjunct — arithmetic, a cast, `next_value`, a
  subquery — stays where it was written; above a region it is evaluated on the rows the region
  keeps, which `query-tree.md` §6.4 rule 3 allows, since a safe conjunct is not `true` on the rows
  removed.
- **Inner-join regions.** A tree of `inner` joins whose `on` is absent or safe, with the safe
  `Filter`s directly over or between them, is one region: its leaves (the inputs that are not such
  joins) and a pool of conjuncts. A `Filter` over a join and the join with those conjuncts in its
  `on` have one result (§6.4 rule 3, §6.5 rule 8). Each conjunct is placed at the lowest point that
  sees every binder it reads: a conjunct of one leaf becomes a `Filter` over that leaf (taken into a
  safe `Filter` already there, so a scan's access path sees it, §4.6); one of several leaves goes into
  the `on` of the join that first meets them, where an `eq` across its inputs is a key; one reading no
  leaf (a constant such as `1 = 0`, or an outer row's value) filters the leftmost leaf.
- **Shape.** The region keeps its own tree unless every leaf is safe to evaluate — a scan of a table
  (not a view), a `ParamScan`, and filters, projections, `Distinct` and joins of these by safe
  expressions — since only then can the shape not change which errors are raised. The bracketing of
  the leaves *in their order* with the fewest joins that no key joins is then taken if it has fewer
  than the region's own (`a, b, c where a.k = c.k and b.k = c.k` is `a ⋈ (b ⋈ c)`), which keeps the
  columns in §8.4 order. Where the region's columns are read only by name — under a `Project`,
  `Aggregate` or `Nest`, through `Filter`, `Sort`, `Limit`, `Distinct`, `Window` and `Unnest` — and
  that still leaves such a join, a left-deep order joining next the first leaf a key joins to those
  joined so far is taken if it has fewer still (`a-c, a-d, b-d` is `((a ⋈ c) ⋈ d) ⋈ b`). At the root,
  in a `SetOp` input or under a `Tag`, the leaves' order is the result's and is never changed.
- **Empty right inputs.** An `inner` join's empty right input ends it before its left input is read
  (§4.1, §6.5 rule 10). A conjunct moved into a right input can make it empty, which would leave
  the left input's errors unraised, so a conjunct moves into a right input only when the left input
  beside it, at every level of the region, is safe to evaluate; otherwise it waits in the `on` of
  the join above.
- **A join whose `on` is not safe** is evaluated on every pair of its inputs: its safe `on`
  conjuncts reading one input move into it (the rest may go unevaluated on a pair they make not
  `true`, §6.5 rule 8), into the right input only beside a safe left input, and a `Filter` over it
  keeps every conjunct, since fewer pairs would leave `on`'s errors unraised.
- **Outer joins.** A safe `Filter` conjunct that rejects a side's nulls — a comparison, or
  `is_not_null`, with a bare column of that side as an operand (§3.2: null when an operand is) —
  removes every row that side was null-extended in, and is not `true` on them, so the rest of the
  filter may go unevaluated there: `left` with its right side rejected and `right` with its left
  side rejected are `inner`, and `full` is `right`, `left` or `inner`. It becomes `inner` only when
  its left input is safe to evaluate, by the rule above. With the join still outer, a safe `Filter`
  conjunct reading only a `left` or `right` join's preserved side moves into that side when `on` is
  safe (a row it removes takes all its output rows with it, and `on` would have raised nothing on
  their pairs); a safe `on` conjunct reading only the null-supplying side moves into it (a row it
  removes was null-extended either way). Nothing moves past a `full` join that stays one, and a
  `coalesce` or `is_null` of the null-supplying side is never taken as rejecting.
- **Result type.** An outer join made `inner` makes the columns it null-extended non-null, so the
  rewritten tree's result type may be the query's with fewer nullable types; `unnest_plan` accepts it
  when it coerces to the query's (`query-tree.md` §4.1), and the rows are sent under the query's type. A rewritten
  tree that does not type is a fallback, and the unnested tree is planned instead.
- **Errors, as the tests check them.** A division by zero on a row whose key matches nothing is not
  evaluated, as §6.4 rule 3 allows (as written the filter evaluates it on every pair and raises
  `R0002`); on a row a key matches it is raised either way; over an empty side nothing is evaluated;
  and a key lookup chosen by a pushed conjunct never reads the row the division would fail on.
- **Measured.** `plan_pushdown_tests.sysl` counts the pairs the joins tested (`Work`): a 2-, 3- or
  4-way comma join of different keys tests only the pairs of equal keys, and a pushed key conjunct
  is checked by the leaf entries the store's scans visited. A differential test runs random comma,
  inner, cross and outer joins over random tables with nulls and duplicates pushed down and as
  written.

## 5. What this note relies on in `query-tree.md`

§3.1–§3.3 for every comparison and for `Sort` and `Distinct`; §3.1 and §3.4 for the hash join's keys; §4.1–§4.2 for
the coercions the executor applies; §4.4 for arithmetic; §6.2–§6.15 for
the operators; §7.1–§7.12 for the expressions; §8.2–§8.4 for the binders a join exposes and
its output columns; §10.1 for parameters. Rules written into it by this work: the evaluation of
`and`/`or` and of `coalesce`, the text functions' units and `like`'s escape, and the calendar `add`
(all in §7.12); when a join's `on` is evaluated and `single`'s `R0003` (§6.5 rules 8 and 9); a
group's key, `filter`, `distinct`, and the values of `sum`, `min`, `max` and `string_agg` (§6.7 rules
5–7); when an `Apply`'s right input and a subquery are evaluated (§6.6 rule 4); a keyless `Nest` of no
rows, and an `Unnest` without `outer` of an empty or null relation (§6.14 rules 1 and 2); and a
window's partitions, peers, ranking, `lag`/`lead` offsets, default frame, the meaning of each frame
unit and exclusion, `range` offsets over every key type, and when offsets are evaluated and refused
(§6.8 rules 1–5); a set operation's left fold and its nulls as duplicates (§6.12); that an unread
CTE is never evaluated, the places a recursive reference may not stand, and the 10000-step bound
(§6.13 rules 2–4); and a DML statement's visibility of its own writes and when it waits (§9.1), an
upsert's conflict with a row the same statement wrote (§9.3 rule 3), a primary-key update (§9.4 rule
4), and when `returning` is evaluated (§9.5). From `isolation.md`: §4.1–§4.4 for scans and their
logged predicates, to whose §4.4 table this work added a DML statement's target rows and an upsert's
conflict check, and the columns `C` a read uses.

## 6. Where the survey's sysl gaps cost speed

The survey (`docs/phase0/sysl-survey/FINDINGS.md`, row 6) found no SIMD mask → bits, no gather and
no compress; sysl 0.13.5 has `mask.bits()`, `gather`, `compress` and `table.lookup`, and `hash64` and `crc32c`
(see its 0.13.5 status), so the gaps named in this document are now closed in sysl and open only in this code. Every
kernel is a plain loop the compiler may auto-vectorise; these are the ones the gaps held back:

- **mask → bits**: building a validity bitmap from a kernel's per-row result (every comparison,
  `is_null`, `Field` of a nullable record); `Filter`'s row list.
- **compress**: `Filter` turning its predicate into the rows it keeps, and `take` after it.
- **gather**: `take` on every fixed-width lane (filtering, `Sort`'s permutation, `If`/`Match`
  reassembly), and `Col` of a union's shared column.

Arithmetic, comparison and logic on fixed-width lanes with no nulls are straight loops and
vectorise without any of the three.
