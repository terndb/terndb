# TernDB query tree

**Status:** normative, draft. **Tree schema version:** 1.0.

This document defines TernDB's native query input: the **typed query tree**. It fixes the type system,
every node kind, how names resolve, how parameters work, what validation reports, the canonical binary
encoding, how the schema evolves, and how Substrait plans map onto it. The wire protocol
(`wire-protocol.md`) and the C ABI (`libterndb`) carry trees in the encoding defined here.

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as in RFC 2119. Every MUST in this
document is meant to be checkable by a test that builds a tree, submits it and inspects the result,
the error code or the `EXPLAIN` output. Text marked *Rationale* explains a choice and is not itself a
rule. Text marked *Informative* has no force.

## 1. Model

1. A **statement** is a tree of nodes. There are three families of node: **relational nodes** (`Rel`),
   which denote relations; **expressions** (`Expr`), which denote values; and **statement roots**
   (`Query`, `Insert`, `Update`, `Delete`).
2. A value that varies between executions is a **parameter**: a typed hole, declared in the
   statement's envelope and bound separately at execution (§10). A constant of the statement itself
   (`0`, `LIMIT 3`, a status tag) MAY be a **literal** node (§7.3), which carries a scalar type and its
   value. A literal is part of the statement's shape (§10.2), so it shapes the plan; a parameter does not.
   *Rationale: injection is impossible by construction either way, because a tree is structured data,
   never text that is parsed; constants that steer the plan belong in the plan-cache key, and values that
   vary belong in parameters so that their statements share one plan.*
