# The cost-based optimizer — design note

This note designs TernDB's cost-based optimizer: the statistics it keeps, how it estimates cardinalities, the
cost model, the plan search, the physical choices it makes, how plans are cached and adapted, what parallel
execution will need from it, how it is tested, and how it is explained. It ends with an ordered implementation
plan cut into agent-sized items (§15) and the choices left to the user (§16).

It is **design, not specification**. What a plan computes is fixed by `docs/spec/query-tree.md`; this note
decides only which of the plans the spec allows is run. Where the optimizer needs something the spec does not yet
say (an `Explain` statement, `Analyze`, extended statistics), the item that builds it writes the spec first, in
the same commit (§15).

The bar is the best production optimizers: SQL Server's Cascades (and its descendants Orca and CockroachDB's
`opt`), Postgres's statistics and estimation, and Umbra/HyPer's and DuckDB's join enumeration and unnesting. Each
section says which of their techniques TernDB takes, which it leaves, and why.

It is written against four branches still pending at the time of writing; each item of §15 names the ones it
needs landed:

| Branch | What it builds | What the optimizer takes from it |
|---|---|---|
| `predpush` | `plan_pushdown.sysl`, `executor.md` §4.7: conjuncts pushed into comma, cross and outer joins, outer joins made inner, joins re-bracketed to minimise unkeyed pairs | the **safe expression** set (`pd_safe`), the error-safety rules for moving conjuncts, the null-rejection test. Its re-bracketing heuristic is replaced by the join search (§4.5) |
| `secidx` | `exec_access.sysl`, `executor.md` §4.6: secondary indexes, partial and expression matching, `access_of`'s fixed rule | the sargable-conjunct extraction, `matches_def`, partial-predicate matching, the counters `entries_read` and `rows_fetched`. `access_of`'s rule is replaced by a costed choice (§9.1) |
| `hashjoin` | `executor.md` §4.1: every equi-join a hash join, built on the smaller side chosen at run time; no merge join | `value_hash.sysl` (the canonical value hash the sketches reuse), the counters `Work.built/probed/compared/pairs`. The build side becomes a plan-time choice with a run-time guard (§9.2) |
| `rowscan` | `store_scan.sysl`: scans decode only the columns they read; `ReadWork` | the per-column decode costs (`ReadWork.columns/skipped/copied/values`) and the column list a scan is planned with |

## 1. What exists, and where the optimizer goes

Today a query runs as `run_query` (`exec_plan.sysl`): `type_envelope` checks it, `unnest_plan`
(`plan_unnest.sysl`) decorrelates it (and, on `predpush`, calls `push_query` to push predicates), and the
`Planner` lowers the rewritten **query tree** directly into executor operators. There is no logical or physical
plan between the tree and the operators; every choice is a fixed rule made while lowering (the access path in
`access_of`, the join algorithm from the shape of `on`, the build side at run time). The plan cache
(`engine.sysl`, `PlanCacheSize` 256) caches validated **trees** by shape digest; every execution plans again with
the actual parameter values.

The optimizer is inserted between unnesting and lowering:

```
type_envelope → unnest_plan → build memo → normalise → explore → implement and cost → extract → lower → run
                (unnesting.md)  (§4.1)       (§5.1)      (§4, §5.2) (§3.4, §8, §9)    (§3.6)    (Planner, §4.8)
```

- **Input**: the typed, unnested query tree (`UnnestPlan`), the transaction's catalog view, the statistics
  (§6), the parameters, and the session's settings (memory budget, degree of parallelism).
- **Output**: a **physical plan** (`PhysPlan`, §4.8): a tree of physical operators, each with its chosen
  algorithm, its inputs, the properties it provides, its estimated rows and cost vector (§8.1), and the id of
  the memo group it came from (for `EXPLAIN ANALYZE`, feedback and re-optimisation).
- **Lowering**: the `Planner` gains a second entry point that lowers a `PhysPlan` instead of a tree. The tree
  lowering stays for the per-row fallbacks `unnesting.md` §3 keeps (a kept `Apply` re-plans its right side per
  outer row, `executor.md` §4.2) and as the oracle the differential tests compare against (§14.3).

