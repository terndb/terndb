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
and the conversion of an operator's output into result batches. Views, identity columns, disk
storage and the unnesting planner come later.

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
| `time`, `datetime`, `timestamp` | `I64s` (microseconds) | `values` |
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
`Distinct` and a merge join drain their input on their first call, and every join its right input;
the rest stream.

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

**Merge join**, used whenever `on` has a conjunct `eq(a, b)` or `same(a, b)` (through any nesting
of `and`) with `a` reading only left binders and `b` only right ones. Both inputs are drained. Each
key is evaluated over each side, coerced to the two operands' common type, and both sides sorted by
the keys with `cmp` (§3.3) through one vector of both sides' keys, so that a left and a right row
compare as two rows of it. Runs of equal keys on the two sides are a group, and every pair of a
group is a candidate. **`cmp` orders nulls, but `eq` never matches them**: under `eq`, a row whose
key holds a null at any depth (a null field of a record key, a null payload of a union key) never
enters a group, since `eq` of it is null or `false` and never `true` (§3.2); under `same` nulls are
ordinary values and match each other (§3.1). The other conjuncts of `on`, the residual, are tested
on the candidates only.

**Block nested-loop join**, for every other `on` (and an absent one, a cross join). The right input
is drained; each left batch is paired with every right row, row-major.

**Common to both.** Candidates are tested `BatchSize` pairs at a time: the pairs' rows are gathered
into one composite with `take` and `on` (or the residual) is evaluated over it like any expression.
Per left row the operator counts matches (to two) and notes whether `on` was null for some pair; per
right row it notes a match. Once a left row's pairs are all tested — at the end of its batch for the
nested loop, after the last group for the merge — it is finished: null-extended (`left`, `full`,
`single` with no match), kept or dropped (`semi`, `anti`), or marked. A second match for one left
row under `single` is `R0003` at once. After the last left row, `right` and `full` null-extend the
right rows nothing matched. Output is queued as pieces of at most `BatchSize` rows and handed up in
batches of exactly `BatchSize` until the last, never empty.

**A mark's null** (§6.5 rule 5) needs to know whether `on` was null for some pair, which a merge
join's keys do not say for a left row they did not match. When `on` is `bool` it never is. With one
scalar `eq` key and no residual, the mark is null exactly when the left key is null or some right
key is (and there is a right row). Otherwise `on` is evaluated on that left row's pairs with every
right row.

**When `on` is evaluated** is §6.5 rule 8, written into the spec by this work: only on pairs, so
never when a side is empty (the merge join evaluates no key then), and a conjunct may go unevaluated
on a pair another conjunct already makes not `true` — which is what lets the merge join skip the
pairs whose keys differ.

**An empty right input ends an `inner` or `semi` join** before its left input is read, since neither
can emit a row (§6.5 rule 10). The unnester relies on it: it joins each uncorrelated subtree to the
outer side's domain with the domain on the right, so over an empty outer side the subtree is never
evaluated and raises nothing (§6.6 rule 4).

**Hash join is the intended equi-join** once sysl has a fast seeded hash (the survey's gap, §6):
build on the smaller side, probe streaming, with the same composite batches and the same per-row
bookkeeping. Until then the merge join is the equi-join; it is blocking on both sides, where a hash
join would block on the build side only.

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
- **Aggregates over a frame** reuse `Aggregate`'s kernels unchanged: each row's frame, as input row
  numbers in the partition's order, is one group of a `Seen`, and `aggregate_column` computes every
  group at once. The frames are handed over in runs of about 65 536 positions, so memory stays
  bounded, but the work is the sum of the frames' sizes, quadratic in a partition for an unbounded or
  running frame. Removable accumulators for `count`, `sum`, `avg`, `bool_and`/`bool_or` and a
  segment tree for `min`/`max` are what would make that linear; they are not written yet.
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

- **Scan** (§6.1): `ScanOp` reads every row visible at the snapshot from the store on its first pull,
  in key order, and hands it up in `BatchSize` batches. A correlated scan is planned again per outer
  row (§4.2) and so read and logged once per row.
- **The logged predicate** (`isolation.md` §4.4). A `Filter` directly over a `Scan` is the scan's
  logged predicate unless it holds a subquery: the store keeps it as a closure that evaluates the
  typed filter on a one-row vector of a write image, counting an error or an image of another schema
  version as a match, which only widens it. The filter itself still runs vectorised as a `FilterOp`.
  Before planning, the statement is walked once for what each binder reads (`Uses`): the columns
  named through it (`C`), and whether its rows reach a result, a whole-row operator or a subquery
  whole, which reads every column. A projection, aggregate or nest above a scan narrows it to the
  columns their expressions name.
- **DML** (§9). A statement reads everything first and writes last (§9.1). `Update`/`Delete` scan the
  target (logging `filter` when there is no `from`/`using`), join `from`/`using` by `filter` with
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
  The writes go to the store as one `write_rows` call, removals first (§9.4 rule 4).

Not yet: views read through the store, identity columns (`next_value`), unique constraints other than
the primary key as an upsert target, foreign keys, and translation of a row of an older schema version
(`ddl.md` §5.3).

## 5. What this note relies on in `query-tree.md`

§3.1–§3.3 for every comparison and for `Sort`, `Distinct` and the merge join's keys; §4.1–§4.2 for
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
no compress. Every kernel is a plain loop the compiler may auto-vectorise; these are the ones the
gaps hold back:

- **mask → bits**: building a validity bitmap from a kernel's per-row result (every comparison,
  `is_null`, `Field` of a nullable record); `Filter`'s row list.
- **compress**: `Filter` turning its predicate into the rows it keeps, and `take` after it.
- **gather**: `take` on every fixed-width lane (filtering, `Sort`'s permutation, `If`/`Match`
  reassembly), and `Col` of a union's shared column.

Arithmetic, comparison and logic on fixed-width lanes with no nulls are straight loops and
vectorise without any of the three.