3. SQL (TernDB's own dialect) and GraphQL are **front ends**. Each produces a tree; neither has
   semantics of its own beyond the tree it produces. A server MUST validate a tree it receives exactly
   as it validates the tree its own SQL front end produces: the same checks, the same error codes.
4. The meaning of a tree MUST NOT depend on session state (time zone, search path, locale, date
   style). Names in a tree are fully qualified (§8.1) and every zone-dependent operation takes its zone
   as an argument. The only exceptions are the statement-time functions (`statement_timestamp()` and
   kin), which read the clock, and the snapshot the statement runs against.
   *Rationale: one tree, one meaning, which is what makes shape-keyed plan caching sound.*
5. **DDL** is specified in `ddl.md`, which defines statement kinds 32–63 (§12.5) and the named type
   reference, type kind 32 (§12.3), in this document's framing and encoding. A DDL statement is a
   statement root like any other and is validated, encoded and versioned by the rules here. A root kind
   in 32–63 that `ddl.md` leaves unassigned is refused with `Q1002`.

## 2. Types

### 2.1 Scalar types

| Type | Values | Justification |
|---|---|---|
| `bool` | `false`, `true` | Predicates. |
| `int8`, `int16`, `int32`, `int64`, `int128` | two's-complement integers of that width | 16/32/64 match SQL's `smallint`/`integer`/`bigint`; `int8` matches Substrait's `i8` and every client language's byte; `int128` holds exact sums of `int64` without falling to decimal. |
| `uint8`, `uint16`, `uint32`, `uint64` | unsigned integers of that width | Counters, hashes, sizes and C interop, without a sign bit's halving of the range. |
| `decimal(p, s)` | exact numbers with `p` significant digits, `s` of them after the point; `1 ≤ p ≤ 38`, `0 ≤ s ≤ p` | Money and exact arithmetic. 38 digits fit an `int128` unscaled value, so decimals are fixed-width and vectorisable. |
| `float32`, `float64` | IEEE 754 binary32/binary64, including ±0, ±∞ and NaN | Measurements, scores, vectors' components. |
| `text` | any sequence of Unicode scalar values, encoded UTF-8; NUL is allowed | Strings. Length-prefixed everywhere, so NUL needs no exclusion. |
| `bytes` | any octet sequence | Opaque data. |
| `date` | a proleptic Gregorian calendar day, years −4713 … 294276 | Calendar dates. |
| `time` | a time of day with microsecond precision, `00:00:00` … `23:59:59.999999` | Wall-clock time without a date. |
| `datetime` | a civil date and time with microsecond precision and **no** zone | A local reading ("the meeting is at 09:00"). |
| `timestamp` | an instant: microseconds since 1970-01-01T00:00:00Z | A point on the global timeline. |
| `interval` | a triple (`months`: int32, `days`: int32, `micros`: int64) | Calendar arithmetic needs months and days kept apart from elapsed time. |
| `uuid` | 128 bits | Identifiers. |
| `json` | a JSON text (RFC 8259) in canonical form (§3.5) | Interop with schemaless documents. Nesting in TernDB's own data stays relational (§2.5); `json` is never how a nested result is encoded. |

1. There is no `char(n)`/`varchar(n)`, no `timetz`, no `money` and no arbitrary-precision `numeric`
   in version 1.0; `decimal` takes at most 38 digits. A length or range limit is a constraint, not a
   type.
   *Rationale: Postgres's `timestamp`/`timestamptz` split is the source of its most common time bug;
   TernDB names the two concepts `datetime` and `timestamp` and never converts between them
   implicitly. `timetz` has no coherent meaning.*
2. Microsecond precision gives `timestamp` a range of about ±292,000 years in an `int64`. Nanosecond
   precision would cut that range to 1677–2262.

### 2.2 Nullability

1. Nullability is part of the type. `T` does not admit null; `T?` admits every value of `T` and null.
   `T??` does not exist: making a nullable type nullable yields the same type.
2. Any type may be nullable, including a record, a union, a relation type and a relation's element
   type (§2.5): in `list<{a: int64}?>` each element is a record or null.
3. A table column declared `NOT NULL` has type `T`; otherwise `T?`.

### 2.3 Records

1. `{n₁: T₁, …, nₖ: Tₖ}` is an ordered sequence of named fields. Names are non-empty, unique within the
   record (`Q3015`), and compared byte-for-byte (case-sensitive). `k = 0` is allowed: `{}` is the type
   with exactly one value.
2. Record types are **structural**: two record types are identical iff they have the same names, in
   the same order, with identical field types.

### 2.4 Tagged unions

1. `union<t₁: R₁, …, tₖ: Rₖ>` holds exactly one variant, identified by its **tag**. `1 ≤ k ≤ 65,536`
   (`Q3016`; the bound is what a 2-octet discriminant holds, `wire-protocol.md` §6.2). Tags are
   unique (`Q3015`). Each payload `Rᵢ` MUST be a record type (`Q3014`); a variant with no data has
   payload `{}`, so an enum is a union whose payloads are all `{}`. There is no separate enum type; a
   catalog may give an enum, like any union or record, a name (`ddl.md` §2.8).
2. The **discriminant** of a variant is its tag's ordinal in the declaration (0-based). The encoding
   carries the discriminant; the tag name appears only in the type.
3. Union types are structural: identical iff the same tags in the same order with identical payloads.
4. The **shared columns** of a union are the fields that appear in every payload with the same name
   and the identical type. They are ordered as in the first payload. (They are how GraphQL interface
   fields are read without a `Match`.)

### 2.5 Relation types

1. A relation type is `bag<E>` (a multiset: no order, duplicates counted) or `list<E>` (a sequence:
   order is part of the value).
2. The element type `E` MUST be a record type, a union whose payloads are records, or a scalar
   (§2.1), and MAY be nullable (§2.2); it MUST NOT itself be a relation type (`Q3014`). A null element
   is one element whose value is null: it counts in the relation's length, and the wire marks it in the
   element column's validity (`wire-protocol.md` §6.2). Relation types may nest to any depth through
   record fields: `bag<{id: int64, posts: list<{id: int64}>}>` is legal.
   A relation whose element is a scalar, such as `list<int64>`, is a **scalar relation**: under its
   binder it exposes one column, `value`, of the element type, and `Row(binder)` is that value.
3. Relation-valued columns are values like any other: they can be stored in a table column, passed as
   parameters, compared (§3), hashed, grouped on and indexed.
4. A relation value is never null unless its type is `bag<E>?`/`list<E>?`; the absence of rows is the
   empty relation, which is distinct from null.

### 2.6 Type identity

Two types are **identical** iff they are the same scalar (with the same `p, s` for decimal), or
structurally identical per §2.3–2.5, with identical nullability at every level.

## 3. Value semantics: equality, ordering, hashing

TernDB has two notions of equality. Both are defined over every type.

### 3.1 Identity equality `same`

`same(a, b)` is total: it always returns `true` or `false`, never null. It is the equality used by
grouping, `DISTINCT`, set operations, `Nest`, join keys produced by the decorrelator, unique
constraints and hash indexes.

1. `same(null, null)` is `true`; `same(null, x)` for non-null `x` is `false`.
2. Integers and decimals compare by numeric value after coercion to a common type (§4.2).
3. Floats: `same(NaN, NaN)` is `true` for every NaN payload; `same(-0.0, +0.0)` is `true`; otherwise
   IEEE equality.
4. `text`, `bytes` and `json` (in canonical form, §3.5) compare as octet sequences. No collation
   applies in version 1.0: `text` orders by binary UTF-8. *Reserved hook:* collations, when added, are a
   property of the **comparison**, never of the type: a later minor (§13.2) appends an optional
   `collation` field to `SortKey` and to the comparison calls, whose absence means binary order, and an
   index on a collated key stores ICU sort keys (`on-disk-format.md` §4.6).
5. `interval` compares **componentwise**: `same('1 month', '30 days')` is `false`.
   *Rationale: Postgres's normalisation makes equal intervals produce different dates.*
6. Records: fieldwise `same`, all fields.
7. Unions: same discriminant and `same` payloads.
8. `list<E>`: same length and `same` element by element.
9. `bag<E>`: equal as multisets under `same` — there is a bijection pairing each element of one with a
   `same` element of the other.

### 3.2 SQL equality `eq` and comparisons

`eq`, `ne`, `lt`, `le`, `gt`, `ge` follow three-valued logic: if either operand is null the result is
null.

1. On scalars `eq` agrees with `same` for non-null operands (so `eq(NaN, NaN)` is `true`).
   *Rationale: one equality on non-null values keeps indexes and predicates consistent.*
2. On records, `eq` is SQL row comparison: `false` if any field pair is `eq`-false; else null if any
   pair is null; else `true`. `lt` etc. compare lexicographically by field: the field pairs are taken
   in order, the first pair that is not equal decides, and a pair that is null before any pair has
   decided makes the comparison null (`(1, null) < (2, 0)` is `true`, `(1, null) < (1, 3)` is null);
   when every pair is equal the operands are equal. A nested record field follows the same rules.
3. On unions, `eq` is `false` if the discriminants differ, otherwise `eq` of the payloads.
4. `eq`/`ne` on a relation type, and `lt`/`le`/`gt`/`ge` on a relation type, a union type,
   `interval` or `json`, MUST be refused with `Q3006` (relations) or `Q3007` (unions, intervals, `json`).
   The refusal is by type, whatever the operands' values: it applies equally to a record or union
   that holds such a type in a field or payload at any depth, and a relation is `Q3006` for every
   operator.
   *Rationale: three-valued multiset equality has no useful definition; use `same`. Intervals and
   unions have no natural order; `Sort` still orders them through `cmp`.*

### 3.3 Total order `cmp`

`cmp(a, b)` returns −1, 0 or +1 and is a total order consistent with `same` (`cmp(a,b) = 0` iff
`same(a,b)`). It is used by `Sort`, ordered indexes and the bag comparison below.

1. Null sorts below every non-null value. (`Sort` keys override this at the top level, §6.9.)
2. `false < true`. Integers and decimals by numeric value.
3. Floats: −∞ < negative < −0 = +0 < positive < +∞ < NaN.
4. `text`, `bytes` and `json` (canonical form): unsigned octet-lexicographic (for valid UTF-8 this is
   code point order); a proper prefix sorts first.
5. `date`, `time`, `datetime`, `timestamp`: chronologically. `uuid`: as a 128-bit big-endian unsigned
   number. `interval`: lexicographically on (`months`, `days`, `micros`).
6. Records: lexicographic by field. Unions: by discriminant, then payload.
7. `list<E>`: lexicographic by element; a proper prefix sorts first.
8. `bag<E>`: compare the two bags' elements each sorted ascending by `cmp`, as lists.

### 3.4 Hashing

1. Any hash a server uses for grouping, joining or indexing MUST satisfy `same(a,b) ⇒ hash(a) =
   hash(b)`. In particular every NaN hashes alike, −0.0 and +0.0 hash alike, numerically equal
   decimals of different scales hash alike, and a bag's hash is independent of element order.
2. The hash function itself is not part of this specification. Where a hash is persisted, it is fixed
   by `on-disk-format.md`.

### 3.5 Values: canonical text forms

The text form of each scalar is what `Cast` to `text` prints and what `Cast` from `text` parses (§7.6).
Printing MUST produce exactly the form below; parsing MUST accept it and the stated variants, and
MUST raise `R0006` (or yield null under `try`) on anything else, including surrounding whitespace.

| Type | Printed form | Also parsed |
|---|---|---|
| `bool` | `true`, `false` | — |
| integers | `-`? then decimal digits, no leading zeros (`0`, `-42`); unsigned types have no sign | a leading `+` |
| `decimal(p, s)` | `-`? digits, then `.` and exactly `s` digits when `s > 0` (`12.50` for `decimal(4, 2)`) | fewer or more fraction digits, rounded half away from zero to `s` |
| floats | the shortest decimal that reads back to the same value, `e` exponent when it is outside 10⁻⁶ … 10²¹; `-0`, `inf`, `-inf`, `nan` | any decimal or exponent form; `Infinity`, `-Infinity`, `NaN` |
| `text` | the value itself | — |
| `bytes` | `\x` then two lowercase hex digits per octet | uppercase hex digits |
| `date` | ISO 8601 `YYYY-MM-DD`, astronomical year numbering; a year outside 0000–9999 has a sign and at least five digits (`-04713-11-24`) | — |
| `time` | `HH:MM:SS`, then `.` and 1–6 digits when the fraction is non-zero, trailing zeros removed | 0–6 fraction digits |
| `datetime` | the `date` form, `T`, the `time` form; no zone | a space in place of `T` |
| `timestamp` | the `datetime` form of the instant in UTC, then `Z` | an offset `±HH:MM` in place of `Z`, converted to UTC; no zone at all is refused |
| `interval` | `P`, then `<n>M`, `<n>D` and `T<s>S` for each non-zero component, `n` a signed integer and `s` signed seconds with the `time` fraction rule; `PT0S` for zero (`P14M-3DT90.5S`) | — |
| `uuid` | RFC 9562 `8-4-4-4-12` lowercase hex | uppercase hex |
| `json` | its canonical form: the RFC 8785 serialisation of the parsed text | any RFC 8259 JSON text, canonicalised on parse |

A `json` value is always held in canonical form, so `same` on `json` is octet equality of canonical
forms. A `json` literal or parameter value that is not canonical is `Q1007`. Records, unions and
relations have no text form in version 1.0.

The table is made exact by these rules:

1. **Integers.** A signed type also parses `-0` as zero; an unsigned type refuses `-` in any form. A
   text outside the type's range is `R0006`.
2. **Decimals.** The whole part has no redundant leading zero (`0.5`, never `00.5` or `.5`); a `.`
   is followed by at least one digit, and no `.` at all is a text with no fraction digits (`12` is
   `12.00` in `decimal(4, 2)`). A leading `+` is accepted, as for integers. A value that needs more
   than `p` digits after rounding is `R0006`.
3. **Floats.** Plain notation is used from 10⁻⁶ up to but not including 10²¹, as ECMAScript's
   `Number.prototype.toString` does; outside it the form is the first digit, `.` and the rest when
   there are more, then `e` and the exponent, with `-` for a negative exponent and no sign otherwise
   (`1e21`, `1.5e-7`). Parsing accepts `[+-]? digits ("." digits)? ([eE] [+-]? digits)?`, leading
   zeros allowed, and exactly the six names `inf`, `-inf`, `nan`, `Infinity`, `-Infinity`, `NaN`. A
   finite text whose value rounds past the type's largest finite value is `R0006`, not an infinity.
4. **Dates.** The parsable range is `-04713-01-01` through `+294276-12-31`. A `datetime` or
   `timestamp` must also fit the `int64` of microseconds it is held in, so the latest `timestamp` is
   `+294247-01-10T04:00:54.775807Z`. A date or interval text is accepted only where it is exactly the
   printed form of the value it reads as: no redundant sign or zero in the year, no zero component,
   no day past its month's end.
5. **Offsets.** A `timestamp` offset is `+` or `-`, then hours `00`–`23`, `:` and minutes `00`–`59`.
6. **`json`.** The canonical form is RFC 8785's, so a positive number exponent is written `e+`
   (`1e+21`) and `-0` is `0`. A text RFC 8785 cannot canonicalise is refused: a name twice in one
   object, a number outside binary64's finite range, an unpaired surrogate escape, and arrays and
   objects nested more than 256 deep.

## 4. Inference and coercion

The tree is explicitly typed at its leaves: every column has a catalog type, every parameter a
declared type, every `Null` a type. Types of all other expressions are inferred bottom-up. There is no
inference of a parameter's type from its context: the front end that builds the tree decides it.

### 4.1 Implicit coercions

Only these coercions are applied implicitly, and only where an expected type is known (a function
argument, a `SetOp` column, an insert or update target, an `If` arm, a `Values` column):

1. **Integer widening**: `int8 → int16 → int32 → int64 → int128`; `uint8 → uint16 → uint32 →
   uint64`; and `uintN → int(2N)` (`uint8 → int16`, …, `uint64 → int128`), each lossless. A signed
   type never coerces to an unsigned one.
2. **Integer to decimal**: `intN → decimal(p, 0)` with `p` = 3, 5, 10, 19, 38 for `int8`, `int16`,
   `int32`, `int64`, `int128`, and `p` = 3, 5, 10, 20 for `uint8`, `uint16`, `uint32`, `uint64`; and
   onward by decimal widening.
3. **Decimal widening**: `decimal(p, s) → decimal(p', s')` when `s' ≥ s` and `p' − s' ≥ p − s`.
4. **Float widening**: `float32 → float64`.
5. **Null widening**: `T → T?`, at any depth (a record field, a relation's element).
6. **Order forgetting**: `list<E> → bag<E>`.
7. **Structural**: a record, union or relation coerces to another of the same shape (names, tags,
   order) when each component coerces.

Everything else MUST be refused with `Q3001`, in particular: integer or decimal to float and back,
any type to or from `text` or `json`, `bool` to or from integer, `datetime` to or from `timestamp`, `date` to
`datetime`, `T?` to `T`, and `bag<E>` to `list<E>`. These are written as an explicit `Cast` (§7.6),
an `assert_not_null` call, or a `Sort`.
*Rationale: every implicit conversion Postgres performs has a known silent-wrong-answer case; the
ones kept are lossless.*

### 4.2 Common type

Where two or more types must unify (`SetOp` columns, `If` arms, `coalesce`, `Values` rows, binary
arithmetic and comparison operands), the **common type** is the least type every input coerces to by
§4.1. If none exists, the node is refused with `Q3001` (or `Q3010` for `SetOp`).

### 4.3 Function and operator resolution

Operators are functions with stable names (§7.12). A call `f(a₁…aₙ)` resolves as follows:

1. If one overload of `f` accepts the argument types exactly, it is chosen.
2. Otherwise, the overloads reachable by implicit coercions are collected; if exactly one needs the
   fewest coercion steps, it is chosen. If several tie, `Q3003`. If none, `Q3002`. A call's steps are
   the sum over its arguments; each arrow of a chain in §4.1 is one step (`int8 → int32` is two,
   `uint8 → int16` one, `int32 → decimal(12, 2)` two: to `decimal(10, 0)`, then widening), `T → T?`
   is one, `list → bag` is one, and a record, union or relation takes the sum of its components'.
3. A function is **strict** unless the catalog says otherwise: its result is `R?` if any argument is
   nullable, and null when any argument is null. A strict overload's parameters are non-null types,
   and an argument is matched against one by its non-null base type.

### 4.4 Arithmetic result types

1. Integer `add`, `sub`, `mul`, `div`, `mod`, `neg`: operands widen to their common type (§4.2; a
   signed and an unsigned operand meet at the signed type that holds both); the result has that type.
   `neg` of an unsigned value is refused (`Q3002`). Overflow is the runtime error `R0001`; integers never wrap. `div` truncates toward zero;
   `mod`'s sign follows the dividend; division by zero is `R0002`.
2. Decimal `add`/`sub`: `s = max(s₁, s₂)`, `p = min(38, max(p₁ − s₁, p₂ − s₂) + s + 1)`. `mul`:
   `s = s₁ + s₂`, `p = min(38, p₁ + p₂)`; if `s > 38` the call is refused with `Q3011`. `div`:
   `s = max(6, s₁, s₂)`, `p = 38`, rounded half away from zero. `mod`, whose result is smaller than
   its divisor: `s = max(s₁, s₂)`, `p = min(38, p₂ − s₂ + s)`. An integer operand of a decimal one is
   its `decimal(p, 0)` of §4.1 rule 2. A result outside the result type's range is `R0001`.
3. Float arithmetic is IEEE 754 in the operands' common type; it never raises errors.
4. Mixing float with integer or decimal operands is refused (`Q3002`).

## 5. Statements

A tree on the wire or across the C ABI is an **envelope**:

```
Envelope  = { version: (major, minor), params: [ParamDecl], statement: Statement, spans: Spans? }
ParamDecl = { name: text?, type: Type }
Statement = Query | Insert | Update | Delete | DDL statement (ddl.md §3)
```

1. `Query { rel: Rel }` returns `rel`'s value. Its result type is `rel`'s relation type.
2. `Insert`, `Update`, `Delete` are defined in §9. With a `returning` list they return a `bag` of
   records. Without one they return **no value**: the statement has no result type. Every statement
   also reports its **row count** as an outcome of execution, not as a value: for a `Query` or a DML
   statement with `returning`, the number of top-level rows of the result; for a DML statement without
   `returning`, the number of rows it inserted, updated or deleted (skipped upsert rows are not
   counted, §9.3); for a DDL statement, 0. The count is unsigned. The wire carries it in `Complete`
   (`wire-protocol.md` §4.3) and the C ABI in `terndb_result_counts`, one count per parameter tuple.
   *Rationale: every value a statement returns is then a relation, decoded by the one result decoder;
   a count is a fact about an execution, like its commit sequence number, and a batch has one per
   tuple, which a single `int64` value could not express.*
3. The **result type** of a statement is determined by validation alone, and so is whether it has
   one. A server MUST be able to return it without executing the statement (the wire spec's describe
   step).
4. `spans`, when present, maps nodes (by pre-order index) to byte ranges in a front end's source text.
   It is metadata: it changes neither meaning nor shape, and it lets errors point at source.
5. The output columns of the root `Rel`, of every `Relation`/`Scalar` subquery and of every `SetOp`
   input and `Insert` source MUST have unique names (`Q2005`).

## 6. Relational nodes

Every `Rel` has an **output type**: `bag<E>` or `list<E>`, where `E` is a record of its output
columns (or a union, §6.15). The column of the output, their names and their types are fixed by the
rules below. A node that exposes one binder has that binder's element type as `E` (a record, a union,
or the scalar of a scalar relation); a node that exposes several has the record of all their columns
in the order of §8.4. "Exposes" refers to the binders a node makes visible to the expressions of its parent
(§8.2).

| Node | Fields | Output kind | Exposes |
|---|---|---|---|
| `Scan` | `table: QualName, as: binder` | bag | `as` |
| `Values` | `as, columns: [(name, Type)], rows: [[Expr]]` | list | `as` |
| `ParamScan` | `as, param: index` | the parameter's kind | `as` |
| `Filter` | `input, predicate: Expr` | input's | input's |
| `Project` | `input, as, items: [(name, Expr)]` | input's | `as` |
| `Join` | `kind, left, right, on: Expr?, mark: name?, as: binder?` | bag | see §6.5 |
| `Apply` | `kind, left, right, on: Expr?, mark: name?, as: binder?` | bag | see §6.6 |
| `Aggregate` | `input, as, keys: [(name, Expr)], aggs: [(name, AggCall)]` | bag | `as` |
| `Window` | `input, as, items: [(name, WinCall)]` | input's | input's + `as` |
| `Sort` | `input, keys: [SortKey]` | list | input's |
| `Limit` | `input, count: Expr?, offset: Expr?, ties: bool, arbitrary: bool` | input's | input's |
| `Distinct` | `input` | bag | input's |
| `SetOp` | `op, all: bool, as, inputs: [Rel]` | bag | `as` |
| `With` | `recursive: bool, ctes: [CteDef], body: Rel` | body's | body's |
| `CteRef` | `name, as` | the CTE's | `as` |
| `Nest` | `input, as, keys: [(name, Expr)], name, items: [(name, Expr)], order: [SortKey]` | bag | `as` |
| `Unnest` | `input, as, value: Expr, outer: bool, ordinality: name?` | bag | input's + `as` |
| `Tag` | `input, as, type: Type, tag` | input's | `as` |

### 6.1 Scan

Reads every row of a table or view visible in the statement's snapshot. Output columns are the
table's columns in catalog order, with catalog types. An unknown table is `Q2001`. A `Scan` of a view
reads the view's stored query in its place (`ddl.md` §2.9).

### 6.2 Values

1. Each row MUST have one expression per declared column (`Q5009`), each coercing to its column's type
   (`Q3001`). Zero rows is allowed.
2. The output is a `list` in row order. Row expressions MAY reference parameters and outer binders
   (§8.3) but no binder of a sibling.

### 6.3 ParamScan

Reads a relation-valued parameter as a source. The parameter MUST have a relation type (`Q3001`), and
not a nullable one (`Q3005`); its element type's fields are the output columns. This is the bulk-load path: one execution, any number of
rows (§10.4).

### 6.4 Filter and Project

1. `Filter` keeps the input rows whose `predicate` is `true`; null and `false` drop the row. The
   predicate MUST have type `bool` or `bool?` (`Q3004`). A list input stays a list in the same order.
2. `Project` evaluates each item once per input row. `items` MUST be non-empty (`Q5009`) with unique
   names (`Q2005`). Items MAY be relation-valued (`Relation`, §7.9); this is how nested results are
   written. A list input stays a list in the same order.

### 6.5 Join

`kind` is one of `inner`, `left`, `right`, `full`, `semi`, `anti`, `mark`, `single`.

1. `on` is evaluated with both sides' binders visible; absent means `true` (a cross join when
   `kind = inner`). It MUST be `bool`/`bool?` (`Q3004`).