The optimizer is a set of new files, `opt_*.sysl`, beside the executor: `opt_memo`, `opt_props`, `opt_rules`,
`opt_norm`, `opt_hypergraph`, `opt_dphyp`, `opt_large`, `opt_card`, `opt_selectivity`, `opt_cost`,
`opt_access`, `opt_phys`, `opt_explain`, and the statistics in `stats_*.sysl` (§15 names each item's files).

## 2. What the spec allows: the legality rules

TernDB's spec is stricter than SQL about runtime errors: a plan may skip an error only where a rule says so.
These rules, not cost, decide which plans are candidates. They are the reason `predpush` refuses some
re-orderings, and the memo must encode every one of them. The optimizer may choose freely among plans that
differ only in what these rules permit; it may never choose a plan outside them, whatever it costs.

### 2.1 The rules the spec gives

1. **Conjunct short-circuit** (`query-tree.md` §6.4 rule 3, §6.5 rule 8): a conjunct of a `Filter`'s
   predicate, or of a join's `on`, MAY go unevaluated on a row (or pair) another conjunct of the same `and`
   makes not `true`. This is what lets an access path, a hash key and a pushed-down filter skip rows.
2. **`on` only on pairs** (§6.5 rule 8): with either input empty, `on` is never evaluated.
3. **Empty right input** (§6.5 rule 10): an `inner` or `semi` join whose right input is empty MAY leave its left
   unevaluated. The converse is **not** given: the right input is always evaluated.
4. **Which error first** (§6.5 rule 9): among several runtime errors the first raised is unspecified. A plan
   may raise a different one of the same statement's errors, but not raise one where the written plan raises
   none, nor raise none where it raises one, except as rules 1–3 allow.
5. **`and`/`or` evaluate both sides** (§7.12 rule 1) outside rules 1 and 2; `coalesce` and `If` are lazy (§7.5,
   §7.12 rule 1); a subquery in a lazily evaluated position stays per-row (`unnesting.md` §3).
6. **Apply** (§6.6 rule 2) must be removed by general unnesting where it can be, and the right side of one is
   evaluated for each left row and no other (§6.6 rule 4).
7. **Order**: a join's output is a bag (§6.5 rule 7); a list input to `Filter`/`Project` stays in order
   (§6.4); `Sort` is stable for a list (§6.9). A plan may deliver a bag in any order, including a useful one.
8. **Volatility**: `next_value` is never folded and draws once per call (§7.12); an expression holding it is
   never duplicated, moved across a row-count-changing operator, or evaluated on more or fewer rows.
9. **Logging** (`isolation.md` §4.4): what is logged depends on the read, not on the plan's shape: a scan logs
   its filter whatever its access path; an index-nested-loop probe logs each probe key or the probed side's
   restriction (already in the table). The optimizer therefore never has to make a plan choice for the
   sake of serializability; it must only hand each scan its full filter to log.

### 2.2 The derived properties the memo keeps

From these the optimizer derives, for every memo group (§4.2), the logical properties that gate the rules:

- **`may_raise`**: can evaluating this subplan raise a runtime error? `false` exactly for `predpush`'s "safe to
  evaluate" leaves: a `Scan` of a table (not a view), a `ParamScan`, and `Filter`, `Project`, `Distinct` and
  joins of these whose every expression is **safe** — built of `Col`, `Row`, `Field`, literals, nulls,
  parameters, `eq ne lt le gt ge same is_null is_not_null and or not coalesce` (`pd_safe`). One module,
  `opt_safety.sysl`, holds the set; `predpush`'s `pd_safe` moves into it when the memo's normaliser replaces
  `push_query` (item N3). Extending the set (a `Cast` known lossless from its types, a `length`, an
  `add` of two `int32` widened to `int64`) is a later, separately tested item (N6), since every addition widens
  every rule below.
- **`volatile`**: holds a `next_value`.
- **`guarded`** (per join): the join was made by the unnester to keep a correlated or uncorrelated subtree
  unevaluated over an empty outer side (`unnesting.md` §2: the domain on the right). §6.6 rule 4 makes the skip
  **mandatory** there, where §6.5 rule 10 makes it optional elsewhere. A guarded join's right input must be read
  first and, empty, must stop the join before its left is opened; every physical alternative keeps that
  (§9.2), and the join may be commuted only when its left input has `may_raise = false`.

### 2.3 The legality rules the memo applies

- **Moving a conjunct down** (into a join input, into an access path, into a hash key): any **safe** conjunct,
  as `predpush` §4.7 does, with its limits: into a right input only when the left input beside it is not
  `may_raise` (rule 3 above), never past a `full` join, a `coalesce` or `is_null` of a null-supplying side never
  taken as null-rejecting.
- **Moving a conjunct up** (out of an `on` into a `Filter` above, or into a later join): only a safe one. An
  unsafe conjunct is pinned to the operator it was written on, and evaluated on exactly the rows or pairs that
  operator sees.
- **An unsafe `on` fences its join**: the pairs it is evaluated on must be exactly the pairs of its two inputs.
  In the join hypergraph (§4.5) such a join's two input sets become **fences**: the join search may reorder
  within each side but no relation crosses the join. An unsafe `Filter` over a join region fences nothing —
  every order of an inner region produces the same rows for it to see — so it simply stays above the region.
- **Commuting a join** `A ⋈ B` to `B ⋈ A` (`inner`): legal when `B` is not `may_raise`, since the commuted plan
  may leave `B` unevaluated when `A` is empty (rule 3), which the written plan never does. A guarded join
  further requires `A` not `may_raise` (§2.2). Both conditions are TES-style constraints in the conflict
  detector (§4.6), so no rule has to re-check them.
- **Re-associating** across non-inner joins: by the conflict rules of §4.6, plus the error conditions above
  for every input that moves to a right-hand side.
- **`single`** raises `R0003` on a second match even for a left row a later `Filter` would remove; filtering its
  left input earlier would lose that error. So a `single` join is a fence for conjuncts, **unless** its right
  side is unique on the join keys (a primary key or a `ready` unique index, read from the catalog, or a key
  derived through the memo's key property): then no left row can match twice, and a normalisation rule turns
  it into a `left` join (N4).
- **Volatile** expressions are never duplicated, never pushed, never pulled; an operator holding one is a fence.
- **The per-row forms** `unnesting.md` §3 keeps are opaque leaves to the memo: their inside is planned per outer
  row by the tree lowering, as now.

A differential test (§14.3) runs every alternative plan of a random query and checks they agree; a separate
**error-semantics suite** (§14.4) runs queries built to raise an error on chosen rows and checks that every
plan raises it or not exactly as these rules require.

## 3. The search framework: Cascades memo, or bottom-up DPhyp?

### 3.1 The two candidates

**Cascades** (Graefe 1995; Columbia; SQL Server; Orca; CockroachDB's `opt`) keeps a **memo** of groups of
logically equivalent expressions. Transformation rules add logical alternatives to a group; implementation rules
add physical ones; a top-down search optimises each group for each **required physical property** (an order, a
partitioning), inserting **enforcers** (`Sort`, `Exchange`) where an input does not provide it, memoising the
**winner** per (group, required property), and pruning by **branch and bound**: a child is optimised with the
cost limit left by its siblings, and abandoned when its lower bound exceeds it. Its strengths are generality:
every choice — joins, aggregation placement, unnesting variants, access paths, orders, partitioning, index
nested-loop inners parameterised by an outer row — is a rule and a property in one framework. Its weaknesses are
join enumeration by rules: commutativity and associativity rules generate the same join tree many times (the
duplicate-free rule sets of Pellenkoft et al. 1997 fix that only for inner joins), outer and anti joins need
care rule by rule, and exploring all bushy trees by rules is slower than dynamic programming over the same space.

**Bottom-up DPhyp** (Moerkotte & Neumann 2008; HyPer, Umbra, DuckDB) enumerates the connected subgraph /
complement pairs (csg-cmp pairs) of the query hypergraph exactly once each, so it builds the optimal bushy join
tree with no duplicate work; hyperedges express complex predicates (`a.x + b.y = c.z`) and, with the
conflict detectors of Moerkotte, Fender & Eich (SIGMOD 2013, CD-C with total eligibility sets, TES, and conflict
rules), every non-inner join kind. It is the fastest complete enumerator known. Its weakness is everything that
is not join order: physical properties, interesting orders, aggregation placement and unnesting variants are
handled by heuristics before or after it (Umbra's and DuckDB's "rewrite, then enumerate joins, then pick
physical operators") and cannot be costed against each other.

### 3.2 Recommendation: a Cascades memo whose join groups are filled by DPhyp

TernDB takes **both**, the way CockroachDB (`ReorderJoins`, a DPSube into the memo) and Orca (DP join order
inside its memo) do, with DPhyp as the enumerator:

- The **framework** is a Cascades memo: groups, expressions, logical properties, required and provided physical
  properties, enforcers, winners per (group, required property), top-down optimisation with branch and bound, a
  rule registry with ids that tests can disable and `EXPLAIN (VERBOSE)` can trace.
- **Join order is not explored by transformation rules.** A maximal region of joins (inner, outer, semi, anti,
  mark, single) with its predicates is turned into a hypergraph (§4.5) the first time its root group is
  explored, and DPhyp enumerates its csg-cmp pairs, adding one group per connected relation set and, for each
  pair, the join expression(s) the conflict detector allows (both orders when commutable, §2.3). The groups the
  enumeration creates are ordinary memo groups; implementation rules then attach hash, nested-loop and index
  nested-loop alternatives to each, and the top-down search costs them under required properties like any other
  group.
- **Above a size limit** the region is enumerated by the adaptive scheme of Neumann & Radke ("Adaptive
  optimization of very large join queries", SIGMOD 2018): DPhyp while the number of connected subgraphs stays
  under a budget (default 10,000 csg-cmp pairs, which in practice is all chains and stars up to ~15–20
  relations and cliques up to ~10), then linearised DP (IKKBZ order, DP over its linear sequence) up to 100
  relations, then greedy operator ordering (GOO) with DP refinement of windows above that. This matches the
  plan's "dynamic programming up to ~15 relations, then an adaptive greedy fallback in the style of Umbra and
  DuckDB", made concrete by a budget instead of a fixed count.

Why not pure Cascades: rule-based join enumeration is slower and harder to make correct for non-inner joins than
DPhyp with CD-C, and join order is the choice that matters most. Why not pure DPhyp: TernDB needs to cost choices
DPhyp cannot see — index nested-loop joins whose inner is parameterised by the outer row, sort avoidance through
PK order, hash versus streaming aggregation, eager aggregation below joins, the unnesting variants (§5.3), and
later partitioning for parallel plans (§12). Those are properties and rules, which is what a memo is for.

### 3.3 The memo

- **Group**: a set of logically equivalent expressions, with **logical properties** computed once when the group
  is created (§4.4): output columns (binders and their columns), the base relations it covers (a bit set over the
  region's leaves), cardinality and per-column statistics (§7), unique keys, functional dependencies, not-null
  columns, `may_raise`, `volatile`, and whether its rows form a list (order significant).
- **Expression**: an operator, its scalar payload (a predicate as a canonical conjunct list, projection items,
  grouping keys, aggregate calls, access-path description) and its child **group ids**. Expressions are
  hash-consed: adding an expression already present returns the existing one, which is how duplicate rule
  firings stop. Scalar expressions stay query-tree `Expr`s, interned by a structural hash so that two equal
  conjuncts are one.
- **Logical operators**: the query tree's relational nodes (`query-tree.md` §6), with `Filter`'s and `Join`'s
  predicates held as conjunct sets.
- **Physical operators**: `TableScan(access)`, `IndexScan(index, range)`, `IndexIntersect`/`IndexUnion` (§9.1),
  `HashJoin(kind, build, keys, residual)`, `NestedLoopJoin(kind, on)`, `IndexNestedLoopJoin(kind, inner access,
  probe keys, residual)`, `HashAggregate`, `SortAggregate`, `StreamAggregate`, `HashDistinct`, `SortDistinct`,
  `StreamDistinct`, `Sort`, `TopN`, `Limit`, `Filter`, `Project`, `Window`, `Nest`, `Unnest`, `SetOp` (hash or
  sort), `With`/`CteRef`, `Values`, `ParamScan`, `Tag`, DML targets, and later `Exchange` (§12). Each lowers to
  an executor operator (`executor.md` §4), new ones built by the items of §15.
- **Required physical properties**: an **ordering** (a list of sort keys with direction and null placement, or
  none) and later a **partitioning** (§12). An ordering is required by a `Sort` above (which then may vanish), by
  a `StreamAggregate`/`StreamDistinct`, by a `Window`'s partition-and-order, by `Limit` with `ties`, by a list
  result, and by the order-keeping numbering `unnesting.md` adds for list outer sides.
- **Provided properties**: a PK scan or range provides PK order; an index scan provides PK order too (the store
  fetches rows in key order, `executor.md` §4.6) or, as `OrderedIndexScan` (P9), the index's order; a hash join
  that streams its probe side provides the probe side's order (rows are emitted per probe row in input order —
  a fact of the executor the item P2 pins with a test, and which a bag may legitimately carry, §2.1 rule 7);
  `Sort`, `TopN` and `StreamAggregate` provide their keys' order; `Filter`, `Project` and `Limit` preserve
  their input's. Functional dependencies and equalities from predicates reduce orderings (Simmen, Shekita &
  Malkemus 1996): an order on `(a, b)` with `a` fixed by `a = 5` satisfies a required order on `b`.
- **Enforcers**: `Sort` for orderings, `Exchange` for partitionings. An enforcer is added by the search, not by
  a rule, when the cheapest expression of a group does not provide what is required.

### 3.4 The search

The search is Columbia's task stack (Shapiro et al. 2001), iterative so it cannot overflow the sysl stack on a
deep query:

- `OptimizeGroup(g, required, limit)`: return the memoised winner if one exists for `(g, required)` and its cost
  is within `limit`, or the memoised fact that none is; else explore `g` and push `OptimizeExpr` for each
  physical expression.
- `ExploreGroup(g)`: fire the exploration rules on each logical expression once (a bit set per expression of
  the rules already fired); for a join-region root, run the enumerator of §4.5 instead.
- `ApplyRule(rule, expr)`: bind the rule's pattern, call it, add the expressions it returns.
- `OptimizeInputs(expr, required, limit)`: derive the children's required properties from the operator and
  `required`, optimise each child with the limit left after the operator's own cost and the costs of the
  children done so far, and record the expression as the group's winner if it is cheapest.

**Branch and bound.** A group's **lower bound** is the cost of reading its base relations through their cheapest
possible access path plus the cost of producing its output rows (`opt_cost.sysl` `lower_bound`), a true bound
since every plan of the group does at least that. An input whose lower bound exceeds the remaining limit is not
optimised; a winner found under a limit is memoised with the limit, so a later, looser request can resume it.

**Budget.** Every task counts toward a per-statement budget (default 100,000 tasks); past it the search stops
exploring, and each group takes the cheapest plan already costed, which always exists because the first
expression of every group (the one built from the query tree) is implemented before any other is explored. A
statement therefore always gets a plan, and `EXPLAIN` says when the budget cut the search.

### 3.5 Rules

A rule is a sysl value in a registry (`opt_rules.sysl`): an id (a stable name, `PushFilterIntoJoin`), a kind
(normalisation, exploration, implementation), a pattern (the operator it matches at the root, and a guard over
the children's logical properties), and a function from the bound expression to the expressions it adds. The
registry is a table, not a DSL: sysl has no macro or code generator for one, and the table is short. Every rule
can be disabled by id through a `RuleMask` in the optimiser's settings, which the rule tests (§14.2) and the rule
fuzzer (§14.6) use; `EXPLAIN (VERBOSE)` lists the rules that fired to produce each operator of the plan.

### 3.6 Extraction

The plan is extracted top-down from the winners: the root group's winner for the statement's required
properties (a list result's order, or none), each child's winner for the properties the parent required of it.
Each `PhysPlan` node keeps its group id, estimated rows and cost vector for `EXPLAIN ANALYZE`, feedback and
re-optimisation.

## 4. Building the memo and enumerating joins

### 4.1 From the query tree

The memo is built from the unnested, typed tree in one bottom-up pass: each relational node becomes an
expression in a new group. Binders are resolved to column ids once (`binder`, column ordinal), so that a
predicate's column references compare by id, not by name. The tree's `Typing` stays the source of types.

### 4.2 Logical properties, computed once per group

A group's properties are derived from the **first** expression added to it and never from a later one, so that
two equivalent expressions can never disagree. For a join group created by the enumerator, they are derived from
its **relation set and the predicates inside it**, not from the pair that created it, so that the cardinality of
`{a, b, c}` is one number whichever of `(a⋈b)⋈c`, `a⋈(b⋈c)` or `(a⋈c)⋈b` is costed (§7.4). This is the
consistency property join enumeration relies on.

### 4.3 Column order is not a constraint

`predpush` re-orders leaves only where the region's column order is invisible, because the `Planner` lays out
a join's flat record in §8.4 order from the tree's shape. The optimizer keeps each region's **logical binder
order** (the leaves' order in the written tree) in the `PhysPlan`, and the lowering builds the composite and the
flattened record in that order whatever the physical order of the joins. Commuting a join therefore never changes
the output's columns, and the root, a `SetOp` input and a `Tag` lose their restriction.

### 4.4 The join regions

A **region** is a maximal tree of `Join` nodes (any kind) whose `on` predicates and the `Filter`s directly over or
between them are pooled, stopped at any node that is not a join or a filter (an `Aggregate`, a `Distinct`, a
`Limit`, a per-row fallback), at a volatile operator, and at a fence (§2.3). Its **leaves** are the inputs that
are not part of it; each leaf is its own memo group, optimised separately (and may contain regions of its own).

### 4.5 The hypergraph and DPhyp

Each leaf is a node. Each conjunct is an edge whose two sides are its **syntactic eligibility sets** (the leaves
it reads, split by the join that introduced it) widened to its TES (§4.6). A conjunct reading one leaf is not an
edge: it is pushed into the leaf (§5.1). A conjunct reading no leaf (a constant, a parameter) filters the region's
first evaluated leaf as `predpush` does. A region that is not connected (a cross join) is connected with
cross-product edges between components, ranked last by cost, so DPhyp still finds the plan.

DPhyp (`opt_dphyp.sysl`) runs as in Moerkotte & Neumann 2008, `EmitCsg`/`EnumerateCsgRec`/`EmitCsgCmp`
over bit sets of up to 64 leaves (larger regions go to §3.2's fallback directly). For each csg-cmp pair
`(S1, S2)` it asks the conflict detector which operators may join them and in which orientation, adds the group for
`S1 ∪ S2` if new, and adds the join expression(s). Each expression's `on` is the set of edges whose TES lies
in `S1 ∪ S2` and crosses `(S1, S2)`.

### 4.6 Non-inner joins: CD-C with TES

Outer, semi, anti, mark and single joins are reordered with **CD-C** of Moerkotte, Fender & Eich (SIGMOD 2013):
each join operator `o` gets a TES, initially the leaves its predicate reads, extended by **conflict rules**
`(T1 → T2)` computed from the associativity, left-asscom and right-asscom table of the operator pairs below it
(their Tables 2–3); a pair `(S1, S2)` is joinable by `o` only when `TES(o)` is covered by the pair the right way
round and every conflict rule whose left side `S1 ∪ S2` meets has its right side inside. CD-C is complete
(finds every valid plan of the core search space) and correct for `inner`, `left`, `full`, `semi`, `anti` and
group joins. TernDB maps its kinds onto the paper's:

| TernDB kind | As | Note |
|---|---|---|
| `inner` | inner join | |
| `left`, `right` | left outerjoin (right as left, commuted) | null-rejection (§5.1) turns them inner first |
| `full` | full outerjoin | |
| `semi` | left semijoin | |
| `anti` | left antijoin | |
| `mark` | left semijoin for reordering (its left rows all survive, like an outerjoin's preserved side), with the mark column produced by it | the mark is read only above; any operator reading it is above the join in every plan |
| `single` | left outerjoin with an extra conflict rule pinning its left side (§2.3) | after N4, a unique right side has made it `left` |

TernDB's own error rules (§2.3) are added as **further conflict rules**: an unsafe `on` adds the rule
"`left(o) ∪ right(o)` exactly", a join that may move a `may_raise` input to a right-hand side is offered only in
the orientation that does not, and a guarded join is offered commuted only when its left input is safe. The
detector's unit tests (H1) are the paper's examples, each TernDB kind pair, and each error rule.

### 4.7 Size limits

The enumerator counts csg-cmp pairs as it goes; a region past the budget is abandoned and re-enumerated by the
next scheme of §3.2. A region of more than 64 leaves goes to GOO directly. Each scheme fills the same memo groups,
so the rest of the search does not know which one ran.

### 4.8 The physical plan

`PhysPlan` (`opt_phys.sysl`) is the extracted tree: each node an operator from §3.3 with its fields resolved to
what the executor needs (key expressions, residuals, access ranges as `executor.md` §4.6 encodes them), its
`group`, `est_rows`, `est_cost` (the cost vector of §8.1), the logical binder order of §4.3, and its provided
ordering. The `Planner`'s new entry, `lower_phys`, builds the operators from it.

## 5. Rewrites: normalisation, exploration and unnesting

### 5.1 Normalisation (applied once, as the memo is built)

Rewrites that are always beneficial, applied bottom-up as each group is created, as CockroachDB's `norm` rules
are, so the memo never holds the unnormalised form:

- **N1 Conjunct canonicalisation**: flatten `and`s, sort conjuncts by a stable key, deduplicate, fold constants
  in safe expressions (`1 = 0` is `false`, `x = x` on a non-null `x` is `true`), detect contradictions on one
  column (`a = 1 and a = 2`, `a > 5 and a < 3`: the group is empty, cardinality 0, and its plan is an empty
  `Values` — but only when every input dropped is not `may_raise`, else the scan stays and the filter runs).
- **N2 Predicate pushdown**, `predpush`'s rules (§2.3), with the pushed conjunct also handed to the scan's
  access-path selection (§9.1).
- **N3 Outer join simplification**: a `left`/`right`/`full` join whose null-supplying side a safe conjunct above
  rejects becomes `inner`/`left`/`right` (`predpush`'s test, which reads the null-rejection from the
  conjunct's bare column operand).
- **N4 `single` to `left`** when the right side is unique on the join keys (§2.3).
- **N5 Projection pruning**: each group computes the columns its parents read; a scan is planned with exactly
  those (what `rowscan`'s column list needs), and a `Project` item nobody reads is dropped unless it is volatile
  or may raise.
- **N6 Redundant operators**: `Distinct` over an input with a unique key; `Sort` under an operator that ignores
  order (a bag's `Aggregate`, `Distinct`, a join input); `Limit` of `count` ≥ a known maximum cardinality.
- **N7 Transitive predicates**: `a.x = b.y and b.y = 5` adds `a.x = 5` (equivalence classes over `eq`/`same`
  conjuncts of one region), safe since every term is a comparison.
- **N8 Join elimination**: a `left` join to a side unique on its keys that nothing above reads, and an inner join
  along a validated foreign key to a parent nothing reads, are dropped (Postgres and SQL Server do both). Both
  need the side dropped to be not `may_raise`.

Retiring `push_query`: N2 and N3 reproduce `predpush`'s rewrite; item N3 runs both side by side in a
differential test over `predpush`'s generator until they agree, then removes the call from `unnest_plan`.

### 5.2 Exploration rules

- **Join order**: §4.5, not rules.
- **E1 Eager aggregation** (Yan & Larson 1995; Eich & Moerkotte's "group join"): an `Aggregate` over a join may
  be split into a partial aggregate below the join on the side its arguments read, grouped by that side's join
  keys and grouping keys, and a final one above. Legal for decomposable aggregates (`count`, `sum`, `min`,
  `max`, `bool_and`, `bool_or`); `sum` keeps the `i256` accumulator in the partial state and checks the result
  type's bound only in the final aggregate, so `R0001` is raised exactly when the written plan raises it
  (`executor.md` §4 Aggregate). A partial aggregate needs its input not `may_raise` when it changes how many
  times an argument is evaluated, which it does not (each input row is aggregated once either way), so the only
  condition is decomposability and no `distinct`, `order` or `filter` that reads the other side.
- **E2 Group join**: an `Aggregate` grouped by a join's build-side key over an equi-join becomes one hash table
  doing both (Moerkotte & Neumann 2011) — an executor operator (P10), costed when built.
- **E3 Semi join as inner join plus distinct**: `semi(L, R)` = `Project_L(L ⋈ Distinct(R by keys))` when `R` is
  small and `L` large, letting `R` drive an index nested-loop into `L` (the "semi-join reduction" Neumann's
  unnesting paper lists). Legal when `L` is not `may_raise` (it may then be read only at matching keys).
- **E4 Limit pushdown** through `left` joins (a `Limit` of `n` over `left(L, R)` may limit `L` to `n` first, when
  nothing between reads order the limit does not keep) and into `UNION ALL` inputs.
- **E5 Distinct pushdown** below joins that preserve uniqueness, and **Distinct pull-up** over `semi` joins.
- **E6 Aggregate pushdown into `UNION ALL`** inputs (partial aggregates per input).
- **E7 OR to UNION**: `a = 1 or b = 2` on one table as the union of two index ranges (§9.1's `IndexUnion`) —
  costed against the full scan.

### 5.3 Unnesting in the memo

`unnesting.md`'s rewrite remains the source of truth for meaning and stays where it is: it produces a tree the
memo starts from. The optimizer then costs the alternatives that tree admits:

1. **Domain elimination** (Neumann & Kemper 2015 §3.3; Neumann's "Improving Unnesting of Complex Queries", BTW
   2025): where every domain column is equated (`same`) with a column of the correlated side `R'`, the domain
   join can read the bindings from `R'` itself, and the `Distinct(Project(copy of L))` disappears. A rule
   (U1) adds the eliminated form as an alternative; it is cheaper whenever `L` is costly to copy.
2. **The domain's copy of `L` versus a shared subplan**: `unnesting.md` §4 copies the outer side once per
   domain because the executor runs no shared subplans. When `With` is executed by the executor (a separate
   executor item, not this note's), the domain becomes a `CteRef` of the materialised outer side and the memo
   costs materialisation against recomputation (U2).
3. **The "nested" alternative is an index nested-loop join**, not a per-row `Apply`. `query-tree.md` §6.6 rule 2
   forbids executing an `Apply` per row where unnesting can remove it, so the optimizer never chooses the per-row
   form; what the per-row form does well (one outer row, an index on the correlated key) is exactly what an index
   nested-loop join from the domain into `R` does, batched (§9.3). The memo therefore has both the hash join of
   `D` with `R'` and the index nested-loop from `D` into `R`'s index, and costs them. An index
   nested-loop join does not count as "an operator that re-evaluates a subtree once per outer row" under §6.6
   rule 2 (decided, Q3 of §16; item S3 writes the clarification): the rule is about `Apply` and arbitrary
   subtrees, not about a probe of one access path, which `isolation.md` §4.4 already lists among reads ("an index-nested-loop probe
   logs each probe key").
4. **Mark joins**: `Exists` and `In` become mark joins whose `on` is keys alone (`unnesting.md` §4); a `Filter`
   reading only the mark (`where exists (…)`) is normalised into a `semi` join (or `anti` for `not exists`, and
   for `not in` when the keys are not nullable on either side), which reorders more freely (§4.6).

## 6. Statistics

### 6.1 The plan's position, and how this design meets it

The plan's Decided list says statistics are "kept current incrementally rather than by a periodic `ANALYZE`,
plus multi-column correlation sketches". This design keeps that as the primary mechanism and adds the explicit
`ANALYZE` the user asks for as the way to rebuild what cannot be maintained incrementally:

- **Maintained incrementally, at commit**: row and slot counts, null counts, min/max (widening only), a per-column
  HyperLogLog distinct-count sketch (insert-only), per-index entry counts, the modification counters, and a
  per-table **backing sample** (Gibbons, Matias & Poosala, "Fast incremental maintenance of approximate
  histograms", VLDB 1997) from which histograms and most-common values are recomputed without reading the table.
- **Rebuilt by `ANALYZE`** (explicit, or automatic past a threshold, §6.6): the sketches insert-only maintenance
  degrades (HLL after many deletes, min/max after the extremes are deleted), the backing sample when it has
  shrunk below its floor, the correlation, and the extended statistics.

### 6.2 What is kept

Per **table** (`TableStats`):

| Field | Meaning | Maintained |
|---|---|---|
| `rows` | visible committed rows | at commit, ± the write set |
| `slots` | leaf entries, dead versions included (what a full scan visits, `slots_read`) | at commit and at purge |
| `mods` | rows inserted, updated or deleted since the last analyze | at commit |
| `pages` | leaf pages of the table's tree, and per index | at split/merge |
| `avg_row_width` | average encoded row width | from the backing sample |
| `analyzed_at`, `rows_at_analyze`, `epoch` | when the histograms were last rebuilt, how many rows then, a counter bumped by each rebuild | by `ANALYZE` and the sample recompute |

Per **column** (`ColumnStats`), keyed by `(table oid, column id, type epoch)` — the column id survives renames
and reorderings (`ddl.md` §5), and the type epoch changes at each retype (§6.8):

| Field | Meaning | Maintained |
|---|---|---|
| `nulls` | null count | at commit |
| `ndv` | HyperLogLog sketch, precision 12 (4,096 six-bit registers, 3 KiB; standard error 1.6%), with HLL++'s bias correction and linear counting for small cardinalities | inserted values at commit; rebuilt by `ANALYZE` |
| `min`, `max` | extremes, in the total order (`query-tree.md` §3.3) | widened at commit; narrowed by `ANALYZE` |
| `mcv` | up to `stats_target` (default 100) most common values with frequencies | from the backing sample |
| `histogram` | equi-depth, `stats_target` buckets over the values not in `mcv`, bounds as values | from the backing sample |
| `avg_width` | average encoded width (for hash-table memory and decode cost) | from the sample |
| `correlation` | Spearman correlation between the column's order and the primary key's order (§6.4) | from the sample |
| `type_class` | fixed-width scalar, variable-width scalar, boxed (record, union, relation, `json`) — selects the cost weights of §8 | from the type |

For a **record** or **union** column, the same statistics per field path (`a.b.c`) the queries read, and a
union's tag frequencies (§6.9). For a **relation** column, the nested statistics of §6.9.

Per **index**: entry count (stale entries included, since a range visits them, `executor.md` §4.6), distinct
rows, leaf pages, `unindexed` count, and the multi-column NDV of each leading prefix of its components (§6.5).
An expression index's components get `ColumnStats` of their own (Postgres does the same), computed by the
index build job that already evaluates them (§6.7).

### 6.3 The sketch and its hash

The HLL hashes each value with `value_hash.sysl` (`hashjoin`): rapidhash (`sysl.checksum.hash64`) over the
value's canonical words, so that values that are `same` (§3.1) hash alike across integer widths and decimal
scales — the NDV counts exactly the classes a hash join or a `Distinct` sees. It uses the fixed seed, never the
`Work.key_hash()` a test narrows. `stats_hll.sysl` implements add, merge (register max), estimate, serialise.
Its tests check the estimate against an exact count over random and adversarial inputs (all equal, all
distinct, 1, 2⁶, 2¹⁶ and 2²⁰ distinct values, every integer width of the same numbers) within three standard
errors.

### 6.4 Incremental maintenance under MVCC

A transaction keeps a **local delta** (`stats_live.sysl`) beside its write set: rows ± per table, null counts,
min/max widening, an HLL of the values it wrote per column, and its additions to and removals from the backing
sample. At commit the delta is merged into the table's live statistics (register max for the HLL, sums for the
counts); at abort, or a rollback to a savepoint (whose delta is a mark in the transaction's delta log, as its
writes are in `store_savepoint.sysl`), it is discarded. So the committed statistics count only committed changes,
and a transaction's own planning sees its own writes: the estimator reads committed statistics plus the
transaction's own delta. A delete or an update's old value lowers counts but cannot lower an HLL; deleted
values stay counted until the next rebuild, which the staleness rule (§6.6) bounds.

The **backing sample** is a reservoir of up to `30 × stats_target` rows' column values (3,000 by default; Postgres
reads 300 × the target) per table. An insert enters it with the reservoir probability `k / rows`; a delete or an
update of a sampled row removes or replaces it. When deletions shrink it below half its size, the table is
marked for a rebuild (§6.6). Histograms and MCVs are recomputed from it at commit when `mods` since the last
recompute exceed 10% of `rows` — an in-memory computation over at most 3,000 values per column, done after the
commit has returned, off the writer's path.

**Correlation** (Postgres's `pg_stats.correlation`): the Spearman rank correlation between the sampled values'
order and their rows' primary-key order. A secondary index's fetches land on fewer distinct pages when it is
high (§8.4). It is recomputed with the histograms.

### 6.5 Multi-column statistics

- **Automatic, for every index and the primary key**: the NDV of each leading prefix of its components (as SQL
  Server keeps "density" vectors). For an index of `(a, b, c)`: `ndv(a)`, `ndv(a, b)`, `ndv(a, b, c)`, each an
  HLL over the hash of the prefix's values combined. Join and group-by estimates on those columns read them
  (§7.4, §7.6), which removes the commonest independence error at no cost a write does not already pay.
- **Declared**, as Postgres's extended statistics: `CREATE STATISTICS s (ndistinct, dependencies, mcv) ON (a, b,
  c) FROM t`, a catalog object with a `QualName` (`ddl.md` §2, new §2.14):
  - `ndistinct`: an HLL per column combination of size ≥ 2 (at most 8 columns, so at most 247 combinations);
  - `dependencies`: the degree of each functional dependency `a → b` among the columns (Postgres's `d(a→b)`,
    the fraction of sample rows consistent with it), from the sample;
  - `mcv`: a multi-column MCV list over the combination, from the sample.
  Expressions are allowed as members (as in Postgres 14+). TernDB also creates such objects automatically for
  column groups the feedback of §11.3 finds misestimated, as Oracle's "column group usage" does, behind a
  setting off by default (decided, Q5 of §16).

### 6.6 ANALYZE, auto-analyze and staleness

`ANALYZE [table [(columns)]]` (a new statement, `ddl.md` §3.2 and `sql.md`, item S2) runs as a **job**
(`ddl.md` §7): a read-committed internal transaction at a fresh snapshot reads the table once, through
`rowscan`'s `scan_vectors` decoding only the analysed columns, and builds every statistic exactly — HLL over
every value (not a sample: in memory the full pass is cheap, and HLL needs no sample), min/max, null counts, the
backing sample drawn anew by reservoir, histograms, MCVs, correlation, the prefix NDVs and the declared extended
statistics. On disk, once the buffer manager lands, a table above `analyze_full_limit` (default 1 GiB) is
sampled instead: the backing sample by B+tree random-walk sampling (Olken & Rotem 1989: descend from the root
taking a random child weighted by its fan-out, rejecting to correct the bias), and NDV estimated from the
sample with the GEE/Haas–Stokes estimator unless an up-to-date incremental HLL exists. `ANALYZE` reads at a
snapshot like any job, takes no locks, logs no predicate (its reads decide no result) and commits its statistics
as one versioned write (§6.7).

**Auto-analyze** queues the job for a table when `mods > auto_analyze_base + auto_analyze_scale × rows_at_analyze`
(defaults 50 and 0.1, Postgres's), when deletes since the last rebuild exceed 20% of `rows` (the HLL and min/max
cannot shrink otherwise), when the backing sample has fallen below half its size, and when the feedback of §11.3
records a q-error above 100 on a single-table estimate of the table. Jobs run below user work (`ddl.md` §7
rule 7).

**Staleness** is a number the estimator reads, not a flag: `stale = mods / max(rows_at_analyze, 1)`. Row counts
are always current (maintained exactly). Histograms are used as fractions, scaled to the current `rows`. A
range beyond the histogram's last bound — the **ascending-key problem** of timestamps and sequence-generated keys,
whose newest values no histogram has seen — is estimated from the incrementally widened `max`: the rows inserted
since the analyze are assumed spread uniformly over `(hist_max, max]` (SQL Server's 2014 estimator does this),
instead of the zero a stale histogram gives. `EXPLAIN (VERBOSE)` prints each table's staleness.

### 6.7 Where statistics live, and transactional DDL

Statistics are **not part of the catalog's definitions**. Putting them in `CatalogVersions` would make every
statistics update a catalog change: a catalog-row lock (`store.md`, "The versioned catalog"), a new catalog
version that invalidates every cached plan and prepared statement (`ddl.md` §1 rule 3, `wire-protocol.md` §4.4
rule 4), and `schema_changed` for writers whose reads it contradicts. Instead:

- **In memory**, a `StatsStore` beside `CatalogVersions` holds the committed statistics of each object by oid,
  with a commit number per entry. Like a sequence's draw position (`CatalogVersions.draws`), they are shared by
  every catalog version rather than versioned with them, since an estimate need not be snapshot-consistent and
  the newest is the best. A planner reads the newest committed statistics plus its transaction's own delta.
- **On disk**, they are rows of a system table `sys.statistics` (`on-disk-format.md` §3.9, item S1), ordinary
  versioned rows written by the commit that merges a delta (the counters, coalesced: a checkpoint writes the
  merged counters, and recovery adds the deltas of the commits it replays, so they are exact after a crash) and by
  `ANALYZE` (the sketches). Because they are WAL-logged rows, **replicas receive them by replay**, as they
  receive jobs' effects (`ddl.md` §7 rule 8), and plan with the primary's statistics. Shown as
  `catalog.statistics` (`ddl.md` §4.2).
- **The plan cache's key** (`query-tree.md` §10.2 rule 2: shape and catalog version) is unchanged; a cached
  physical plan also records each table's statistics epoch, and is re-planned when one moves past the threshold
  of §10.3.

Under **transactional DDL**:

- A table **created** in a transaction has no committed statistics; the transaction's delta is all there is, and
  it becomes the committed statistics at commit. Rolled back, it never existed.
- A column **added** gets statistics derived from its fill: a constant default is one value (`ndv` 1, `nulls`
  0 or all rows); a volatile default or a generated column is marked unknown until the job filling it, which
  builds its statistics as it writes, commits (`ddl.md` §8.2).
- A column **retyped** (`ddl.md` §8.5) bumps its type epoch: statistics under the old epoch are not read
  (a histogram's bounds and an HLL's hashes are of the old type's values); the rewrite job builds the new ones.
- A column or table **dropped** leaves statistics no catalog version reads; they are removed when the catalog
  version that last had the object is retired (`store.md`, "Ending"), with the rows.
- An **index created** gets its prefix NDVs and expression statistics from its `build_index` job, which visits
  every row anyway; until the index is `ready` the optimizer cannot choose it (`ddl.md` §2.5 rule 5), so no plan
  needs them earlier.
- `CREATE STATISTICS` is DDL (a catalog object), versioned like any; the job it queues computes the
  statistics.

### 6.8 Statistics a transaction's snapshot cannot see

The live statistics count committed rows, which a snapshot transaction may not see (its snapshot is older). The
estimator does not correct for this: a long-running transaction's estimates use newer counts than its reads see,
which is what every MVCC database does, and the error is bounded by the rows committed since its snapshot.

### 6.9 Nested relations, records, unions and json

- **Relation-valued columns** (`query-tree.md` §2.5): per column, the **fan-out** (average, null count, and an
  equi-depth histogram of each row's element count, so `Unnest` and an `exists` over the nested relation
  estimate correctly when most rows have none and a few have thousands), and the element type's statistics
  computed over **all elements of all rows**, as if the nested relation were a child table (its fields' NDV,
  MCVs, histograms; nested relations inside it recursively). `Unnest` reads these (§7.7).
- **Records and unions**: per field path that a query of the table has read (the paths are recorded by the
  planner in the live statistics, so the next rebuild covers them; a path never read is never analysed), the
  field's `ColumnStats`; per union, its tags' frequencies, so `Match` arms and `Tag` filters estimate exactly.
- **`json`**: the core function catalog (`query-tree.md` §7.12) has no accessor into a `json` value yet, so no
  predicate can read inside one and there is nothing to estimate. When one is added, the statistics follow the
  same path scheme: per path a query has read, the path's presence fraction and a `ColumnStats` over its scalar
  values (Postgres keeps none; this follows what SQL Server's JSON-path statistics and DuckDB's `json`
  statistics do). Item S9 builds it then.

## 7. Cardinality estimation

`opt_selectivity.sysl` holds a pure function per predicate form over `ColumnStats`; `opt_card.sysl` combines
them per operator. Each is unit-tested on hand-made statistics with exact expected values, then against real data
through the counters (§14.1).

### 7.1 Single-column predicates

`n` is `rows`, `f_null` the null fraction, `d` the NDV, `mcv` the list with frequencies `f_i`, `Σmcv` their sum,
and `H` the histogram over the non-MCV values, with `f_H = 1 − f_null − Σmcv`.

| Predicate | Selectivity |
|---|---|
| `col = c` (`eq`, `c` a literal or a known parameter) | `f_i` if `c` is the i-th MCV; else `f_H / (d − |mcv|)`, and 0 when `c` lies outside `[min, max]` (`max` from the widened value, §6.6) |
| `same(col, c)` | as `eq`, and `f_null` when `c` is null |
| `col = null`, any comparison with null | 0 (never true, `executor.md` §4.6's `NoRows`) |
| `col ne c` | `1 − f_null − sel(col = c)` |
| `col < c`, `<=`, `>`, `>=`, `BETWEEN` | MCVs summed exactly, plus `f_H ×` the histogram fraction with linear interpolation inside the bucket for numeric, date and time types; text and bytes interpolated on their first 8 octets as an integer (Postgres's `convert_string_to_scalar`); beyond the last bound, §6.6's ascending-key rule |
| `like(col, p)` with a literal prefix `p` before the first wildcard | the range `[p, succ(p))` by the row above, times `0.2^k` for the `k` literal code points after the first wildcard (Postgres's `FIXED_CHAR_SEL`), times the MCV check where `p` has no wildcard; a leading `%`: `0.2^k` over `f_H`, floored at `1/d` |
| `starts_with(col, p)` | the range `[p, succ(p))` |
| `col in (c1 … cm)` (unnested to a mark join over `Values`, or an `or` of `eq`) | the sum over the distinct `c_i` of `sel(col = c_i)`, capped at `1 − f_null` |
| `is_null(col)` | `f_null` |
| `is_not_null(col)` | `1 − f_null` |
| `col1 = col2` (one table) | `(1 − f_null1)(1 − f_null2) / max(d1, d2)` |
| `col1 < col2` (one table) | `1/3` of the non-null fraction |
| `f(col) op c`, an expression | its expression index's statistics, or a declared statistics expression's, if one matches node for node (`matches_def`); else `f`'s **monotonicity** (an increasing cast, `add` of a constant) carries the range to `col`; else the defaults below |
| a boolean column, `col` | `f_true` from the MCVs |
| a parameter in a generic plan | §10.2 |

**Defaults** when nothing is known (no statistics, an opaque expression): equality `1/200`, range `1/3`, `like`
`1/20`, `is_null` `1/200`, any other boolean `1/2` — Postgres's values, so estimates are comparable. Every default
used in a plan is listed by `EXPLAIN (VERBOSE)`, since each is a likely misestimate.

### 7.2 Boolean structure

- **`not p`**: three-valued: `not p` is true where `p` is false, so `sel(not p) = 1 − sel(p) − sel(p is null)`,
  where `sel(p is null)` is the null fraction of `p`'s nullable columns.
- **`p or q`** on one column: the union of their ranges and values, computed exactly on the histogram. Otherwise
  `s_p + s_q − s_p × s_q`.
- **`p and q`**: on one column, the intersection of ranges; otherwise conjuncts are combined in order of
  selectivity, most selective first, by **exponential backoff** (SQL Server 2014): `s1 × s2^(1/2) × s3^(1/4) ×
  s4^(1/8)`, the rest ignored. Where a declared `dependencies` statistic covers two columns, Postgres's formula
  replaces it: `P(a ∧ b) = P(a) × (d + (1 − d) × P(b))` for the degree `d` of `a → b`; where a multi-column MCV
  covers the conjuncts' columns and they are equalities, the MCV's frequency is read directly.
- **Why backoff, not independence**: independence multiplies, and every real benchmark (JOB in particular,
  Leis et al., "How good are query optimizers, really?", VLDB 2015) shows correlated conjuncts underestimated by
  orders of magnitude, which leads to nested-loop disasters. Backoff errs toward more rows; §7.5 bounds the rest.

### 7.3 Grouping and distinct

The NDV of a column set `C` over a group: the multi-column NDV where a prefix or declared statistic has it;
otherwise `min(n, Π d_i)` damped by backoff over the factors (largest first, as §7.2); never more than the input's
rows. **After a filter** of selectivity `s` the NDV of a column is not `d × s`: it is Yao's/Cardenas's expected
number of distinct values left when `s n` of `n` rows are kept, `d × (1 − (1 − s)^(n/d))`. A key column (or a
column set containing a key) keeps NDV equal to the rows. `Aggregate`'s output rows are the NDV of its grouping
keys; with no keys, one. `Distinct` is the NDV of all its columns.

### 7.4 Joins

For an equi-join of `R` and `S` on keys `R.a = S.b`, the textbook estimate (containment of value sets):
`|R ⋈ S| = |R| × |S| × (1 − f_null_a)(1 − f_null_b) / max(d_a, d_b)`. TernDB refines it in order:

1. **MCV matching** (Postgres's `eqjoinsel`): where both sides have MCV lists, the frequencies of the values in
   both are multiplied and summed exactly, and only the remainder uses the formula above over the non-MCV
   values.
2. **Foreign keys**: when `R.a` references `S.b` through a **validated** foreign key (`ddl.md` §2.6), every
   non-null `R.a` matches exactly one `S` row, so `|R ⋈ σ_p(S)| = |R| × (1 − f_null_a) × sel(p)` (Postgres 9.6+
   does the same). This is exact for the commonest join, whatever the NDVs.
3. **Unique keys**: when `S.b` is unique, `|R ⋈ S| ≤ |R|`.
4. **Several key pairs**: the multi-column NDV of each side's key columns where an index prefix or declared
   statistic has it; otherwise the pairs' selectivities combined by backoff (§7.2).
5. **The NDV after filters** (§7.3) is used, not the base table's.

The estimate is a function of the **relation set and the edges inside it**: the product of the leaves'
filtered cardinalities times every edge's selectivity, computed once per memo group (§4.2), which is what makes
every join order of the group agree.

Other kinds: `semi` is `|R| × min(1, d_b / d_a)` over non-null keys (containment), `anti` its complement, plus,
for `not in`'s null handling, 0 when the right side holds a null key; `mark` and `single` keep `|R|` rows, with
the mark's true fraction the semi-join's; `left` is `max(|R ⋈ S|, |R|)`; `full` is `|R ⋈ S|` plus the unmatched
fractions of both sides; a cross join is the product.

### 7.5 Bounds and safeguards

Independence and containment fail on correlated data and skew; the estimator guards against their worst
failures with **upper bounds** that cost nothing to compute:

- **The max-frequency bound** (Hertzschuch et al., "Simplicity done right for join ordering", CIDR 2021, used
  in Umbra; and Cai, Balazinska & Suciu's pessimistic estimation, SIGMOD 2019): `|R ⋈ S| ≤ min(|R| ×
  mf(S.b), |S| × mf(R.a))`, where `mf` is the largest frequency of any one value (the first MCV's count, or
  `rows / d` when there is no MCV list, or 1 for a key). It is chained through a join tree: the bound of
  `(R ⋈ S) ⋈ T` multiplies the bound of `R ⋈ S` by `mf(T.c)`. The estimate is `min(estimate, bound)`.
- **Never below the provable minimum**: a `left` join keeps `|R|`; `mark` and `single` keep `|R|`.
- **Never below one row** unless the group is provably empty (N1's contradictions, an empty table with
  current statistics): a zero estimate makes every plan above it look free.
- **Uncertainty**: every estimate carries a **confidence** — exact (from a key, a foreign key, a current count),
  statistical (from statistics), or default (from §7.1's defaults or a stale table). The cost model (§8.6) and
  the adaptive rules (§11) treat low-confidence estimates with caution: a nested-loop or index nested-loop join
  whose outer estimate is a default is costed at the outer's upper bound, not its estimate (the "robust" choice
  SQL Server's adaptive join and Umbra's pessimism both make).

### 7.6 Other operators

- **Filter**: the predicate's selectivity times the input's rows.
- **Project**: the input's rows; the NDV of a computed item is the NDV of its input column for an injective
  function (a cast widening, `add` of a constant), else `min(rows, Π NDVs of its inputs)`.
- **Limit**: `min(rows, count)` (with `offset` subtracted); `ties` adds the expected rows equal to the last.
- **Window**: the input's rows.
- **SetOp**: `union all` the sum; `union` the NDV of the union (HLL merge where both are scans, else the sum
  capped by backoff); `intersect` at most the smaller; `except` at most the left.
- **Recursive CTE**: the seed's rows times an iteration estimate: the step's selectivity against the working table,
  iterated until it falls below one row or 100 iterations; with no statistics, the seed × 10. A recursive CTE's
  estimate is always of confidence "default" (§7.5).
- **Values** and **ParamScan**: exact (a relation-valued parameter's row count is known at execution;
  `query-tree.md` §10.4), and its column statistics computed from its values when it is over 1,000 rows.

### 7.7 Nested relations

- **`Nest`**: output rows are the NDV of its keys; each row's nested relation holds `input rows / groups`
  elements on average, and the nested column's fan-out statistics are derived from that.
- **`Unnest`**: `rows × fan-out average`, the element statistics from §6.9's flattened statistics (for a stored
  column) or from the `Nest`'s input (for a computed one). A predicate on an unnested field uses the flattened
  statistics, as a predicate on a child table would.
- **`Relation(r)`** (a nested result, unnested to an `Aggregate` of `collect` over `D ⟕ r'`, `unnesting.md` §1):
  the outer rows are `|D|`, and each collected relation holds `|r'| / |D|` elements on average.

## 8. The cost model

### 8.1 Cost is a vector of the real counters

The cost of a plan is the **vector of the counters its execution will produce**, and a scalar cost is that
vector's dot product with calibrated weights. Every component is a counter the executor or store already
increments (or will, from item C2), so every estimate can be checked against an actual run in a test:

| Component | Counter | Source branch |
|---|---|---|
| leaf entries visited by scans | `Store.slots_read` | dev / `secidx` |
| index entries visited | `Store.entries_read` | `secidx` |
| rows fetched by key | `Store.rows_fetched` | `secidx` |
| leaf entries copied out of a page | `ReadWork.copied` | `rowscan` |
| images, `StoredRow`s and values built on the slow path | `ReadWork.images`, `.rows`, `.values` | `rowscan` |
| columns decoded into lanes / skipped | `ReadWork.columns`, `.skipped` | `rowscan` |
| rows built into hash tables | `Work.built` | `hashjoin` |
| rows looked up | `Work.probed` | `hashjoin` |
| key comparisons after agreeing hashes | `Work.compared` | `hashjoin` |
| pairs `on` (or the residual) was tested on | `Work.pairs` | `hashjoin` |
| rows a predicate or item was evaluated on, per expression node | `Work.evals` (new) | C2 |
| rows sorted, and comparisons | `Work.sorted`, `Work.sort_cmps` (new) | C2 |
| rows aggregated, groups formed | `Work.agg_rows`, `Work.groups` (new) | C2 |
| batches handed up | `Work.batches` (new) | C2 |
| bytes held by hash tables and sorts at peak | `Work.peak_bytes` (new) | C2 |
| pages read from the buffer pool, hits and misses (sequential, random) | `Pool.hits`, `Pool.misses_seq`, `Pool.misses_rand` (new, the buffer manager's) | K3 |

Each physical operator has a cost function that predicts its own counters from its inputs' estimated rows and
statistics; a plan's vector is the sum over its operators. The scalar cost is `Σ w_i × c_i` with weights in
nanoseconds per unit, one per counter and, for `evals`, `columns` and `values`, one per type class (§6.2:
fixed scalar, variable-width, boxed, since a record compared through `compare.cmp` costs far more than an
`int64` lane, `executor.md` §3.5).

**Why counters, not a hand-tuned formula.** Postgres's `seq_page_cost`/`cpu_tuple_cost` are weights over
counters it never checks. Here the counters are checked: a test (§14.1) asserts that the predicted vector
matches the actual one component by component within a stated q-error, so a cost-model bug is a failing unit
test, not a slow query found in production. The weights are fitted to timings by a calibration suite (§8.7),
the only part of the model that depends on the machine.

### 8.2 Operator cost functions (the counters each predicts)

- **Full scan** of `T`: `slots_read = slots(T)`; `copied = rows on the fast path`; `columns = rows × |read
  non-key columns|`, `skipped = rows × |unread|`; slow-path `values` for the fraction of rows with versions in
  the undo log or an older schema version (estimated from `mods` since the oldest live snapshot, else 0);
  `batches = ⌈rows / BatchSize⌉`.
- **PK range**: as a full scan, over the range's estimated slots.
- **Index range**: `entries_read = entries in range` (the index's entries per row, stale ones included,
  times the range's rows); `rows_fetched = rows in range + unindexed`; then the rows' columns decoded as for a
  scan.
- **Filter**: `evals` per expression node = input rows (every conjunct is evaluated on every row, §7.12 rule 1,
  except as §6.4 rule 3 allows; the model assumes the executor evaluates all, as it does).
- **Hash join**: `built = non-null-key rows of the build side`, `probed = non-null-key rows of the probe side`,
  `compared = candidate groups found` (≈ output rows of the key join, one comparison per group found plus
  collisions, which a 64-bit hash makes negligible), `pairs = candidate pairs` when there is a residual (0 for a
  `semi`/`anti`/`mark` whose `on` is keys alone), `peak_bytes = built × (row width + 16)`.
- **Nested-loop join**: `pairs = |L| × |R|`; `peak_bytes = |R| × width`.
- **Index nested-loop join**: per outer row, the inner access path's `entries_read`/`rows_fetched` at the
  probe's selectivity, `pairs` for the residual.
- **Sort**: `sorted = n`, `sort_cmps = n log₂ n` (merge sort), times the key's comparison weight by type
  class.
- **TopN**: `sort_cmps = n log₂ k`.
- **Aggregate (sort)**: a sort of the input by the keys plus `agg_rows = n`; **(hash)**: `probed = n`,
  `built = groups`, `agg_rows = n`; **(stream)**: `agg_rows = n` and one key comparison per row.
- **Distinct**: as the matching aggregate with no calls.
- **Window**: a sort by partition and order keys unless provided, plus `agg_rows` per frame row (`executor.md` §4.3's sliding
  frames make it `n`, unbounded frames per partition `n`, others `n × frame`).
- **Exchange** (§12): rows × a per-row transfer weight.

### 8.3 The vectorised executor's per-batch costs

Every operator pays a fixed cost per batch (`w_batch`: the call, the vector headers) and a cost per row per
expression node by type class. The per-batch term is what makes tiny inputs cost more than `rows × per-row`, and
is why a plan with many small intermediate results loses to one with few large ones by more than their row
counts say. Value-at-a-time paths (`executor.md` §3.5: records, unions, relations) are their own type class with
their own weights, measured by the calibration (§8.7).

### 8.4 The buffer manager: cached and uncached pages

In memory mode (`on-disk-format.md` §8.1: every page in a frame, no eviction) the page terms are 0. With the buffer
manager (swizzled pointers, `on-disk-format.md` §3, being built on `bufmgr`), each tree reports its **resident
fraction** `ρ` (frames held over pages, kept by the buffer manager per tree as it loads and evicts, item K3).
Then:

- a scan of `P` leaf pages misses `(1 − ρ) × P` pages, read sequentially (`misses_seq`);
- an index range fetching `k` rows by key from a table of `P` pages touches `P × (1 − (1 − 1/P)^k)` distinct
  pages (Cardenas) when the index's order is uncorrelated with the key's, and `k / rows_per_page` when it is
  perfectly correlated; Postgres's interpolation by `correlation²` (§6.4) between the two; misses are `(1 − ρ)`
  of them, random (`misses_rand`), and when the pages touched exceed the pool's free frames, the Mackert–Lohman
  formula replaces Cardenas;
- an index nested-loop join's repeated probes into the same pages are hits after the first, by the same formula
  over all probes.

### 8.5 Memory

Each statement has a memory budget `work_mem` (a setting, default 64 MiB, shared by its blocking operators by
their estimated sizes). A hash join whose build side's `peak_bytes` exceeds its share is costed as a **grace
hash join**: both sides partitioned to temporary pages and each partition joined, adding `2 × (|L| + |R|) ×
width` bytes of temporary writes and reads; a sort over its share is costed as an external merge sort, a hash
aggregate as partitioned. The executor does not spill yet: until the spill items (P12) land, a plan whose
estimate exceeds the budget is costed as above and executed in memory, and `EXPLAIN` marks the operator "would
spill". The plan choice already avoids the spill where an alternative (the other build side, an index
nested-loop) is cheaper.

### 8.6 Robustness

A plan's cost is its expected cost, except that an operator whose cost grows super-linearly with an input's rows
(a nested-loop join's `pairs`, an index nested-loop's probes) is costed at that input's **upper bound** (§7.5)
when the estimate's confidence is "default". A hash join degrades gracefully when wrong; a nested loop does not,
and this asymmetry is what Leis et al. found to matter most.

### 8.7 Calibration

`bench/costcal/` (item K2) runs a suite of micro-plans, each isolating one or two counters — scans of `n` rows
of `k` columns of each type class, index ranges of `n` entries, hash builds and probes of `n` rows, `n` pairs of a
residual, sorts of `n` keys, aggregates of `n` rows into `g` groups — over a grid of `n`, times each under
`marshal run --exclusive --quiet-load auto`, and fits the weights by non-negative least squares over all runs. It
writes `sh/sysl/terndb/opt_weights.sysl`, a checked-in table of constants with the machine and the date in its
comment. A held-out set of whole queries (TPC-H's at a small scale factor) checks that the fitted model's
predicted time is within a factor of 2 of the measured one for every query, and ranks pairs of plans for the
same query in the measured order in at least 90% of pairs. The calibration is run by a person, never by the
gate: it is a timing run. The gate's tests use the checked-in weights and never time anything.

## 9. Physical choices

### 9.1 Access paths, by cost

`access_of`'s fixed rule (`secidx` §4.6, "The choice") is replaced by `opt_access.sysl`, which **enumerates**
every candidate and costs each:

- the full scan;
- the PK range or lookup the filter's conjuncts allow;
- every `ready` index's range (partial indexes only when their predicate matches, expression components only by
  `matches_def`, both exactly as `secidx` decides);
- `IndexIntersect` of two or more index ranges (the rows' primary keys from each, intersected before any fetch)
  and `IndexUnion` for an `or` of sargable conjuncts on different indexes (E7). These are TernDB's form of
  bitmap index ANDing and ORing: the store already fetches an index range's rows in key order once each
  (`executor.md` §4.6), so a sorted key list is the bitmap, and intersecting two is a merge. Worth having because
  it is cheap to build on what `secidx` does and it serves the commonest multi-predicate case without a
  composite index; costed per range as `entries_read`, the fetch as `rows_fetched` of the intersection.

Rule 1 (a comparison with a null reads nothing) stays a rule, since it is a fact, not a cost. Everything
`secidx` guarantees — the filter evaluated over every row read, the logged predicate unchanged, a failing operand
left to the filter, the result in key order — holds for every path, because each path is still a range plus the
whole filter. The test of the replacement is that each of `secidx`'s `exec_access_index_tests.sysl` cases picks
the path it picked before when the statistics make that path cheapest, and the cheaper one when they do not.

**Covering (index-only) scans** are not possible on TernDB's indexes as they are: an entry is never removed
(`store.md`), so whether the row still holds the value can only be learned by fetching it. An index-only scan
needs either versioned entries (each entry carrying its creating and deleting commit, as the on-disk §4.9 entries
partly do) or a per-page all-visible map that purge maintains (Postgres). Both are storage work beyond this note;
the choice between them is deferred until the on-disk secondary indexes are built (decided, Q8 of §16; item
P11), and the optimizer's access-path enumeration has the slot
for it. A PK scan is already covering: the table is clustered on its key.

### 9.2 Hash join build side: plan time, with a run-time guard

`hashjoin` decides the build side at run time by reading both sides until the smaller is known; this costs a
read-ahead buffer of the left side up to the right's size and makes the output order of the join unknown at plan
time. With estimates the optimizer chooses at plan time:

- **Build on the side with the smaller estimated `peak_bytes`**, as a property of the physical operator
  (`HashJoin(build = left | right)`), so the probe side's order is provided (§3.3) and a guarded join keeps its
  right side read first (§2.2): a guarded join built on the left still drains the right first, stops if it is
  empty, and only then builds the left.
- **A run-time guard where the estimate is weak**: when the two sides' estimates are within a factor of 4, or
  either is of "default" confidence (§7.5), the operator keeps `hashjoin`'s run-time choice (role reversal), and
  its provided order is "none". Elsewhere the read-ahead is skipped.
- **§6.5 rule 10 and the build side**: whichever side is built, the right input is read first, as `hashjoin` does
  now. An empty left input never lets the operator skip its right, since rule 10 does not allow it (§2.1).

### 9.3 Index nested-loop join

A new executor operator, `IndexJoinOp` (item P3): for each outer batch, the probe keys are evaluated, sorted and
deduplicated, and the inner table's PK or a `ready` index is read for each distinct key, batched — a sorted
key list probing a B+tree is one descent per key with neighbouring keys found on the same leaf, so the probes of a
batch cost little more than one range scan over them. The inner side is exactly a scan with an access path; its
remaining filter and the join's residual run on the pairs. Kinds: `inner`, `left`, `semi`, `anti`, `mark`,
`single`. Logged per probe key or as the inner's restriction (`isolation.md` §4.4, already written). Chosen when
the outer is small against the inner (the domain of a correlated subquery over a few outer rows is the defining
case, §5.3). Its legality: the inner side is evaluated only at the probe keys, so it must be a bare scan with
safe filters, which is the only shape it accepts.

### 9.4 Aggregation and distinct

- **Hash aggregation** (P4), now possible since `hashjoin` brought a fast canonical hash: streams its input,
  needs no sort, costs `probed = n`. Its output is unordered.
- **Sort aggregation** (the present operator) where the output must be ordered by the keys anyway.
- **Stream aggregation** (P5) where the input already provides the keys' order (a PK prefix, a sort below for
  another reason): no table, no sort, one comparison per row.
- The same three for `Distinct`. All three give the same groups (each group is a class of `same` rows,
  `executor.md` §4 Aggregate), and `sum`'s once-per-group bound check makes `R0001` independent of the algorithm.
- **Order of rows within a group**: an aggregate call with `order` (or `string_agg`, `collect` into a list)
  sorts its group itself (`executor.md` §4), so the algorithm does not change it.

### 9.5 Interesting orders, now that the merge join is gone

The merge join was the main consumer of orders; what is left still pays: an `ORDER BY` on a PK prefix (no sort),
`ORDER BY … LIMIT k` on a PK prefix (no sort, and the scan stops after `k` rows: `isolation.md` §4.4's "scan that
stops early"), stream aggregation and stream distinct over PK order, a `Window` partitioned by a PK prefix, list
results whose order a PK scan already gives, and the probe-side order a hash join or index nested-loop join keeps.
They are carried as required and provided properties (§3.3), with the equivalences and constants of the
predicates reducing them. A secondary index provides its own order only through `OrderedIndexScan` (P9), which
fetches rows in entry order rather than key order, at the cost of random fetches; whether it is worth building is
for the benchmarks to show (it serves `ORDER BY indexed_col LIMIT k`).

### 9.6 Top-N, limits and early termination

`Sort` then `Limit` becomes `TopN` (a heap of `count + offset` rows, P6; stable, so a list's order among equal keys
is kept by a sequence tie-breaker), unless the input provides the order, in which case `Limit` alone stops the
pipeline early. `ties` keeps the heap's boundary group.

### 9.7 Semi and anti joins

Each is a hash join (keys alone test no pair), an index nested-loop join (one probe per distinct outer key,
stopping at the first match), or a nested loop (non-equi `on`). The optimizer also costs E3's inversion. A `not
in` whose keys may be null keeps `hashjoin`'s null-mark machinery, which the cost model prices as one extra lookup
per null-key mask.

## 10. Parameters, plan caching and plan stability

### 10.1 What the cache holds

Today the plan cache holds validated trees and every execution plans anew with its parameter values (`custom`
plans always). That is correct but pays the optimizer on every execution. The cache (`engine.sysl`) is extended
to hold, per (role, shape digest, catalog version) — the key `query-tree.md` §10.2 rule 2 requires — the
statement's **memo after exploration** (optional, bounded) and its **generic plan** with its **validity
ranges**, plus up to three custom plans by parameter bucket.

### 10.2 Generic plans, custom plans and parameter sensitivity

- A **generic plan** is optimised with each parameter's selectivity taken from statistics without its value:
  equality `1/d` (or the non-MCV average), range `1/3`, `like` its default.
- While optimising, the optimizer records for each parameter that reaches a selectivity estimate the **range of
  selectivities over which the chosen plan stays the winner** (parametric optimisation in its cheap form, as
  Oracle's adaptive cursor sharing and SQL Server 2022's parameter-sensitive plans do): at each plan choice the
  parameter influenced, the cost crossover with the runner-up gives a bound.
- At execution, each parameter's actual selectivity is computed from the statistics (an MCV or histogram lookup,
  microseconds). Inside every validity range: the generic plan runs. Outside: a **custom plan** for that
  parameter's bucket (selectivity in one of `< 0.1%`, `0.1–10%`, `> 10%`) is optimised and cached, at most three
  per statement.
- **Parameter sniffing** in SQL Server's sense — the first execution's values decide the plan for all — is
  avoided by this design: no plan is built from one value and reused for values it was not valid for.
- A statement with no parameters is planned once and cached as its generic plan.
- **Relation-valued parameters** (`query-tree.md` §10.4): their row count is known at execution and enters the
  validity check like a selectivity.

### 10.3 Re-planning and plan stability

The plan says: "a cached plan is re-chosen only when estimates move past a threshold, can be pinned by an
operator, and a slower replacement is reported." So:

- A cached plan records the statistics epoch and row count of each table it read. It is re-planned when a
  table's rows moved by more than 2× or its histograms were rebuilt (epoch changed) and the re-planned plan's
  cost differs by more than 10% from the cached plan's cost re-estimated under the new statistics; otherwise the
  cached plan stays, so small statistics drift never flips a plan.
- **Pinning**: `PIN PLAN FOR <digest>` (an admin statement, item A4) stores the current plan's **outline** — the
  join order, algorithms, build sides, access paths — in `sys.plan_baselines`; the optimizer then restricts the
  memo's choices to the outline (a rule mask plus forced choices) as long as the outline is still valid for the
  catalog (an index it names still exists), and falls back to free optimisation with a warning when it is not.
- **Reporting a slower replacement**: when a re-planned statement's measured execution (the run-time of its first
  `n` executions, kept per cached plan) is more than 1.5× the replaced plan's, it is recorded in
  `catalog.plan_regressions` with both outlines, and with `plan_regression_revert` on, the old plan is reinstated
  as a pinned outline (SQL Server's "automatic plan correction").

## 11. Adaptivity and feedback

### 11.1 Re-optimisation at pipeline breakers

A hash join's build, a sort and a hash aggregate's table are **pipeline breakers**: when one finishes, its exact
row count is known (and, for a hash table, the exact NDV of its keys: the number of groups). If the actual count
differs from the estimate by a q-error above 32 (a setting), and the part of the plan above has not started
producing rows, the executor stops and asks the optimizer to re-plan **the rest**: the finished operator becomes
a leaf of known cardinality and key NDV, and the memo is re-optimised above it (Kabra & DeWitt 1998; Neumann's
"progressive" re-optimisation in HyPer/Umbra). The re-plan is legal because it is another plan of the same
statement over the same inputs, and the materialised leaf is evaluated exactly once, as before: no row is read
twice, no error raised twice. It is costed: re-planning is done only when the re-optimised remainder's estimated
saving exceeds the optimisation time. At most two re-optimisations per statement.

### 11.2 Adaptive join

An index nested-loop join whose outer estimate was of low confidence is emitted as an **adaptive join** (SQL
Server 2017): the outer side is buffered up to a threshold row count `t` (the crossover where the hash join
becomes cheaper); below it the probes run; above it, a hash table is built on the inner instead. The threshold
is computed by the optimizer from the two alternatives' costs. Both branches read the inner side's rows through
the same filter, so the two yield the same result and raise the same errors.

### 11.3 Feedback

`EXPLAIN ANALYZE`'s per-operator actual counts (§13) are also recorded, for every execution when
`stats_feedback` is on (default on, sampled 1 in 16 executions), in a bounded **feedback store** keyed by the
memo group's **logical signature** (the relation set, the canonical predicate set, the grouping keys — not the
plan): the actual rows of every operator whose q-error exceeded 4. The estimator consults it first for a group of
the same signature (LEO, Stillger et al. 2001, in its simplest form: an observed count overrides an estimate, as
long as the statistics epoch of every table involved is unchanged). It also triggers auto-analyze (§6.6) and, with the
setting of §6.5 on, creates extended statistics.

## 12. What parallel execution needs from the optimizer

The store is single-threaded today (`store.md`, "Concurrency"); a shared `&sync` store is its next slice. The
optimizer is designed so that parallel plans are a property and an enforcer, not a second optimizer:

- **Model**: morsel-driven parallelism (Leis et al. 2014, HyPer and DuckDB): a pipeline's source is cut into
  morsels — ranges of a B+tree's keys at an inner level, so each is a key range of roughly equal leaf pages — and
  worker threads take morsels until none are left; pipeline breakers (hash builds, aggregates, sorts) merge their
  workers' partial states.
- **Partitioning property**: `any` (each worker's own rows), `hash(keys)`, `range(keys)` (from PK morsels), or
  `singleton`. A hash join needs its build side's table shared (one table, built by all workers, the default in
  morsel-driven designs) or both sides `hash(keys)` partitioned (partitioned hash join, chosen when the table
  exceeds a cache size); a hash aggregate needs a final merge by `hash(keys)`; a `Sort` a final merge; a list
  result or a `Limit` `singleton`. `Exchange` (repartition, broadcast, gather, merge-gather preserving order) is
  the enforcer.
- **Degree of parallelism** is chosen per pipeline: `dop = min(max_workers, ⌈work / morsel_target⌉)` where
  `work` is the pipeline's estimated cost; under `parallel_min_cost` (default ~1 ms of estimated work) the
  pipeline runs on one thread. The cost of a parallel pipeline is `work / dop + dop × w_worker_start + merge`,
  and its memory is per worker.
- **Determinism**: a list result's order is kept by merge-gathering; errors under parallelism follow §6.5 rule 9
  (which error is first is unspecified), and every legality rule of §2 holds per morsel because each worker runs
  the same plan over a subset of the same rows.
- **What the optimizer must expose now**, so that this is added without rework: the partitioning slot in the
  required/provided properties (always `singleton` until P13), the `Exchange` operator in `PhysPlan`, and a
  `dop` field per pipeline in `EXPLAIN`.

## 13. EXPLAIN and EXPLAIN ANALYZE

### 13.1 The statement

`query-tree.md` §15 says `EXPLAIN` prints the validated tree, then the physical plan, then the shape digest, but
the tree has no `Explain` statement and the SQL dialect lists `EXPLAIN` as not spelled yet (`sql.md`). Item S0
adds, in the spec first: a statement kind `Explain { statement, analyze: bool, verbose: bool }` (`query-tree.md`
§5, §12.5), and `EXPLAIN [ANALYZE] [VERBOSE] <statement>` to `sql.md`. `EXPLAIN ANALYZE` runs the statement
(a DML one's writes take effect, in its transaction, as in Postgres) and discards its result rows.

### 13.2 The result

The result is an ordinary relation, so every client reads it with the one decoder that serves the C API and the
wire alike:

```
{ tree: text,                          -- the validated tree, §15 text form
  digest: bytes,                       -- the shape digest (§10.2 rule 3)
  plan: list<{ id: int32, parent: int32?, depth: int32,
               op: text,                -- HashJoin, IndexScan, …
               detail: text,            -- the operator's arguments, below
               est_rows: float64, est_cost: float64,
               confidence: text,        -- exact | statistical | default
               actual: { rows: int64, batches: int64, loops: int64,
                         time_us: int64?, q_error: float64,
                         counters: list<{name: text, est: float64, actual: int64}> }? }>,
  notes: list<text> }                  -- budget cut, defaults used, stale tables, would-spill, re-optimised
```

`actual` is null under plain `EXPLAIN`. `time_us` is null unless timing is asked for (`EXPLAIN (ANALYZE,
TIMING)`), since timing perturbs a vectorised plan little but is not deterministic; every other field is
deterministic for a given plan and data, so tests can compare whole `EXPLAIN ANALYZE` outputs. `q_error` is
`max(est, act) / min(est, act)` with both clamped to at least 1. `counters` lists each component of §8.1 that
the operator's cost predicted, with the predicted and the actual value, the actual attributed to the operator
exclusively (§14.1's attribution).

### 13.3 The text rendering

The shell renders the plan (and tests compare it) as one line per operator, indented two spaces per depth:

```
HashJoin inner build=right keys=[o.user_id = u.id]                         rows=9,980 cost=41.2k
  IndexScan orders o ix=orders_by_user range=[$1, $1] filter=(gt o.total '10'::decimal(9,2))  rows=12 cost=0.31k
  TableScan users u range=[…] cols=[id, name]                              rows=1,000 cost=5.1k
```

and with `ANALYZE` appends `actual=9,412 q=1.06` to each line, and under `VERBOSE` the counters, the rules that
produced the operator, the memo group id and the confidence. Expressions print in the §15 text form. The
`detail` field is exactly the text after the operator name, so the relation and the text agree by construction.

## 14. Testing

Every test below runs in the gate (`sysl test .`) unless it says otherwise; none times anything.

### 14.1 Estimate against actual, through the counters

- **Attribution** (C1): every operator snapshots the shared `Work`, `ReadWork` and the store's counters around
  each `next()` call and keeps the difference, so each operator's counters are inclusive of its inputs' and the
  exclusive figure is the inclusive minus its children's. Rows out, batches and opens (loops) per operator too.
- **Cost functions**: for each operator kind, a test builds a table with exactly known statistics (computed by a
  full `ANALYZE`), runs a plan with that operator, and asserts every predicted counter equals the actual for the
  exactly predictable cases (a full scan's `slots_read`, a hash join's `built`/`probed` on keys with no nulls,
  a sort's `sorted`) and is within a stated q-error for the statistical ones (a range's rows: ≤ 1.5 on uniform
  data, ≤ 3 on Zipf data with MCVs).
- **Selectivity**: each row of §7.1's table with hand-made `ColumnStats` and an exact expected value, plus the
  edge cases: empty table, one row, all null, all equal, a value just outside `[min, max]`, a stale histogram
  with the ascending-key rule, a `like` with every wildcard placement, an `in` list with duplicates and nulls.
- **Joins**: FK joins exact; MCV-matched joins on skewed data within 2; the max-frequency bound never exceeded by
  the actual (a property test over random tables: the bound is a bound).

### 14.2 Rule tests

A text-driven harness (`opt_rule_tests.sysl` over `testdata/opt/*.txt`, embedded as the wire vectors are): each
case gives a catalog, statistics, a query in SQL or the §15 text form, and a command — `memo` (the memo after
normalisation or exploration), `opt` (the extracted plan's text rendering), `rules` (the rules that fired),
`explain` — and the expected output. Each rule has at least: a case where it fires, a case where its guard
refuses (every legality condition of §2.3 it checks, each its own case), and a case where it fires but loses on
cost. This is CockroachDB's `testdata` approach, which keeps a rule's behaviour readable beside the rule.

### 14.3 Every plan gives the same result

- **Random plan extraction** (T1): for a query, extract `k` random complete plans from the explored memo (at each
  group a random physical expression satisfying the required property), run each, and compare results as bags (as
  lists where the result is a list), and their error codes. Run over `predpush`'s random query generator
  (comma, inner, cross and outer joins over random tables with nulls and duplicates), extended with aggregates,
  semi/anti/mark joins from subqueries, limits and sorts.
- **Rule masks** (T2): the same queries optimised with random subsets of the rules disabled must give the same
  results.
- **Against the tree lowering**: every optimised plan's result equals the unoptimised tree lowering's, which is
  the oracle `unnesting.md` already uses.
- The generator avoids expressions that may raise (so plans legitimately differing in which errors they skip do
  not make false failures); errors have their own suite (§14.4).

### 14.4 Error semantics

Queries built to raise on chosen rows — a division by zero on a row a key matches or does not, in an `on`, a
`Filter`, a projected item, a guarded subtree over an empty domain, a `single` with two matches on a row a later
filter removes — each run under every plan the memo offers, asserting the error is raised by every plan where the
rules of §2.1 require it and by none where the written plan raises none.

### 14.5 Plan stability

Golden `EXPLAIN` outputs for a fixed catalog, fixed statistics (loaded, not analysed) and the checked-in weights:
a change to a rule, a selectivity or a weight that changes a plan changes a golden file, and the diff is reviewed
as a plan change. Separately, a test that small statistics drift (±5% rows) does not change a cached plan
(§10.3).

### 14.6 Fuzzing the rules

Beyond T1/T2, a rule fuzzer generates random **memo expressions** that match each rule's pattern (random
children's logical properties within the guard), applies the rule, and checks the produced expressions'
logical properties equal the input's (same output columns and types, same cardinality bounds, same `may_raise`
or a looser one only where §2.3 allows), and runs both on random data. SQLancer's oracles run on random
queries too: **TLP** (a query's result equals the union of its results under `p`, `not p` and `p is null`) and
**NoREC** (a filter's row count equals the count of rows its predicate evaluates `true` on, computed without the
optimizer's help). The fuzzer runs a bounded number of cases in the gate (seeded, so failures reproduce) and an
unbounded run as a marshal job.

### 14.7 The plan-regression suite

`bench/planreg/` (item T5), run as a marshal job, not in the gate:

- **TPC-H** (22 queries) and **TPC-DS** (the 99 queries' shapes, those the dialect can express), with data
  generators written in sysl (deterministic, scale factor 0.01 in a gate smoke test of the first five TPC-H
  queries' plans, 1 and 10 in the suite).
- **JOB** (the Join Order Benchmark, 113 queries over IMDB, Leis et al. 2015), the test of estimation on real
  correlated data. IMDB's data must be downloaded (3.6 GB); the suite reads it from a path given on the command
  line and is skipped when it is absent.
- For every query and every operator, the q-error of the estimate; reported as the distribution (median, 90th,
  95th, max) per benchmark and per operator kind, against the previous run's, as the release benchmarks are
  (every release's benchmark keeps its raw results for the next to compare against). A rise in the 95th percentile, or any query whose plan's
  measured time rises by more than 20%, is a regression treated as a bug.
- **Dialect gaps**: TPC-H and TPC-DS need date arithmetic, `extract`, `substring`, `case`, views, `exists`, `in`
  subqueries, `having`, `rollup` (TPC-DS) and window functions. Item T5 begins by translating every query and
  reporting each one the dialect cannot express, which become SQL items, not workarounds.

## 15. Implementation plan

Items are ordered; each is one agent's deliverable with its tests (every feature unit-tested, and every
item its own edge-case pass: empty and one-element inputs, boundary sizes, duplicates and nulls, interleavings,
every error path). "Needs" lists the items and pending branches that must
have landed first. Items in one phase with no dependency between them can run in parallel.

### 15.1 Summary

| # | Item | Needs |
|---|---|---|
| **Phase 0 — specification** | | |
| S0 | `Explain` statement: `query-tree.md` §5, §12.5, §15 (result type of §13.2), `sql.md`; decode, check, encode, text form; the statement runs the existing lowering and prints the operator tree without estimates | — |
| S1 | `sys.statistics` in `on-disk-format.md` §3.9; `catalog.statistics` in `ddl.md` §4.2 | — |
| S2 | `Analyze` and `CreateStatistics`/`DropStatistics` statements: `ddl.md` §2.14, §3.2, `sql.md`; catalog objects for declared statistics | S1 |
| S3 | `query-tree.md` §6.6 rule 2 clarified to permit index nested-loop probes of one access path (Q3, decided) | — |
| **Phase 1 — counters** | | |
| C1 | Per-operator actuals: rows, batches, loops, and exclusive counter attribution | `hashjoin`, `rowscan`, `secidx` |
| C2 | New counters: `Work.evals` (per type class), `sorted`, `sort_cmps`, `agg_rows`, `groups`, `batches`, `peak_bytes` | C1 |
| **Phase 2 — statistics** | | |
| ST1 | `stats_hll.sysl`: HLL++ (p = 12) over `value_hash`, merge, serialise | `hashjoin` |
| ST2 | `ColumnStats`/`TableStats`, the equi-depth histogram and MCV builders from a sample, correlation | ST1 |
| ST3 | `StatsStore` beside `CatalogVersions`; the transactional-DDL behaviour of §6.7 | ST2, S1 |
| ST4 | Live statistics: transaction deltas merged at commit, discarded at abort and savepoint rollback; the backing sample; min/max widening; recompute of histograms from the sample | ST3 |
| ST5 | `ANALYZE` as a job: full pass through `scan_vectors`; auto-analyze thresholds; staleness | ST4, S2, `rowscan` |
| ST6 | Prefix NDVs for every PK and index; expression-index statistics from `build_index` | ST5, `secidx` |
| ST7 | Declared extended statistics: `ndistinct`, `dependencies`, `mcv` | ST5, S2 |
| ST8 | Nested relation, record-field and union-tag statistics | ST5 |
| ST9 | `json` path statistics | a `json` accessor in `query-tree.md` §7.12 |
| ST10 | On-disk persistence of statistics, replay on replicas, random-walk sampling | ST5, the buffer manager |
| **Phase 3 — the memo** | | |
| M1 | `opt_memo.sysl`: groups, hash-consed expressions, column ids, build from the unnested tree, extraction back to a tree; round-trip test (tree → memo → tree is the identity) | — (best after `predpush`) |
| M2 | `opt_props.sysl`: logical properties (keys, FDs, not-null, `may_raise`, `volatile`, `guarded`, list-ness); `opt_safety.sysl` taking `pd_safe` | M1, `predpush` |
| M3 | Search engine: task stack, required/provided ordering, `Sort` enforcer, winners, branch and bound, budget; rule registry with masks and trace | M2 |
| M4 | `PhysPlan` and `lower_phys`; `run_query` and DML through the optimizer with only implementation rules (one plan per group: today's choices); differential against the tree lowering | M3 |
| X1 | `EXPLAIN` of the physical plan with estimates (§13), the text rendering, golden-test harness | S0, M4 |
| T0 | Rule-test harness (§14.2), random plan extraction (T1 of §14.3) and rule masks (T2) | M4, X1 |
| **Phase 4 — estimation and cost** | | |
| CE1 | `opt_selectivity.sysl`: every row of §7.1 and §7.2 | ST2 |
| CE2 | `opt_card.sysl`: per-operator cardinality and NDV propagation (§7.3, §7.6, §7.7), confidence | CE1, M2 |
| CE3 | Join estimation: MCV matching, FK and unique keys, multi-column NDV, the max-frequency bound (§7.4, §7.5) | CE2, ST6 |
| K1 | `opt_cost.sysl`: the counter vector per operator (§8.2, §8.3), the weights table, lower bounds; estimate-vs-actual tests (§14.1) | CE2, C2 |
| K2 | Calibration suite `bench/costcal/`, the fitted `opt_weights.sysl`, the held-out check | K1 |
| X2 | `EXPLAIN ANALYZE`: actuals, q-error, counters (§13.2) | X1, C1, K1 |
| **Phase 5 — rewrites and join order** | | |
| N1 | Normalisation: conjunct canonicalisation, constants, contradictions (N1 of §5.1), projection pruning (N5), redundant operators (N6) | M3 |
| N2 | Predicate pushdown and outer-join simplification as memo rules (§5.1 N2, N3); differential against `push_query`, then `push_query`'s call removed | N1, `predpush` |
| N3 | `single` to `left` (N4), transitive predicates (N7), join elimination (N8) | N2 |
| H1 | `opt_hypergraph.sysl`: regions, hyperedges, CD-C with TES and conflict rules, TernDB's error rules (§4.6); the paper's examples as tests | N2 |
| H2 | `opt_dphyp.sysl`: DPhyp into the memo, budget counting; logical binder order kept (§4.3) | H1, CE3, K1 |
| H3 | `opt_large.sysl`: linearised DP and GOO fallbacks (§3.2) | H2 |
| U1 | Unnesting alternatives: domain elimination, mark-filter to semi/anti (§5.3) | H2 |
| E1 | Eager aggregation and aggregate pushdown into `UNION ALL` (E1, E6) | H2 |
| E2 | Semi-join inversion, limit and distinct pushdown (E3, E4, E5) | H2 |
| **Phase 6 — physical choices** | | |
| P1 | Access paths by cost (§9.1), replacing `access_of`'s rule | K1, CE1, `secidx` |
| P2 | Plan-time build side with the run-time guard (§9.2); the probe-order test | K1, `hashjoin` |
| P3 | `IndexJoinOp` and its rule (§9.3) | P1, S3 |
| P4 | Hash aggregation and hash distinct operators, and the choice | K1, `hashjoin` |
| P5 | Stream aggregation and distinct; sort elision through provided orders (§9.5) | M3, P4 |
| P6 | `TopN` and early-stopping limits (§9.6) | P5 |
| P7 | `IndexIntersect`/`IndexUnion` (§9.1) and OR-to-union (E7) | P1 |
| P8 | Grace-hash, external-sort and partitioned-aggregate costing; "would spill" (§8.5) | K1 |
| P9 | `OrderedIndexScan` (§9.5), if the benchmarks call for it | P5 |
| P10 | Group join (E2) | P4, E1 |
| P11 | Covering scans: the storage choice deferred by Q8, then the scan | the on-disk secondary indexes |
| P12 | Executor spilling for hash join, sort and hash aggregate (an executor item the costing of P8 anticipates) | P8, the buffer manager |
| **Phase 7 — caching, stability and adaptivity** | | |
| A1 | Generic plans with validity ranges, custom plans by bucket (§10.2) | P1, H2 |
| A2 | Re-planning thresholds by statistics epoch (§10.3) | A1, ST4 |
| A3 | Feedback store and its use by the estimator (§11.3) | X2 |
| A4 | Plan outlines, pinning, regression reporting and revert (§10.3) | A2 |
| A5 | Re-optimisation at pipeline breakers (§11.1) | H2, X2 |
| A6 | Adaptive join (§11.2) | P3, A5 |
| **Phase 8 — the buffer manager and parallelism** | | |
| K3 | The buffer manager's per-tree resident fraction and page counters; the page terms of §8.4 | the buffer manager, K1 |
| R1 | Partitioning property, `Exchange` in `PhysPlan`, DOP per pipeline in `EXPLAIN` (always serial) | M3, K1 |
| R2 | Morsel-driven parallel execution and its costs (§12) | R1, the `&sync` store |
| **Phase 9 — the test suites** | | |
| T3 | The error-semantics suite (§14.4) | H2, P1, P3 |
| T4 | The rule fuzzer, TLP and NoREC (§14.6) | T0, H2 |
| T5 | The plan-regression suite: TPC-H, TPC-DS, JOB generators and q-error reports (§14.7); the dialect-gap report first | X2, H2 |

### 15.2 The items in more detail

Each paragraph gives the files, the tests and the edge cases the item owes beyond the general rule.

**S0 — `Explain`.** `query-tree.md` §5 (the statement and its result type), §12.5 (its kind number), §15 (the
plan's text form from §13.3), §10.2 rule 3 (the digest it reports); `sql.md` (`EXPLAIN [ANALYZE] [VERBOSE]`);
`qtree.sysl`, `qtree_decode.sysl`, `qtree_encode.sysl`, `qtree_check.sysl`, `text_form.sysl`,
`sql_parser.sysl`, `sql_translate.sysl`; the shell's renderer. Before the optimizer exists the plan is the
operator tree the `Planner` builds, with no estimates. Tests: encoding round trip; `EXPLAIN` of every statement
kind, nested `EXPLAIN` refused; `EXPLAIN ANALYZE` of DML in a rolled-back transaction leaves nothing.

**S1, S2 — the statistics in the spec.** `on-disk-format.md` §3.9 (`sys.statistics`'s columns: object oid,
column id, type epoch, kind, payload bytes, commit number), `ddl.md` §4.2 (`catalog.statistics`: one row per
column with its readable fields, Postgres's `pg_stats`), §2.14 (declared statistics objects: name, table,
members, kinds; dependencies on columns, dropped with them), §3.2 (`Analyze { table?, columns }`,
`CreateStatistics`, `DropStatistics`), §7 (`ANALYZE` runs as a job), `errors.md` for any new code, and the code
for the new statement kinds (catalog object, privilege: `ANALYZE` needs the table's ownership or `maintain`).

**C1 — per-operator actuals.** `exec_eval.sysl` (`OpStats` beside `Work`), `exec_ops.sysl` and each operator's
`next`. The snapshots are of a few `u64`s per batch. Tests: for every operator kind, its rows out equal the rows
its parent received; exclusive counters sum to the statement's totals; a join of joins attributes `built` to
the right join.

**C2 — new counters.** Increment sites in `exec_ops.sysl` (Filter, Project), `exec_agg.sysl`, `exec_window.sysl`,
`exec_join.sysl` (`peak_bytes`), the sort. Tests: each counter's exact value on small inputs, empty inputs, one
batch exactly `BatchSize`, one row over.

**ST1 — HLL.** `stats_hll.sysl`. Tests (§6.3): the estimate within three standard errors at each cardinality;
merge is commutative and idempotent; serialise round trip; every integer width of the same values counts once;
`-0.0` and `+0.0` once; every NaN once; nulls not counted.

**ST2 — statistics structures and builders.** `stats.sysl`: `ColumnStats`, `TableStats`, `build_histogram`,
`build_mcv`, `correlation`. Tests: histograms on uniform, Zipf, all-equal, two-valued, all-null and one-row
columns; MCV threshold (a value is an MCV only when more frequent than the average, Postgres's rule); bounds of
every scalar type including text and `decimal`; records compared by `cmp`.

**ST3 — the statistics store.** `stats_store.sysl`, hooked into `catalog_mvcc.sysl`'s retirement. Tests for
every row of §6.7's transactional-DDL list, each its own test, at each isolation level.

**ST4 — live statistics.** `stats_live.sysl`, hooks in `store.sysl`'s write and commit paths and
`store_savepoint.sysl`. Tests: interleaved transactions committing and aborting in every order give the committed
counts exactly; a savepoint rollback drops exactly its delta; the backing sample stays a uniform sample (a
chi-squared property test over many inserts and deletes); the histogram recompute fires at the threshold and not
before; a transaction sees its own inserted rows in its estimates.

**ST5 — `ANALYZE`.** `stats_analyze.sysl`, a job kind in `store_jobs.sysl`, the statement's execution. Tests:
exact statistics after a full analyse (NDV within HLL error, MCVs and histograms exactly as a reference
computation over the same rows); analyse of an empty table, of a table being written concurrently (reads its
snapshot only), of a table dropped meanwhile (the job is cancelled), of a column retyped meanwhile; auto-analyze
fires at each threshold of §6.6 and not below it.

**ST6–ST8.** As §6.5 and §6.9; each kind of statistic with its edge cases (a prefix of one component equals the
column's NDV; a declared dependency of degree 0 and 1; a nested column whose rows all hold empty relations; a
union with one tag never used).

**M1 — the memo.** `opt_memo.sysl`. Tests: building and extracting every node kind of `query-tree.md` §6 gives
back the tree (the whole of the existing query test corpus run through the round trip, results unchanged);
hash-consing makes a duplicated subtree one group; column ids are stable across renames by `as`.

**M2 — logical properties.** `opt_props.sysl`, `opt_safety.sysl`. Tests per property per operator: keys through
joins (a key of each side, or of the left when the right is unique on the keys), through `Aggregate` (its keys),
`Distinct` (all columns); not-null through outer joins; `may_raise` for each expression kind of the safe set and
one just outside it; `guarded` set by the unnester's joins and nothing else.

**M3 — the search engine.** `opt_search.sysl`, `opt_rules.sysl`. Tests: branch and bound prunes (a counted test:
fewer tasks with a tight bound than without, same winner); the budget stops the search and the plan is still
valid; the `Sort` enforcer is added exactly when required and not provided; a rule mask disables a rule.

**M4 — `PhysPlan` and lowering.** `opt_phys.sysl`, `exec_plan.sysl` (`lower_phys`), `run_query`, `exec_dml.sysl`.
With only implementation rules, every query's plan is today's, so the whole existing suite is the regression
test, plus the differential of §14.3 against the tree lowering. This is the item that switches execution onto
the optimizer.

**X1, X2 — `EXPLAIN`.** `opt_explain.sysl`. Golden tests for every operator's `detail`; the relation and text
forms agree; `ANALYZE` counters equal C1's.

**T0 — the test harnesses.** `opt_rule_tests.sysl`, `testdata/opt/`, the random extraction and mask
differentials over `predpush`'s generator, extended.

**CE1–CE3.** As §7; the tests of §14.1. CE3's property test: the max-frequency bound is never below the actual
join size on random data.

**K1 — the cost model.** `opt_cost.sysl`, `opt_weights.sysl` (initial weights by hand, replaced by K2). Tests:
each operator's predicted counters against the actual (§14.1); lower bounds never above any plan's cost
(property test over random memo groups).

**K2 — calibration.** `bench/costcal/` and its driver; a marshal `--exclusive` run; the fitted weights committed
with the run's raw data, as benchmark results are kept.

**N1–N3.** `opt_norm.sysl`. Each rule with the three kinds of case of §14.2; N2's differential against
`push_query` over its generator, then `push_query` removed from `unnest_plan` and `plan_pushdown.sysl`'s
remaining code deleted with its tests moved.

**H1 — conflict detection.** `opt_hypergraph.sysl`. Tests: every example of Moerkotte, Fender & Eich 2013;
for every pair of TernDB join kinds, the reorderings allowed and refused, each its own test, checked against a
brute-force oracle that enumerates every bracketing of a three- and four-relation query and compares results on
random data with nulls; each error rule of §2.3 refusing the reordering it must.

**H2 — DPhyp.** `opt_dphyp.sysl`. Tests: the number of csg-cmp pairs equals the known formula for chains, cycles,
stars and cliques up to 12 relations; every csg-cmp pair emitted once; the plan found equals the brute-force
optimum on random graphs of up to 8 relations; hyperedges (a predicate over three relations); cross products
only where the graph is disconnected; binder order kept (§4.3). This is the item that retires `predpush`'s
re-bracketing.

**H3 — large queries.** `opt_large.sysl`. Tests: a 20-, 50- and 120-relation chain, star and random graph plan
within the budget; the fallback's plan cost is within a stated factor of DPhyp's where both run.

**U1, E1, E2.** Rules as §5.2 and §5.3, each with legality cases; E1's `sum` overflow raised exactly as without
the rewrite.

**P1 — access paths.** `opt_access.sysl`, `exec_access.sysl`'s `access_of` reduced to the null rule and the
range construction. Tests: §9.1's rule; every `secidx` access test still passing; a selective index chosen over a
full scan and a non-selective one not; a partial index chosen only when matched.

**P2 — build side.** `exec_join.sysl` (a fixed build side, the guard), the implementation rule. Tests: probe
order provided and relied on by an elided sort; a guarded join built on the left reads its right first and stops
when it is empty (a subtree raising an error over an empty domain is not evaluated); the run-time guard chosen at
the stated thresholds.

**P3 — index nested-loop join.** `exec_index_join.sysl`, the rule. Tests: every kind; duplicate and null probe
keys; probes on PK and on an index, a partial index, an expression index; the logged predicate (a serializable
phantom test through a probe); `single`'s `R0003`.

**P4–P10.** Operators and rules as §9; each operator with the edge-case pass (empty input, one group, every row
its own group, null keys, `same`-equal values of different widths in one group).

**A1–A6.** As §10–§11. A1's tests: a skewed column where a value is 50% of the rows and another 0.001% gets two
plans; a uniform column gets one; the validity range computed is checked against brute-force re-optimisation at
sampled selectivities. A5's tests: a forced misestimate (statistics loaded wrong) triggers exactly one
re-optimisation and the result is unchanged; no re-optimisation once the plan above has produced rows.

**K3, R1, R2.** As §8.4 and §12, waiting on the buffer manager and the shared store.

**T3–T5.** As §14.4, §14.6, §14.7.

### 15.3 Which items need which pending branches

| Branch | Items that need it landed |
|---|---|
| `predpush` | M2 (the safe set), N2, and everything after N2 (H1, H2, …) |
| `secidx` | C1, ST6, P1, P3, P7 |
| `hashjoin` | C1, ST1 (and so every statistics item), P2, P4 |
| `rowscan` | C1, ST5 |
| none | S0–S2, M1 |

S0, S1, S2 and M1 can start at once; C1 and ST1 as soon as `hashjoin`, `rowscan` and `secidx` land.

## 16. Decisions

Every choice this design left open is **decided** (2026-11-25), as recorded in `docs/decisions.md` § Optimizer
(rows O1–O11, which keep these numbers). Q1, Q2, Q4 and Q6 are the user's decisions; the rest are the director's,
following this design's recommendation. Each decision is the design the sections named describe; reverting one
changes them.

| # | Question | Decided | By | Changes |
|---|---|---|---|---|
| Q1 | Search framework: a Cascades memo with DPhyp filling join groups (§3.2), or pure bottom-up DPhyp plus heuristics? | The hybrid: a Cascades memo whose join groups DPhyp fills | the user | §3, §4, M3, H1–H3 |
| Q2 | Statistics: incremental maintenance (the plan's Decided list), `ANALYZE` and auto-analyze, or one of them? | Both, as §6 has them | the user | §6, ST4, ST5 |
| Q3 | Does `query-tree.md` §6.6 rule 2's "no operator that re-evaluates a subtree once per outer row" forbid an index nested-loop join, whose inner is one access path probed per outer key? | No: the rule is clarified to permit a probe of one access path with safe filters (S3), as `isolation.md` §4.4 already assumes | the director | §5.3, §9.3, S3, P3, A6 |
| Q4 | Statistics inside or outside the versioned catalog's definitions (§6.7)? | Outside: `ANALYZE` is not DDL and changes no catalog version | the user | §6.7, S1, ST3 |
| Q5 | Create extended statistics automatically for column groups the feedback finds misestimated? | Yes, behind a setting, off by default | the director | §6.5, §11.3 |
| Q6 | Plan caching for parameterised statements? | Generic plans with validity ranges, and up to three custom plans per statement by selectivity bucket (§10.2), replacing planning on every execution | the user | §10, A1 |
| Q7 | `EXPLAIN` as a statement kind of the tree (§13.1), or a flag on `Execute` in the wire protocol? | A statement kind returning a relation, so the C API, the wire and SQL all have it | the director | §13, S0 |
| Q8 | Index-only scans: versioned index entries, or an all-visible map maintained by purge? | Deferred until the on-disk secondary indexes (`store.md` "Deferred") are built, since both are storage format | the director | §9.1, P11 |
| Q9 | Re-optimisation at pipeline breakers (§11.1) and adaptive joins (§11.2), or robust costing (§8.6) and feedback alone? | Build both, after the static optimizer is measured on JOB (T5) | the director | §11, A5, A6 |
| Q10 | On a plan regression (§10.3): revert automatically, or report? | Report by default; revert only with `plan_regression_revert` on | the director | §10.3, A4 |
| Q11 | Where does the plan-regression suite's JOB data (3.6 GB, downloaded) live? | Outside the repository, read from a path given to the suite, which skips JOB when it is absent | the director | §14.7, T5 |

## 17. References

- G. Graefe, "The Cascades framework for query optimization", IEEE Data Eng. Bull. 1995; L. Shapiro et al.,
  "Exploiting upper and lower bounds in top-down query optimization" (Columbia), IDEAS 2001.
- G. Moerkotte, T. Neumann, "Dynamic programming strikes back" (DPhyp), SIGMOD 2008.
- G. Moerkotte, P. Fender, M. Eich, "On the correct and complete enumeration of the core search space"
  (CD-A/B/C, TES), SIGMOD 2013.
- T. Neumann, B. Radke, "Adaptive optimization of very large join queries", SIGMOD 2018.
- T. Neumann, A. Kemper, "Unnesting arbitrary queries", BTW 2015; T. Neumann, "Improving unnesting of complex
  queries", BTW 2025.
- V. Leis et al., "How good are query optimizers, really?", VLDB 2015; "Morsel-driven parallelism", SIGMOD 2014.
- A. Hertzschuch et al., "Simplicity done right for join ordering", CIDR 2021; W. Cai, M. Balazinska, D. Suciu,
  "Pessimistic cardinality estimation", SIGMOD 2019.
- P. Gibbons, Y. Matias, V. Poosala, "Fast incremental maintenance of approximate histograms", VLDB 1997.
- S. Heule, M. Nunkesser, A. Hall, "HyperLogLog in practice" (HLL++), EDBT 2013.
- F. Olken, D. Rotem, "Random sampling from B+ trees", VLDB 1989; P. Haas et al., "Sampling-based estimation of
  the number of distinct values of an attribute", VLDB 1995.
- W. Yan, P.-Å. Larson, "Eager aggregation and lazy aggregation", VLDB 1995; G. Moerkotte, T. Neumann,
  "Accelerating queries with group-by and join by groupjoin", VLDB 2011.
- D. Simmen, E. Shekita, T. Malkemus, "Fundamental techniques for order optimization", SIGMOD 1996.
- N. Kabra, D. DeWitt, "Efficient mid-query re-optimization of sub-optimal query execution plans", SIGMOD 1998;
  M. Stillger et al., "LEO — DB2's learning optimizer", VLDB 2001.
- J. Pellenkoft, C. Galindo-Legaria, M. Kersten, "The complexity of transformation-based join enumeration",
  VLDB 1997.
- M. Rigger, Z. Su, "Finding bugs in database systems via query partitioning" (TLP), OOPSLA 2020; "Detecting
  optimization bugs in database engines via non-optimizing reference engine construction" (NoREC), ESEC/FSE 2020.
- PostgreSQL's `selfuncs.c`, `pg_statistic`, extended statistics (`CREATE STATISTICS`); SQL Server's 2014
  cardinality estimator (exponential backoff, ascending keys), adaptive joins, parameter-sensitive plans and
  automatic plan correction; CockroachDB's `opt` (Optgen rules, `testdata`, `ReorderJoins`).
