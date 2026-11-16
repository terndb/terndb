# Unnesting — design note

The code is `sh/sysl/terndb/plan_unnest.sysl`; the rule it meets is `query-tree.md` §6.6 rule 2. Before a
query is planned (`run_query`, `exec_plan.sysl`), every `Apply` and every subquery expression
(`Relation`, `Scalar`, `Exists`, `In`) that can be decorrelated is rewritten into ordinary joins, by
Neumann and Kemper's general unnesting ("Unnesting Arbitrary Queries", BTW 2015). The result is an
ordinary query tree: it is type-checked again, and kept only if its result type is the original's
(otherwise the original runs whole, per row, and the reason is recorded). Whatever cannot be
decorrelated keeps the per-row form of `executor.md` §4.2, which is also the oracle the tests check the
rewrite against.

## 1. The domain

For a correlated side `R` over an outer side `L`, the **outer references** are the distinct `Col` and
`Row` expressions in `R`, at any depth, that read a binder `L` exposes. The **domain** `D` is
`Distinct(Project(copy of L, d, [k_i: ref_i]))`: each binding once. A reference to no binder at all (an
uncorrelated `R`) gets one constant column, so `D` is one row when `L` has rows and none when it is
empty. The copy renames every binder `L` introduces (binders are unique, §8.2 rule 2).

`D` is **pushed** into `R`: each operator is rewritten to compute its result for every domain row at
once, carrying the domain columns, until no operator references `L`. `R` becomes `R'`, and the
correlated node becomes a join of `L` and `R'` on `same(ref_i, R'.k_i)` for every `i`. The comparison
is `same`, never `eq`, so a null binding meets its own rows (§3.1). The `same` conjuncts are the merge
join's keys (`executor.md` §4.1), so pairs from different bindings are never formed and the rest of
`on` is evaluated only where the per-row form would evaluate it (§6.5 rule 8).

The node that becomes the join: `Apply` of kind `cross`, `outer`, `semi`, `anti`, `mark` or `single`
becomes the `Join` of kind `inner`, `left`, `semi`, `anti`, `mark` or `single`, its `on` conjoined with
the keys. A subquery expression in a position evaluated on every row of its node's input becomes a join
laid over that input, and the expression a column of it:

| Expression | Join | Value |
|---|---|---|
| `Exists(r)` | `mark` | the mark |
| `In(es, r)` | `mark`, `on` also `eq(es_i, r's column i)` | the mark |
| `Scalar(r)`, `r` cardinality-one (§7.10) | `inner` (`r'` has a row for every domain row) | `r`'s column |
| `Scalar(r)`, otherwise | `single` (null for none, `R0003` for two) | `r`'s column |
| `Relation(r)` | `inner`, to `Aggregate` by the domain of `collect` of `r`'s rows over `D` left-joined to `r'` | the collected relation |

`Relation`'s `collect` sees only the rows that came from `r'` (a row number taken over `r'` is its
filter), so a binding with no rows gets the empty relation, not null. Its `order` is the order `r'`'s
rows had (§3), so a list stays a list.

A list outer side keeps its order: a row number is taken over it before the first join, and the rows
are sorted by it after the last.

## 2. Pushing the domain, per operator

An operator whose subtree references no outer reference is joined to a copy of `D` as it is, the domain
on the **right**: an inner join with no right row reads nothing from its left (`exec_join.sysl`), so
the subtree is evaluated only when the outer side has rows (§6.6 rule 4). A list is numbered first.

| Operator | Under the domain |
|---|---|
| `Scan`, `ParamScan` | never correlated: joined to the domain as above |
| `Values` | rows that read the outer side: an index `Values` of `0..n` joined to the domain; each column the `If` of the rows by index (only the selected row's expression is evaluated, §7.5), cast to the column's type where it differs |
| `Filter` | the predicate over the pushed input |
| `Project` | the items, plus the domain columns (and the order's keys) as items under the same binder |
| `Sort` | sorted as it was; the order kept is its keys then its input's order (it is stable) |
| `Limit` | a `Window` numbering each domain group's rows in the group's order (`row_number`, and `rank` over the sort keys for `ties`), and a `Filter` on the numbers: `rn > offset`, `rn − offset ≤ count`, and with ties `count > 0 ∧ rank − offset ≤ count` |
| `Distinct` | over the pushed input; the domain columns are part of every row |
| `Join` | the domain pushed into the left input, and into the right input too when it is correlated or the kind is `right`, the domain columns then equated in `on`; a `right` join reads them from the right |
| `Apply` | the domain pushed into the left input; the inner `Apply` is then decorrelated against it (its references to the outer domain now read the pushed left input) — this is how correlation across several levels unnests |
| `Aggregate`, keyed | grouped by the domain columns first |
| `Aggregate`, no keys | the **count bug**: it must emit a row for every domain row, also one with no input rows. A fresh copy of `D` is left-joined to the marked input and grouped by its columns; every call's `filter` also requires the mark, so `count` is 0, `collect` the empty relation and the others null over an empty group. Arguments of a non-null type are read through `assert_not_null`, which is never reached on a padded row, so the result types are the original's |
| `Window` | the domain columns lead every call's `partition`; the input sorted by its order first, which ties and frames read |
| `Nest`, keyed | the domain columns lead the keys |
| `Nest`, no keys | the `Aggregate` §6.14 rule 1 says it equals (one `collect` of the items' record), with the count bug's treatment |
| `Unnest` | `value` over the pushed input |

Under a domain a relation is a bag, so an original list carries its **order** as sort keys that put
each domain group's rows back in order; `Limit`, `Window`, a list's `Relation` and the order-reading
aggregates use them. An operator whose parent does not read the order (a `Distinct` above, through
`Filter`, `Project` and `Sort`) is not numbered.

## 3. What is kept per row

Each of these is kept in its per-row form, and recorded among the plan's fallbacks; the rest of the
statement is still unnested around it.

1. A subquery expression in a position evaluated on only some rows, since evaluating it on every row
   could raise an error the per-row form does not (§6.6 rule 4, §7.5): an `If` arm's value or a later
   condition, a `Match` arm, a `coalesce` argument after the first, an aggregate call's arguments or
   `order`, a `Join`'s or `Apply`'s `on`, a `Window` item, a `Values` row, a `Limit`'s `count` or
   `offset`.
2. A `Limit` whose `count` or `offset` is not a non-negative `int64` literal or a parameter bound to
   one. The per-row form evaluates them once per outer row, and a negative one raises `R0006` for every
   outer row; under a domain they would be evaluated only for non-empty groups.
3. A `full` join under a domain: its rows unmatched on either side lose that side's domain columns.
4. A `Tag` under a domain, or over an input whose rewrite adds columns: its payload is its input's
   element.
5. A `Project`, `Aggregate`, `Nest` or correlated `Values` whose binder some `Row` reads whole, under a
   domain: the domain's columns would change the row.
6. A `Distinct` over an input whose rewrite exposes a row number (a list numbered to keep its order),
   which would make every row distinct.
7. Anything inside a `SetOp`, `With` or `CteRef`, which the rewrite does not enter; one under a domain
   keeps the whole correlated side per row.

A kept `Apply` still has its left input unnested and its right input rewritten where that leaves its
output unchanged; a kept subquery likewise.

## 4. What the result costs

The outer side is evaluated once more for each domain, as a copy: there is no shared subplan yet (the
executor runs no `With`). A keyless `Aggregate` or `Nest` under a domain, and each `Relation`, copy the
domain once more. Each list kept in order costs a row number and a sort. All of these are linear in the
rows, where the per-row form re-plans and re-runs the correlated side for every outer row.