2. The right input MUST NOT reference a binder exposed by the left input (`Q5006`); that is an `Apply`.
3. `inner`, `left`, `right`, `full` expose both inputs' binders. `left` makes every right-side column
   nullable, `right` every left-side column, `full` both.
4. `semi` keeps left rows with at least one right row for which `on` is `true`; `anti` keeps left rows
   with none. Both expose only the left binders.
5. `mark` keeps every left row and adds column `mark` (required, `Q5009` if absent) under binder `as`
   (required): `true` if some right row makes `on` `true`; else null if some makes it null; else `false`.
   Its type is `bool?` when `on` is nullable, else `bool`. This is SQL `IN` semantics.
6. `single` is a left join that MUST raise `R0003` if a left row matches more than one right row. It is
   the decorrelated form of a scalar subquery.
7. The output is a bag. *Rationale: preserving order across joins forbids hash joins; order is
   restored with `Sort`.*

### 6.6 Apply (correlated source form)

`Apply` has `Join`'s fields; its `kind` is one of `cross`, `outer`, `semi`, `anti`, `mark`, `single`, but
its right input MAY reference the left input's binders: the right side is evaluated once per left row,
conceptually. Output columns follow §6.5 with `cross` as `inner` and `outer` as `left`.

1. `Apply` is the form every correlated subquery denotes: `Relation`, `Scalar`, `Exists` and `In`
   expressions (§7.9–7.11) are equivalent to an `Apply` of kind `outer`-with-collect, `single`, `mark`
   and `mark` respectively.
