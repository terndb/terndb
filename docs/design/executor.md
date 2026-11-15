# The vectorised executor — design note

This note records how TernDB's executor is built. It is **design, not specification**: what a node
or an expression *means* is fixed by `docs/spec/query-tree.md`, and this note only says how the
executor computes it. Where it states a rule of meaning it cites the section it relies on; where
`query-tree.md` was silent about something the executor has to decide at run time, the rule was
written into `query-tree.md` itself, and this note points at it.

The code is `sh/sysl/terndb/exec_*.sysl`. This first slice covers what does not depend on storage:
column vectors, expression evaluation, and the operators `Values`, `ParamScan`, `Filter`,
`Project`, `Sort`, `Limit`, `Distinct`, `Aggregate` and `Tag`, a source over in-memory batches
standing in for `Scan`, and the conversion of an operator's output into result batches. Joins,
windows, `Nest`/`Unnest`, `Apply`, set operations, DML, storage, MVCC and the planner come later.

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
the type and the function per expression node) is noise. Every node exposes one binder in this
slice (§6, "Exposes"), so a batch is **one `ColumnVector` of the node's element type** `E`: a
record, a union (§6.15) or a scalar.

Operators are **pulled**: `trait Operator { next_batch(*self) -> Result[Option[ColumnVector],
CodecError] }`. `Some` is a batch of at least one row — an operator never hands up an empty batch,
which matches §6.4 rule 1 for the result — and `None` is the end. An operator holds its input as an
`&Operator`, so a tree is built from trait objects and dispatches once per batch. `Sort` and
`Distinct` drain their input on their first call; the rest stream.

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
not written yet.

## 5. What this note relies on in `query-tree.md`

§3.1–§3.3 for every comparison and for `Sort` and `Distinct`; §4.1–§4.2 for the coercions the
executor applies; §4.4 for arithmetic; §6.2–§6.4, §6.7, §6.9–§6.11 and §6.15 for the operators;
§7.1–§7.8 and §7.12 for the expressions; §10.1 for parameters. Rules written into it by this work: the
evaluation of `and`/`or` and of `coalesce`, the text functions' units and `like`'s escape, and the
calendar `add` (all in §7.12); a group's key, `filter`, `distinct`, and the values of `sum`, `min`,
`max` and `string_agg` (§6.7 rules 5–7).

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
