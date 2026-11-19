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
| `Filter` | the predicate over the pushed input; where that input is a cross join (an uncorrelated subtree beside the domain, as above), the predicate's conjuncts that hold no subquery become the join's `on`, and the `Filter` stays only for the rest |
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

The `Filter` rule is what keeps the common shape linear. `exists (select … from orders o where
o.user_id = u.id and …)` pushes the domain into `Scan(orders)` as a cross join with it, and a `Filter`
over that pair would test every order against every binding — the per-row form's cost, |orders| ×
|users|. As the join's `on`, `eq(o.user_id, d.k)` is a merge key (`executor.md` §4.1), so only the
pairs of one binding are formed. A `Filter` over a cross join and the join with its predicate as `on`
have one result (§6.4 rule 3, §6.5 rule 8: a conjunct may go unevaluated where another is not `true`,
and the join tests exactly the rows the `Filter` saw). A conjunct holding a subquery stays in a
`Filter` above, and is lifted over the join already narrowed, so a subquery nested in it is
decorrelated against the matching pairs only — correlation two levels deep stays linear too.

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
rows (each sort `n log n`), where the per-row form re-plans and re-runs the correlated side for every
outer row.

The joins back to the outer side are merge joins on the `same` keys, and an uncorrelated subquery's
one-row domain is joined back on `same(true, d.k)`, a constant beside a column, which is a merge key
too. `Exists` and `In` become mark joins whose `on` is keys alone, so no pair is tested (§4.1); `In`'s
null mark is found by lookup. What is left that grows with the product of two sides is a correlation
that is not an equality (`o.day > u.age` alone), whose pairs are those of one binding, and a mark
join's residual condition beside a null key, tested with the null-keyed rows of the left row's
partition.

`unnest_work_tests.sysl` holds this down: every shape — `exists`, `not exists`, scalar subqueries in
`where` and the select list (the count bug), `in` and `not in` correlated or not over nulls, an `in` of
two columns, correlation on two columns and two levels deep, `any` and `all` — runs over 100 and 1000
users with ten orders each and at the benchmark's 1000 and 10000, and the pairs its joins test
(`Work`, `exec_eval.sysl`) must stay under four per table row. The benchmark's three correlated
shapes test 7.2e3 to 1.5e4 pairs at 1000 users and 7.2e4 to 1.5e5 at 10000; a `Filter` left over the
cross join would test 1.0e7 and 1.0e9.