2. A conforming planner MUST remove every `Apply`, at every nesting depth, by general unnesting
   (Neumann–Kemper, "Unnesting Arbitrary Queries", BTW 2015) before execution. The executed plan
   MUST contain no operator that re-evaluates a subtree once per outer row, and `EXPLAIN` of the
   physical plan MUST show no `Apply`. A per-level `Limit`, `Sort` or `Aggregate` inside the right side
   keeps its per-outer-row meaning through unnesting (it becomes a partitioned window or a grouped
   aggregate on the outer key).
3. A client MAY send `Apply` directly. Its meaning is the per-row one, whatever the plan.

### 6.7 Aggregate

1. Groups the input by `keys` under `same` and emits one row per group: the keys, then the aggregates,
   under binder `as`. With no keys, it emits **exactly one row**, also for empty input.
2. `AggCall = { function, args: [Expr], distinct: bool, filter: Expr?, order: [SortKey] }`. `filter`
   restricts the rows the call sees. `order` is honoured by order-sensitive aggregates (`collect`,
   `string_agg`) and refused on the others (`Q3002`).
3. Aggregate calls MUST appear only in `Aggregate.aggs` and `Window.items` (`Q3013`). `HAVING` is a
   `Filter` above the `Aggregate`.
4. Core aggregates: `count()` (rows) and `count(x)` (non-null `x`) → `int64`; `sum` → `int128` for
   integer inputs, `decimal(38, s)` for `decimal(p, s)`, `float64` for floats; `min`, `max` (any type
   with `cmp`); `bool_and`, `bool_or`; `collect(x)` where `x` is a record, union or scalar (§2.5) → `bag<X>`, or
   `list<X>` when `order` is non-empty. Over an empty set every core aggregate except `count` and
   `collect` (empty relation) yields null, so its result type is nullable.

### 6.8 Window

`WinCall = { function, args, partition: [Expr], order: [SortKey], frame: Frame? }`. Each item adds one
column under binder `as`; the input's rows, binders, kind and order are unchanged. Ranking functions
(`row_number`, `rank`, `dense_rank`) take no arguments → `int64`; `lag(x)`, `lag(x, n)` and `lead`
likewise → `X?`, `lag(x, n, d)` → the common type of `x` and `d`, with `n` coercing to `int64`;
`first_value(x)`, `last_value(x)` → `X?`; and every core aggregate, typed as in §6.7. Any other
function there is `Q3002`, or `Q2007` when no catalog has it. `Frame = { unit: rows | range | groups, start: Bound, end: Bound, exclude: none |
current | group | ties }`; `Bound = { kind: unbounded_preceding | preceding | current |
following | unbounded_following, offset: Expr? }`. An invalid frame (start after end, in the order
the kinds are listed; a start of `unbounded_following` or an end of `unbounded_preceding`; an offset
absent from a `preceding` or `following` bound or present on another; `range` with an offset and not
exactly one order key) is `Q5008`. A `rows` or `groups` offset is `int64`; a `range` offset coerces
to a numeric key's type, or is an `interval` for a `date`, `time`, `datetime` or `timestamp` key
(`Q3001`).

### 6.9 Sort

`SortKey = { expr: Expr, desc: bool, nulls_first: bool }`. Both flags are always present in the tree;
front ends supply the defaults. The output is a `list` ordered by the keys using `cmp` (§3.3), with
null placed per `nulls_first` for each key's top-level value. The sort is **stable**: when the input is
a list, rows equal on every key keep their input order. A key MAY be of any type, including unions and
relations.

### 6.10 Limit

1. `count` and `offset` MUST be `int64` (`Q3001`; no coercion, and `int64?` is `Q3005`) and MUST NOT reference the input's binders; they MAY
   reference parameters and outer binders. A negative value is `R0006`. Absent `count` means no limit;
   absent `offset` means 0.
2. The input MUST be a list (`Q3008`) unless `arbitrary` is `true`, in which case any `count` rows of a
   bag are chosen. *Rationale: an unordered `LIMIT` is a nondeterminism bug far more often than a
   wish; the flag makes the wish explicit.* The GraphQL front end never sets `arbitrary`: a `first:`
   without `orderBy` sorts by the table's primary key, so its pages are deterministic.
3. `ties = true` also keeps every row equal on all sort keys to the last kept row; it requires the input
   to be a `Sort` node directly (`Q3008`).

### 6.11 Distinct

Removes duplicates under `same`. The output is a bag with the input's binders.

### 6.12 SetOp

`op` is `union`, `intersect` or `except`; `all` selects multiset (`true`) or set (`false`) semantics.
`inputs` holds at least two relations (`Q5009`), each with the same number of columns (`Q3010`);
columns unify positionally to their common type (§4.2, `Q3010`); output names are the first input's.
Duplicates are judged by `same`. The output is a bag; `union all` of lists is a bag.

### 6.13 With and CteRef

1. `CteDef = { name, columns: [(name, Type)]?, rel: Rel }`. Names are unique in one `With` (`Q2004`).
   A `CteRef` names a CTE of an enclosing `With` (`Q2008`); its output is the CTE's, under its own
   binder. In a non-recursive `With`, a CTE is in scope in the CTEs after it and in the body, not in its
   own `rel`; in a recursive one, every CTE of the `With` is in scope in every `rel` and the body. The
   nearest enclosing `With` that defines a name is the one a reference names.
2. Every `CteRef` of a non-recursive CTE observes one and the same value within a statement. The
   planner MAY inline a CTE whose body is free of volatile functions.
3. With `recursive = true`, each CTE MUST declare `columns` (`Q5001`) and its `rel` MUST be a `SetOp`
   with `op = union` whose first input does not reference the CTE (the seed) and whose remaining inputs
   each reference it exactly once (linear recursion), never inside an `Aggregate`, `Distinct`, the
   null-supplying side of an outer join, an `anti` join's right input, or a `Relation` subquery
   (`Q5001`). Mutual recursion is refused (`Q5001`).
4. Evaluation iterates: the working set is the seed; each step evaluates the recursive inputs against
   the previous step's new rows; with `all = false`, rows `same` as any earlier row are discarded. It
   ends when a step yields no new rows. A server MAY bound the number of steps; exceeding the bound is
   `R0005`.

### 6.14 Nest and Unnest

1. `Nest` groups its input by `keys` under `same` and emits, per group, the keys followed by one
   column `name` holding the group's rows as `bag<{items}>` (or `list<{items}>`, sorted by `order`, when
   `order` is non-empty). It MUST equal `Aggregate` with the same keys and the single aggregate
   `collect(Record(items))` with that `order`.
2. `Unnest` evaluates `value`, a relation-typed expression that MAY reference the input's binders, per
   input row, and emits one row per element: the input's columns followed by the element's columns
   (for a scalar relation, the one column `value`, §2.5) under binder `as`. With `outer = true`, an input row whose relation is empty or null emits one row
   with every element column null (so they are nullable). `ordinality` adds an `int64` column, 1-based,
   under `as`; it requires `value` to be a list (`Q3008`). With `outer = true` the ordinality column is
   nullable too, null in the padded row; with an ordinality, `Row(as)` is the record of every column
   under `as`, the element's then the ordinality.

### 6.15 Tag and union-element relations

1. `Tag` turns a relation of records into a relation of union values: each row becomes variant `tag` of
   `type`, which MUST be a union (`Q3001`) with a variant `tag` (`Q2010`) whose payload the input's
   element type coerces to (`Q3001`).
2. A relation whose element type is a union exposes, under its binder, the union's **shared columns**
   (§2.4) as ordinary columns, and the whole element through `Row(binder)` (§7.2). Variant-specific
   fields are read with `Match` (§7.7).
3. `SetOp` inputs of union element type MUST have identical union types (`Q3010`).

## 7. Expressions

| Expr | Fields | Type |
|---|---|---|
| `Col` | `binder, name` | the column's |
| `Row` | `binder` | the binder's element type |
| `Param` | `index` | the declared type |
| `Null` | `type` | `type?` |
| `Literal` | `type, value` | `type` |
| `Call` | `function, args: [Expr]` | per §4.3 |
| `Cast` | `expr, type, mode: strict \| try` | `type` (`type?` for `try`) |
| `If` | `arms: [(cond, value)], else: Expr?` | common type of values |
| `Match` | `expr, arms: [(tag, binder, Expr)], else: Expr?` | common type of arms |
| `Record` | `fields: [(name, Expr)]` | record |
| `Field` | `expr, name` | the field's |
| `Variant` | `type, tag, expr` | `type` |
| `Relation` | `rel` | `rel`'s relation type |
| `Scalar` | `rel` | the single column's |
| `Exists` | `rel` | `bool` |
| `In` | `exprs: [Expr], rel` | `bool` or `bool?` |
| `Action` | — | the DML action union (§9.5) |

### 7.1 Col

Names a column of a binder visible at this point (§8). Its type is the column's as seen there,
including nullability added by an outer join between the binder and this point.

### 7.2 Row

The whole current row of `binder` as a record (or, for a union-element relation, the union value).

### 7.3 Param, Null and Literal

`Param` refers to the envelope's parameter `index` (0-based; `Q4001`). `Null(type)` is the null value
of `type?`. `Literal(type, value)` is a constant: `type` MUST be a non-nullable scalar (§2.1; `Q3017`),
and `value` is that type's §12.6 encoding (a `json` value in canonical form, `Q1007`). A literal is
kept in the shape form (§10.2); a front end puts a value in a literal only when it is a constant of
the statement, and in a parameter when it varies between executions.

### 7.4 Call

Applies a catalog function (§7.12). Unknown function: `Q2007`.

### 7.5 If

Evaluates conditions in order; the first `true` selects its value. No arm `true` and no `else` yields
null (so the type is nullable). Conditions MUST be `bool`/`bool?` (`Q3004`). Unselected arms MUST NOT
be evaluated, so their runtime errors do not occur.

### 7.6 Cast

`strict` raises `R0006` on a value that does not convert; `try` yields null. Permitted casts: between
any two numeric types (range-checked; rounding half away from zero into decimal and integer targets);
between `text` and every scalar (parsing and printing the forms of §3.5); `bytes` ↔ `uuid` (16 octets); records, unions and relations
componentwise where shapes match; `list<E> → bag<E>`. Anything else, including `bag → list`,
`timestamp ↔ datetime` and `T? → T`, is `Q3012`. Nullability is judged at every level: a nullable
value (or component) casts only to a nullable type, except that a `try` cast's target is `type?`
whatever `type` says, so `try` takes a nullable value to any permitted base. The zone conversions are functions
(`at_zone(timestamp, text) → datetime`, `from_zone(datetime, text) → timestamp`); a non-null value is
obtained from a nullable one with `assert_not_null(x) → T` (`R0004` on null).

### 7.7 Match

1. `expr` MUST have a union type (`Q3001`). Each arm names a tag (`Q2010`; a tag twice is `Q3009`) and a
   binder through which the payload's fields are read as `Col(binder, field)`.
2. Arms MUST cover every tag unless `else` is present (`Q3009`, naming the missing tags). *Rationale:
   adding a variant to a stored union then fails validation of every query that would silently
   mishandle it.*
3. A null `expr` yields null.

### 7.8 Record, Field, Variant

`Record` builds a record (unique names, `Q2005`). `Field` reads a field of a record-typed expression
(`Q2011`); null record yields null. `Variant` builds variant `tag` of union `type` from a record
expression.

### 7.9 Relation

The value of `rel`, evaluated with the enclosing row's binders visible (§8.3). It is the only way a
nested result is written: TernDB never encodes a nested relation as JSON. A per-level `Sort`, `Limit`
or `Aggregate` inside `rel` applies per enclosing row.

### 7.10 Scalar and Exists

1. `Scalar` requires `rel` to have exactly one column (`Q5009`). More than one row is `R0003`. Zero rows
   yields null, so the type is `T?` — **unless** `rel` is cardinality-one: an `Aggregate` with no keys,
   or a `Project` or `Window` whose input is cardinality-one. Then the type is the column's own.
2. `Exists` is `true` iff `rel` has at least one row.

### 7.11 In

`In(exprs, rel)` requires `rel` to have one column per expression, pairwise comparable by `eq`
(`Q3001`). It is `true` if some row is `eq`-true to the tuple; else null if some comparison is null;
else `false` (SQL semantics, including `NOT IN`'s behaviour with nulls).

### 7.12 The core function catalog

The function catalog is versioned with the tree schema (§13) and published with it. This spec fixes
the core operators' names and typing:

- comparison: `eq ne lt le gt ge` (§3.2), `same` (§3.1, always `bool`), `is_null`, `is_not_null`
  (non-strict, `bool`);
- logic: `and or not` (three-valued, non-strict; `and` is `false` if either side is `false`);
- arithmetic: `add sub mul div mod neg abs` (§4.4);
- `coalesce(x₁…xₙ)`: common type, nullable iff every argument is nullable;
- `assert_not_null`, `at_zone`, `from_zone` (§7.6);
- text: `concat length lower upper contains starts_with like` (`like` takes its escape character as an
  argument), all strict: `concat(text, text) → text`, `length(text) → int64`, `lower(text)` and
  `upper(text) → text`, `contains(text, text)` and `starts_with(text, text) → bool`,
  `like(text, text, text) → bool`;
- time: `add(timestamp, interval)` computes calendar parts in UTC; `add(datetime, interval)` and
  `add(date, interval) → datetime` compute in civil time; `statement_timestamp() → timestamp`;
- sequences: `next_value(schema: text, name: text) → int64`, which draws the next value of a sequence
  (`ddl.md` §2.7). It is **volatile**: each call draws a new value, and it is never folded.

The operands of the comparisons, `same` and `coalesce` meet at their common type (§4.2, `Q3001`);
`eq`/`ne` then refuse a relation type anywhere in it (`Q3006`) and the orderings an unorderable one
(`Q3007`). `and`, `or` and `not` take `bool`/`bool?` (`Q3002`) and are `bool?` when an argument is.
The core functions resolve before a catalog's own; a name no catalog has is `Q2007`. A function the
catalog has that is not an aggregate, written as an `AggCall`, is `Q3002`.

## 8. Names, binders and scoping

### 8.1 Catalog names

A `QualName` is `(schema, name)`, both required. There is no search path in a tree; the front end
resolves one. Names compare byte-for-byte.

### 8.2 Binders

1. Columns are referenced by **binder and name**: `Col(binder, name)`. Ordinals are never used in a
   tree. *Rationale: names survive column reordering, read well in `EXPLAIN`, and are what a client
   building trees by hand already has; resolution to positions is the server's job.*
2. A binder is a non-empty string (`Q5009`) introduced by a node's `as` field (or a `Match` arm, or
   a DML node's `old` or `excluded`). Every binder MUST be unique across the whole statement
   (`Q2004`). A `Match` arm's binder is visible in that arm's expression only. *Rationale: no shadowing, so a reference has
   exactly one possible meaning and the error is always "unknown", never "ambiguous".*
3. The nodes listed with `as` in §6 introduce a binder; their consumers see their columns only
   through it. `Filter`, `Sort`, `Limit` and `Distinct` pass their input's exposed binders through
   unchanged. `Join` and `Apply` expose per §6.5; `Window` and `Unnest` add their own binder to the
   input's.
4. A `Join` or `Apply` binder and its `mark` column name are both required for `mark` and otherwise
   MUST both be absent (`Q5009`). A reference from a `Join`'s right input to a binder of its left
   input is `Q5006` (§6.5 rule 2), not `Q2006`.

### 8.3 Scope

The binders **visible** to an expression attached to node `N` are, in order of search:

1. the binders exposed by `N`'s inputs (for `Join` and `Apply`'s `on`, both inputs; for `Project`,
   `Aggregate`, `Window`, `Filter`, `Sort`, `Nest`, `Unnest`, its one input; for a DML node, §9);
2. then, if `N` lies inside a subquery expression (`Relation`, `Scalar`, `Exists`, `In`) or the right
   input of an `Apply`, the binders visible at that expression (or exposed by the `Apply`'s left
   input), recursively outward.

A reference to a binder found by step 2 is a **correlated** reference. A binder that exists in the
statement but is not visible is `Q2006`; one that does not exist is `Q2002`; a column the binder does
not have is `Q2003`.

Expressions under `Limit.count`/`offset` and `Values.rows` see only step 2.

### 8.4 Output names

A node's output columns are its exposed binders' columns, in the order the binders were exposed
(left before right), each binder's columns in their own order. Where §5 rule 5 requires unique names, the
plain column names (without binder) MUST be unique.

## 9. DML

All DML nodes are statement roots only; a DML node elsewhere is `Q5002`. Data-modifying CTEs are not
in 1.0.

### 9.1 Binders in DML

Each DML node has a binder `as` for the target row and an optional binder `old`. In `filter`, `set`
and the conflict action, `as` denotes the existing row. In `returning`, `as` denotes the row as it was
written (or, for `Delete`, as it was removed), and `old` denotes the row as it was before the
statement, with every column nullable (null for a newly inserted row).

### 9.2 Insert

`Insert { table, as, old: binder?, columns: [name], source: Rel, on_conflict: OnConflict?, returning:
[(name, Expr)] }`

1. `columns` MUST name distinct (`Q5005`) insertable columns (`Q5003`). Source columns pair with
   `columns` by position; their counts MUST match (`Q3001`) and each MUST coerce to the target column
   (`Q3001`). A nullable source column into a non-null target is `Q3005`.
   *Rationale: a NOT NULL violation that can be found statically is found statically.*
2. Columns not named take their defaults, identity values or generated values (`ddl.md` §2.7). A
   generated column, or an identity column declared `always`, MUST NOT be named (`Q5003`).

### 9.3 Upsert

`OnConflict { target: columns [name] | constraint name, excluded: binder, action: nothing | update {
set: [(name, Expr)], where: Expr? } }`

1. `target` MUST identify a unique constraint or unique index of the table (`Q5004`).
2. For each source row: if no existing row conflicts on `target`, the row is inserted. Otherwise, with
   `nothing` the row is skipped; with `update`, if `where` is absent or `true` the existing row is
   updated by `set`, evaluated with `as` (the existing row) and `excluded` (the proposed row) visible;
   else it is skipped. Skipped rows produce no `returning` row and are not counted. Both `as` and
   `excluded` have the table's row type; `target`'s columns match a unique key's in any order, and a
   constraint name matches a unique constraint's or a unique index's name.
3. A statement that would insert or update the same target row twice is `R0007`.

### 9.4 Update and Delete

`Update { table, as, old: binder?, from: Rel?, filter: Expr?, set: [(name, Expr)], returning }`,
`Delete { table, as, old: binder?, using: Rel?, filter: Expr?, returning }`.

1. `set` MUST be non-empty (`Q5009`) and name each column at most once (`Q5005`); values coerce to the
   column's type (`Q3001`, `Q3005`).
2. `from`/`using` is joined to the target by `filter`; its binders are visible in `filter`, `set` and
   `returning`. A target row matched by more than one `from` row MUST raise `R0007` (Postgres picks one
   arbitrarily; TernDB refuses).
3. Absent `filter` means every row.

### 9.5 Returning and Action

`returning` items see `as`, `old`, and the `from`/`using` binders. `Action()` is valid only in
`returning` (`Q5002`), outside any subquery of a returning item, and has type `union<inserted: {}, updated: {}, deleted: {}>`. The result is a bag
of the returning records.

## 10. Parameters, shape and batches

### 10.1 Declaration and binding

1. Every parameter is declared in the envelope with a type (any type, including relation types).
   Every declared parameter MUST be referenced at least once (`Q4002`).
2. A bound value MUST have the declared type exactly (`Q4003`); no coercion happens at bind. The
   number of values MUST equal the number of declarations (`Q4004`). Null bound to a non-nullable
   parameter is `Q4005`.
3. Parameter names are documentation and error-message material only.

### 10.2 Shape

The **shape form** of a validated statement is its canonical encoding (§12) at the server's current
schema version, after: (a) every `ParamDecl.name` is made absent; (b) `spans` is made absent; (c) every
binder is replaced by `b<i>` and every CTE name by `c<i>`, numbered in order of first introduction in
pre-order. A binder is introduced by an `as`, a `Match` arm, a DML node's `as`, `old` or `excluded`; a
CTE name by its `CteDef`. A reference (`Col`, `Row`, `CteRef`) takes the number of what it resolves
to (§8.3, §6.13), so an inner CTE that reuses an outer name gets its own. Output column names, parameter types, literal types and values, table names and everything
else are kept.

1. Two statements have the **same shape** iff their shape forms are byte-identical.
2. The **shape digest** is the SHA-256 of the shape form (32 octets); the wire carries it as is
   (`wire-protocol.md` §4.3). A server's plan cache MUST key on the shape form or its shape digest,
   together with the catalog version the plan was made against. A server MAY re-plan for
   particular parameter values, but MUST NOT give two statements of the same shape different
   validation results.
3. `EXPLAIN` MUST report the shape digest, so a test can check that two trees share a plan.

### 10.3 Batches

An execution MAY bind a **batch**: a sequence of parameter tuples. The statement then runs once per
tuple, in order, within one transaction (the caller's, or an implicit one), atomically: if any run
fails, none take effect. Results are returned per tuple, in order, tagged with the tuple's index.

### 10.4 Relation-valued parameters

A parameter of relation type, read through `ParamScan` or used as a `Relation`-typed expression, is
the preferred form of bulk input: one execution, one plan, set-based. *Rationale: a batch of 10,000
inserts is 10,000 executions; a `ParamScan` of 10,000 rows is one.* The wire MAY carry such a
parameter in the columnar result layout instead of §12.6 (`wire-protocol.md` §4.2); the value bound
is the same.

## 11. Validation and the error model

### 11.1 Errors

A validation error is `{ code, path, message, details }`. `code` is stable across releases and is what
clients and tests match on; `message` is human text and not stable. `path` locates the node: a
sequence of steps, each a field name from the schema with an index for list elements, written
`/statement/source/rows/0/1`. An error in a bound parameter value has the path
`/params/<tuple>/<index>` (both 0-based: the tuple of the batch, §10.3, and the declaration), followed
by one `/<field or element index>` step per level into a composite value. `details` is a map from stable keys (`binder`, `column`, `expected`,
`actual`, `missing_tags`, …) to text. When `spans` is present, the error also carries the source span.

### 11.2 Order of checks

Validation runs in phases: decode (`Q1…`), names (`Q2…`), types (`Q3…`), parameters and structure
(`Q4…`, `Q5…`). A server MUST stop after the first phase that finds an error. Within that phase it MUST
report the error whose path comes first in pre-order, and SHOULD report every other independent error
in that phase, sorted by path.

### 11.3 Codes

The validation codes (`Q…`) and the runtime codes (`R…`) this spec raises, with their names, are
registered in `errors.md` §2 and §3; that registry is the one list of them, and its rules (code form,
names as aliases, no reuse) apply here. Constraint violations and transaction errors (`T…`) are raised
under `isolation.md` and registered in `errors.md` §4.

## 12. Canonical binary encoding

The same bytes cross the C ABI and the wire. `wire-protocol.md` frames them and MUST NOT alter them.

### 12.1 Primitives

| Primitive | Encoding |
|---|---|
| `uvar` | unsigned LEB128, minimal (no redundant `0x80` continuation bytes) |
| `svar` | zigzag, then `uvar` |
| `bool` | one octet, `0x00` or `0x01` |
| `str` | `uvar` byte length, then UTF-8 |
| `list<X>` | `uvar` count, then each `X` |
| `opt<X>` | `0x00`, or `0x01` then `X` |
| `enum` | `uvar` ordinal, from 0 in the order the spec lists the values; one past the last is `Q1002` |
| `node` | `uvar` kind, `uvar` payload length, then the payload: the node's fields in the order this spec lists them |

The encoding is **canonical**: one tree has exactly one encoding. A decoder MUST refuse with `Q1003` a
non-minimal `uvar`, a `bool` other than 0/1, an `opt` tag other than 0/1, or a nullable wrapper around
a nullable type. *Rationale: canonical bytes make shape equality a byte comparison.* Pairs
`(name, X)` encode as `str` then `X`. A binder is a `str`.

### 12.2 Envelope

```
Envelope  := uvar major, uvar minor, list<ParamDecl>, node Statement, opt<Spans>
ParamDecl := opt<str> name, Type
Spans     := list<(uvar node_preorder_index, uvar start, uvar end)>   ordered by index
```

A span's index counts the statement's `Statement`, `Rel` and `Expr` nodes from 0 (the `Statement`) in
the order their kinds appear in the encoding; `Type` nodes are not counted. The indexes MUST strictly
increase (`Q1003`, so a set of spans has one encoding). An index past the last node, or a `start`
after its `end`, is `Q1001`.

### 12.3 Types

`Type` is a `node` with kind:

| Kind | Type | Payload |
|---|---|---|
| 0 | `bool` | — |
| 1–4 | `int16`, `int32`, `int64`, `int128` | — |
| 5, 6 | `float32`, `float64` | — |
| 7 | `decimal` | `uvar p, uvar s` |
| 8, 9 | `text`, `bytes` | — |
| 10–13 | `date`, `time`, `datetime`, `timestamp` | — |
| 14, 15 | `interval`, `uuid` | — |
| 16 | record | `list<(str, Type)>` |
| 17 | union | `list<(str, Type)>` |
| 18 | `bag` | `Type` |
| 19 | `list` | `Type` |
| 20 | nullable | `Type` (not itself nullable) |
| 21 | `int8` | — |
| 22–25 | `uint8`, `uint16`, `uint32`, `uint64` | — |
| 26 | `json` | — |
| 27–31 | reserved (scalar types) | |
| 32 | named type reference (`ddl.md` §2.8) | `str schema, str name` |
| 33–63 | reserved (named catalog types, `ddl.md`) | |

A named type reference denotes the structure its catalog type defines, resolved at validation against
the statement's catalog snapshot (an unknown name is `Q6004`); it is an alias, so type identity (§2.6)
compares the structures. It MAY appear wherever a statement declares a type: a `ParamDecl`, `Values`
columns, `Cast`, `Tag`, `Variant`, inside another type, and in DDL statements. A named type is always
a record or a union, never nullable, so a nullable wrapper around a reference is canonical. Because
the reference is kept in the encoding, it is part of the statement's shape (§10.2). A result type is always **structural**: a server MUST NOT send a named catalog
type (kinds 32–63) in a statement's result type, and sends the structure the name denotes
(`wire-protocol.md` §6.1).

### 12.4 Relational and expression kinds

Each payload is the fields in the order of the tables in §6, §7 and §9; an `Expr?` is `opt<node>`, a
`[X]` is `list<X>`, a name or binder is `str`, `QualName` is `str schema, str name`, a kind/op/mode is
an `enum` in the order the spec lists its values.

Rel kinds: 1 `Scan`, 2 `Values`, 3 `ParamScan`, 4 `Filter`, 5 `Project`, 6 `Join`, 7 `Apply`,
8 `Aggregate`, 9 `Window`, 10 `Sort`, 11 `Limit`, 12 `Distinct`, 13 `SetOp`, 14 `With`, 15 `CteRef`,
16 `Nest`, 17 `Unnest`, 18 `Tag`. Expr kinds: 1 `Col`, 2 `Row`, 3 `Param`, 4 `Null`, 5 `Call`, 6 `Cast`,
7 `If`, 8 `Match`, 9 `Record`, 10 `Field`, 11 `Variant`, 12 `Relation`, 13 `Scalar`, 14 `Exists`, 15 `In`,
16 `Action`, 17 `Literal` (payload `Type`, then the value's §12.6 encoding). The auxiliary records `AggCall`, `WinCall`, `Frame`, `Bound`, `SortKey`, `CteDef`,
`OnConflict` are encoded inline as their fields in order (not as nodes); a choice inside them
(`OnConflict.target`, `action`) is an `enum` followed by the chosen alternative's fields (`target`: 0
`columns`, then `list<str>`, 1 `constraint`, then `str`; `action`: 0 `nothing`, 1 `update`, then `set`
and `where`). An optional list (`CteDef.columns`) is `opt<list<…>>`; `Values.rows` is
`list<list<node>>`. Kind 0 and kinds 64 and above in every family are reserved.

A `Literal`'s value can be read only through its type, so the decoder checks that type as it reads
it: one that is not a non-null scalar is refused with `Q3017` during decoding, the one `Q3…` code the
decode phase (§11.2) raises.

### 12.5 Statement kinds

1 `Query`, 2 `Insert`, 3 `Update`, 4 `Delete`; 32–63 DDL, assigned and encoded by `ddl.md` §3.

### 12.6 Parameter and literal values

A parameter value is encoded by its declared type, with no tags (the type is known):

| Type | Encoding |
|---|---|
| `bool` | one octet |
| `intN` | N/8 octets, little-endian two's complement |
| `uintN` | N/8 octets, little-endian unsigned |
| `decimal(p, s)` | 16 octets, little-endian unscaled `int128` |
| `float32`/`float64` | IEEE 754, little-endian |
| `text`, `bytes`, `json` | `uvar` length, octets (`json`: its canonical form, §3.5) |
| `date` | `int32` LE, days since 1970-01-01 |
| `time`, `datetime`, `timestamp` | `int64` LE microseconds (since midnight; since 1970-01-01T00:00 civil; since the Unix epoch) |
| `interval` | `int32` months, `int32` days, `int64` micros, each LE |
| `uuid` | 16 octets, RFC 9562 order |
| `T?` | `0x00` for null, or `0x01` then the `T` encoding |
| record | each field in order |
| union | `uvar` discriminant, then the payload record |
| `bag`/`list` | `uvar` count, then each element |

Fixed-width scalars are fixed-width so a decoder can read a column without a varint loop. This table
is the encoding of parameter values and of `Literal` values. Results are not encoded this way: they
use the columnar result encoding of `wire-protocol.md` §6, which holds each **fixed-width scalar
payload** in exactly the octets this table gives that scalar, so one scalar decoder serves parameters,
literals and result columns. If the wire spec needs a different fixed-width scalar encoding, it changes
this table in the same commit.

### 12.7 Worked encoding

`(query :rel (filter :input (scan :table app.t :as t) :predicate (call eq t.id $0)))`, with one
parameter `$0` named `id` of type `int64` and no spans, is these 40 octets:

```
01 00                      major 1, minor 0
01  01 02 69 64  03 00     one ParamDecl: name "id", Type int64 (kind 3, empty payload)
01 1c                      Statement Query, 28-octet payload
  04 1a                    Rel Filter, 26 octets
    01 08 03 61 70 70 01 74 01 74          Scan: "app", "t", binder "t"
    05 0e 02 65 71 02                      Call "eq", two arguments
      01 05 01 74 02 69 64                 Col "t" "id"
      03 01 00                             Param 0
00                         no spans
```

Its shape form (§10.2) drops the parameter's name and renames the binder `t` to `b0`, which grows
the `Scan`, the `Col` and every length around them:

```
01 00  01 00 03 00  01 1e  04 1c  01 09 03 61 70 70 01 74 02 62 30
05 0f 02 65 71 02  01 06 02 62 30 02 69 64  03 01 00  00
```

## 13. Versioning and negotiation

1. The tree schema has a version `major.minor`, published with every release as a machine-readable
   schema file together with the function catalog and error codes. The schema file is a JSON
   description of every node kind, field, enum and type kind, **generated from the implementation's
   source** at build time, so it cannot drift from what the server decodes.
2. A **minor** version MAY add node kinds, enum values, functions, error codes and fields. An added
   field MUST be appended to the end of its node's payload and MUST have a default value under which the
   node means exactly what it meant before. A minor version MUST NOT change the meaning of any existing
   tree.
3. A **major** version MAY change anything. A server MUST accept its current major and the two
   previous majors.
4. **Negotiation.** On the wire, the client offers the list of `(major, max_minor)` it supports; the
   server answers with the highest common major and, for it, `min(client max_minor, server max_minor)`.
   No common major ends the connection with `Q1005`. Through the C ABI, `terndb_tree_version()` reports
   the library's `(major, minor)`, and that is the negotiated version.
5. An envelope's version MUST have the negotiated major and a minor at most the negotiated minor
   (`Q1005`). Its fields are decoded per its own minor; fields of a later minor take their defaults.
   An encoder MUST NOT emit a field or kind beyond the envelope's minor.
6. Shape forms are computed after re-encoding at the server's current version (§10.2), so one query
   sent by clients of different minors has one shape.

## 14. Substrait

A server MUST accept a Substrait plan in place of a tree, in the one Substrait version pinned for its
release (`Q7004` otherwise), and MUST translate it to a tree that is then validated as any other.
Errors from the translation are `Q7…` with a path into the Substrait plan; errors from validation carry
the tree path and the Substrait relation's anchor. Substrait is **import-only**: TernDB never exports
its plans as Substrait.

### 14.1 What maps

| Substrait | Tree |
|---|---|
| `ReadRel` named table | `Scan` |
| `ReadRel` virtual table | `Values` of `Literal`s |
| `FilterRel`, `SortRel` | `Filter`, `Sort` |
| `ProjectRel` (+ `emit`) | `Project` listing the kept input fields and the new expressions |
| `JoinRel` inner/left/right/outer/left_semi/left_anti/left_single/left_mark | `Join` inner/left/right/full/semi/anti/single/mark; the `right_*` kinds swap inputs |
| `CrossRel` | `Join` inner, no `on` |
| `HashJoinRel`, `MergeJoinRel`, `NestedLoopJoinRel` | `Join`; the physical choice is dropped |
| `AggregateRel` with one grouping set | `Aggregate` |
| `FetchRel` | `Limit`, with `arbitrary = true` unless its input maps to a `Sort` |
| `SetRel` union/minus/intersection (primary and multiset forms with two inputs) | `SetOp` |
| `ConsistentPartitionWindowRel`, window function expressions | `Window` |
| `WriteRel` insert/delete/update | `Insert`/`Delete`/`Update`; output mode "modified records" becomes `returning` of every column |
| field reference (ordinal, `outer_reference` steps) | `Col` by the referenced column's name; outer steps become correlated references |
| literal of a mapped scalar type | `Literal`; a null literal is `Null` |
| scalar function in a mapped extension URI | `Call` per the published mapping table |
| `IfThen`, `SwitchExpression` | `If` |
| `Cast` (throw / return null) | `Cast` strict / try |
| scalar, in-predicate and `exists` subqueries | `Scalar`, `In`, `Exists` |
| `SingularOrList`, `MultiOrList` | `In` over `Values` |
| types `boolean i8 i16 i32 i64 fp32 fp64 string binary date time uuid decimal(p≤38)` | the matching scalar |
| `timestamp`, `timestamp_tz`, precision variants with precision ≤ 6 | `datetime`, `timestamp` |
| `interval_year`, `interval_day`, `interval_compound` | `interval` |
| `struct` (names from the enclosing `NamedStruct`, else `f0`, `f1`, …) | record |
| `list<struct>`, `list<T>` of a mapped scalar `T` | `list<record>`, `list<T>` |

### 14.2 What does not map, and why

| Substrait | Code | Why |
|---|---|---|
| `varchar<L>`, `fixed_char`, `fixed_binary` | `Q7002` | A length is a constraint in TernDB, not a type. |
| precision > 6 timestamps | `Q7002` | Microsecond values cannot hold them losslessly. |
| `map`, `list` of a list, user-defined types | `Q7002` | No equivalent type in 1.0. |
| multiple grouping sets, `ExpandRel` | `Q7001` | Grouping sets are not in 1.0. |
| `ExtensionLeaf/Single/MultiRel`, `ExchangeRel`, `DdlRel`, local files | `Q7001` | Opaque, distributed or DDL: outside the tree. |
| functions outside the mapping table | `Q7003` | No silent approximation of a function's semantics. |
| `unique` set predicate | `Q7001` | No tree equivalent in 1.0. |

Tagged unions, `bag`-typed nested relations, upsert and `Action` have no Substrait form; they are
reachable only through a tree.

## 15. Text form and EXPLAIN

`EXPLAIN` prints the validated tree in the text form below, then the physical plan, then the shape
digest. The text form is also how this spec writes examples. A server SHOULD provide a parser for it
(tests are written in it); the binary encoding stays the only input format a client is required to
produce.

```
node    = "(" kind field* ")"
field   = ":" fname value
value   = node | "[" item* "]" | binder "." column | "$" index | literal | name | type
literal = "'" text-form "'::" type
item    = value | name ":" value
```

A `Col(b, c)` prints as `b.c`; a `Param(i)` as `$i`; a `Literal` as its §3.5 text form quoted, with
`'` doubled inside, then `::` and its type (`'3'::int64`); a pair list as `[name: value …]`; `SortKey` as
`(asc|desc expr nulls_first|nulls_last)`; types as `int64`, `text?`, `{a: int64}`, `bag<…>`,
`list<…>`, `union<T: {…}, …>`, and a named type reference (§12.3 kind 32) as its qualified name,
`app.status`. A field holding its default is omitted.

## 16. Worked examples

### 16.1 Users, their 3 newest posts with comments, and a post count

GraphQL:

```graphql
query { users(first: 50, orderBy: {id: ASC}) {
  id name postCount
  posts(first: 3, orderBy: {createdAt: DESC}) { id title comments { id body } } } }
```

Parameters: `$0: int64` = 50, `$1: int64` = 3. Every column is `NOT NULL`.

```
(query :rel
 (project :as r
  :input (limit :count $0
           :input (sort :keys [(asc u.id nulls_last)]
                    :input (scan :table app.users :as u)))
  :items [
   id: u.id
   name: u.name
   post_count: (scalar :rel
     (aggregate :as pc
       :input (filter :input (scan :table app.posts :as p1)
                      :predicate (call eq p1.author_id u.id))
       :aggs [n: (count)]))
   posts: (relation :rel
     (project :as pp
       :input (limit :count $1
                :input (sort :keys [(desc p.created_at nulls_last) (desc p.id nulls_last)]
                         :input (filter :input (scan :table app.posts :as p)
                                        :predicate (call eq p.author_id u.id))))
       :items [
        id: p.id
        title: p.title
        comments: (relation :rel
          (project :as cc
            :input (filter :input (scan :table app.comments :as c)
                           :predicate (call eq c.post_id p.id))
            :items [id: c.id body: c.body]))]))]))
```

Result type:

```
list<{id: int64, name: text, post_count: int64,
      posts: list<{id: int64, title: text, comments: bag<{id: int64, body: text}>}>}>
```

`post_count` is `int64`, not `int64?`, because a keyless `Aggregate` is cardinality-one (§7.10). The
second sort key makes the per-user order total, so the result is deterministic. `comments` is a bag
because nothing ordered it.

*Informative — what the unnester produces.* `U` = the 50 users. `P` = `posts` semi-joined to `U` on
`author_id`, with window columns `rn = row_number()` and `cnt = count()` partitioned by `author_id`
and ordered by `(created_at desc, id desc)`, filtered to `rn ≤ $1`. `C` = `comments` semi-joined to `P`
on `post_id`. Then `Nest C` into `P` by `post_id`, `Nest P` into `U` by `author_id` with a left join,
and `post_count` = `cnt` or 0 for a user with no posts (Neumann–Kemper's treatment of the COUNT bug).
Three scans, three batched joins, no per-user work.

### 16.2 A list of union elements

GraphQL `search(text: $q, first: 10)` returns `[SearchResult]`, where `SearchResult = Post | Photo`
and both implement an interface with `id` and `createdAt`.

```
SR = union<Post: {id: int64, created_at: timestamp, title: text},
           Photo: {id: int64, created_at: timestamp, url: text}>
```

Parameters: `$0: text`, `$1: int64` = 10.

```
(query :rel
 (limit :count $1
  :input (sort :keys [(desc s.created_at nulls_last) (asc s.id nulls_last)]
   :input (setop :op union :all true :as s
    :inputs [
     (tag :as t1 :type SR :tag Post
       :input (project :as a
                :input (filter :input (scan :table app.posts :as p)
                               :predicate (call contains p.title $0))
                :items [id: p.id created_at: p.created_at title: p.title]))
     (tag :as t2 :type SR :tag Photo
       :input (project :as b
                :input (filter :input (scan :table app.photos :as f)
                               :predicate (call contains f.caption $0))
                :items [id: f.id created_at: f.created_at url: f.url]))]))))
```

Result type `list<SR>`. `s.created_at` and `s.id` are SR's shared columns (§2.4), so the sort needs no
`Match`. A projection that reads variant fields must match:

```
(project :as out :input … :items [
  id: s.id
  label: (match :expr (row s) :arms [(Post x x.title) (Photo y y.url)])])
```

Dropping the `Photo` arm is refused with `Q3009`, `details.missing_tags = "Photo"`.

### 16.3 Upsert with returning

SQL intent: add `$1` to counter `$0`, creating it if absent, and report what happened.

Parameters: `$0: text`, `$1: int64`. Table `app.counters(key text primary key, n int64 not null)`.

```
(insert :table app.counters :as t :old o
  :columns [key n]
  :source (values :as v :columns [key: text n: int64] :rows [[$0 $1]])
  :on_conflict (on_conflict :target (columns [key]) :excluded x
                 :action (update :set [n: (call add t.n x.n)]))
  :returning [key: t.key n: t.n previous: o.n action: (action)])
```

Result type `bag<{key: text, n: int64, previous: int64?, action: union<inserted: {}, updated: {},
deleted: {}>}>`. First run with (`"hits"`, 1): one row `{hits, 1, null, inserted}`. Second run with
(`"hits"`, 5): `{hits, 6, 1, updated}`. Binding `$1` to null is `Q4005`. Declaring `$1: int64?`,
and the `Values` column `n: int64?` that carries it, makes the source column `n` nullable into a
`NOT NULL` column and is refused with `Q3005` (§9.2 rule 1); with the `Values` column left `int64`,
the nullable parameter is refused where it meets that column, with `Q3001` (§6.2).

## 17. Open questions

The questions this section listed have been answered; the answers, and what to revert for each, are in
`docs/decisions.md` (Q1–Q12). One remains:

1. **`int128`.** Decision Q2 lists the integer types as `int8…int64` and `uint8…uint64`, without
   `int128`. This spec keeps `int128` (§2.1), because `sum` over `int64` (§6.7) and the `uint64`
   coercion (§4.1) rely on it. Should it stay a user-visible type?
