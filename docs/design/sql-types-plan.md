# Every SQL data type: the implementation plan

The specification is `query-tree.md` §2.7–§2.21 with the sections they cite (`sql.md` §2.5, §4, §5.10;
`on-disk-format.md` §4.3–§4.6; `wire-protocol.md` §6.2–§6.3; `ddl.md` §2.3, §2.8, §3.3, §5.3, §8.5; `errors.md`
`Q3018`–`Q3021`, `Q4006`, `Q8014`, `R0009`–`R0016`), decided in `docs/decisions.md` Q78–Q90. This file cuts it into
items. Each item is one agent's deliverable: it implements its part of the spec exactly, with a test for every rule
it implements and every edge case its section names, and it ends READY for the merge train. An item that finds the
spec wrong or silent changes the spec in the same commit, or stops and asks.

## How to read the list

- **Depends** names the items that must have landed on `dev` first. Items with no dependency between them may run
  at once, each in its own worktree.
- **Contended** names the files that the pending branches `rowscan`, `hashjoin`, `secidx` and `predpush` also
  change. `rowscan` changes `exec_scan.sysl` and `key_encoding.sysl` and adds `exec_vector_build.sysl` and
  `store_scan.sysl`; `hashjoin` rewrites `exec_join.sysl` and adds `value_hash.sysl`; `secidx` changes
  `exec_access.sysl`, `store_index.sysl` and `exec_scan.sysl`; `predpush` changes `plan_unnest.sysl`. **An item
  marked contended starts only after the branches it names have landed**, so it builds on their code instead of
  conflicting with it. Every item that gives a type values touches the row-to-vector path (`rowscan`), hashing
  (`hashjoin`) and, if the type can be a key, the key encoder (`rowscan`, `secidx`), so in practice the value items
  wait for all four branches; the libraries and the foundation items do not.
- **Shared files.** The value items all add arms to the same dispatch points: `types.sysl`, `qtree_types.sysl`,
  `value.sysl`, `compare.sysl`, `text_form.sysl`, `encode.sysl`/`decode.sysl`, `wire_encode.sysl`/`columns.sysl`,
  `row_format.sysl`, `key_encoding.sysl`, `qtree_check.sysl`, `exec_functions.sysl`, `value_hash.sysl`,
  `sql_parser.sysl`, `sql_translate.sysl`. Two value items running at once will conflict textually there; the train
  takes them one at a time, and each merges `dev` before READY.
- **Tests.** Each item adds a `<area>_tests.sysl` file of its own (named below) and extends the round-trip and
  golden suites it touches. The edge-case pass of `CLAUDE.md` applies to every item: empty and one-element inputs,
  boundary sizes and maximum values, duplicates and nulls, every error path with its code. Wire golden vectors
  (`docs/spec/vectors/wire/`) are added for every new fixed-width and varbin encoding.

## The items

### Foundation (sequential)

**A1. `timestamp` and `timestamptz` renamed (Q79).** Kind 12 is `timestamp` (alias `datetime`), kind 13
`timestamptz`, in the type model, the text tree and `EXPLAIN`, the catalog relations (`jobs.started`,
`jobs.finished`), the shell's rendering and every test that names them; SQL's `TIMESTAMP` is the zoneless type,
`TIMESTAMP WITH TIME ZONE`/`timestamptz` the instant, and a `TIMESTAMP '…'` literal with an offset is a
`timestamptz` (`sql.md` §2.5 rule 1). Files: `types.sysl`, `qtree_types.sysl`, `text_tree.sysl`,
`catalog_relations.sysl`, `sql_parser.sysl`, `sql_translate.sysl`, `shell_render.sysl`. Tests:
`sql_timestamp_tests.sysl` (each spelling, the literal rule with `Z`, `+HH:MM` and none, `datetime` still
accepted, `timestamp ↔ timestamptz` still `Q3012`), updated text-tree and catalog-relation tests. Depends: none.
Parallel with the libraries. Contended: none.

**A2. The type model of §2.7.** Kinds 27–34 and 64–82 with their parameters: the type model, §12.3 encoding and
decoding (the canonical-form rules: precision 6 only as kinds 11–13, `decimal` to `p ≤ 1000`, `Q1003`), tree minor
version 1.1 (a kind of 1.1 in a 1.0 envelope is `Q1002`), type identity with parameters and nominal kinds (§2.6),
`Q3019` for every parameter range, the descriptor tags 0x18–0x1F and 0x24–0x34 (`on-disk-format.md` §4.3, read and
written, unknown tags `data_corrupted`), the type text of the text tree, the C ABI kind constants, and the error
rows `Q3018`–`Q3021`, `Q4006`. No type gets values here: a `Literal`, parameter or column value of a new type is
refused with `Q3002` until its family item lands. Files: `types.sysl`, `qtree_types.sysl`, `qtree_encode.sysl`,
`qtree_decode.sysl`, `qtree_check.sysl`, `row_format.sysl`, `catalog_typing.sysl`, `text_tree.sysl`, `error.sysl`.
Tests: `qtree_types_sql_tests.sysl` (every kind round-trips; every parameter boundary and one past it; the
non-canonical precision-6 forms; minor-version gating; identity of `varchar(3)` vs `varchar(4)`, `time(3)` vs
`time`, two distinct types over one source), descriptor round-trip and corruption tests. Depends: A1. Contended:
none.

**A3. Store assignment (§4.1 rule 15).** The assignment coercion at `Insert`/`Update`/`Upsert` targets and fields,
with its runtime check node, and parameter domain checks (`Q4006`), wired for `varchar(n)` first (so it is
testable) and generic for the others. Files: `qtree_check.sysl`, `exec_dml.sysl`, `exec_eval.sysl`,
`wire_params.sysl`. Tests: `assignment_tests.sysl` (exact length, one over, over by spaces only, over by spaces then
a letter, null, a record field, an `Upsert` `set`, a parameter). Depends: A2. Contended: none.

### Libraries (no type-model dependency; start any time)

Each is a self-contained sysl module with its own tests against a reference model; none touches a contended file.

**L1. `numeric` arithmetic** (`numeric.sysl`): base-10,000 digit arithmetic, `add`, `sub`, `mul`, `div` with
PostgreSQL's `select_div_scale`, `mod`, `round`, `trunc`, `sqrt`, `exp`, `ln`, `log`, `power`, comparison,
parsing and printing with display scale, the special values. Tests: `numeric_tests.sysl`, including randomized
tests against a big-integer reference model and PostgreSQL's published results for its regression cases.

**L2. `decfloat` arithmetic** (`decfloat.sysl`): IEEE 754-2008 decimal64 and decimal128 in BID, encode and decode,
arithmetic with round-half-even, `quantize`, `normalize`, `totalorder`, to-scientific-string and parsing. Tests:
`decfloat_tests.sysl`, including the General Decimal Arithmetic test vectors (`dectest`) for the operations
implemented.

**L3. Unicode Collation Algorithm** (`collation.sysl`, plus a generated data module from the DUCET `allkeys.txt` and
the CLDR root): sort keys at strengths 1–4, comparison, BCP 47 tag parsing with `ks`, CLDR tailorings for the
locales the item lists. Tests: `collation_tests.sysl`, including the UCA conformance test files
(`CollationTest_*.txt`). Its size may need a split (DUCET first, tailorings second); the agent says so before
starting.

**L4. Unicode normalization** (`unicode_norm.sysl`): NFC, NFD, NFKC, NFKD and the quick checks, from
`UnicodeData.txt`/`DerivedNormalizationProps.txt`. Tests: `NormalizationTest.txt`.

**L5. Regular expressions** (`regex.sysl`): the PostgreSQL advanced-regex subset `regexp_*` needs and ISO's `SIMILAR
TO` translation, with a linear-time matcher. Tests: `regex_tests.sysl` against a backtracking reference on random
patterns.

**L6. XML** (`xml.sysl`): an XML 1.0 well-formedness parser for documents and content, a serializer, and the node
model the constructors need. Tests: `xml_tests.sysl`, including the W3C XML conformance suite's well-formedness
cases.

**L7. XPath 1.0** (`xpath.sysl`) over L6's node model. Depends: L6. Tests: `xpath_tests.sysl`.

**L8. SQL/JSON path** (`json_path.sysl`): the path language of ISO/IEC 9075-2:2023 in lax and strict modes, over a
value model shared by `json` and `jsonb`. Tests: `json_path_tests.sysl`, including PostgreSQL's `jsonpath`
regression cases.

### Families (each gives a type values end to end)

Each family item implements, for its types: the value representation; the §12.6 parameter and literal codec; the
result column (`wire-protocol.md` §6.2); the row encoding and, where the type may be a key, the key encoding; the
text form (§3.5); `same`, `cmp`, `eq` and the hash; the coercions, common type and casts of its section; its
operators and functions; its SQL spellings and constants; and the retype rows of `ddl.md` §8.5 rule 6 that concern
it. **Contended, for every family item: `exec_scan.sysl`/`exec_vector_build.sysl` and `key_encoding.sysl`
(`rowscan`), `value_hash.sysl` (`hashjoin`), and `store_index.sysl` where the type is a key (`secidx`).** So each
starts after those three branches land. Families with no dependency between them may run in parallel, subject to
the shared-files note above.

| Item | Types and scope | Files beyond the shared ones | Tests | Depends |
|---|---|---|---|---|
| F1 | `varchar(n)`, `char(n)`: PAD SPACE in every comparison, `char` stored stripped and keyed padded, `concat` typing, the §2.8 rule 6 functions except the regex, normalization and `similar_to` ones; `N'…'`, `U&'…'` | `exec_kernels.sysl` | `char_types_tests.sysl` | A3 |
| F2 | `varbinary(n)`, `binary(n)`, `X'…'`, the §2.9 functions | — | `binary_types_tests.sysl` | A3 |
| F3 | `time(p)`, `timestamp(p)`, `timestamptz(p)`, `timetz(p)`: rounding, `timetz` order and key, the §2.11 casts, `extract`, `date_trunc`, `date_bin`, `make_*`, `age`, `overlaps`, the `current_*`/`local*` functions, `AT TIME ZONE`/`AT LOCAL` | `exec_zone.sysl` | `datetime_types_tests.sysl` | A2 |
| F4 | `interval_ym`, `interval_dt`: ISO interval literals, the §4.4 rule 8–9 arithmetic, window `range` offsets over them | `exec_window.sysl` | `interval_types_tests.sysl` | F3 |
| F5 | `numeric` and `decimal(p > 38)`: §4.4 rules 2 and 5, the numeric key encoding, `numeric` constants in SQL (`sql.md` §2.6, more than 38 digits) | — | `numeric_type_tests.sysl` | A2, L1 |
| F6 | `decfloat(16)`, `decfloat(34)` | — | `decfloat_type_tests.sysl` | F5, L2 |
| F7 | `money` | — | `money_tests.sysl` | F5 |
| F8 | `inet`, `cidr`, `macaddr`, `macaddr8` and their operators | — | `network_types_tests.sysl` | A2 |
| F9 | `bit(n)`, `varbit(n)`, `B'…'` and their operators | — | `bit_types_tests.sysl` | A3 |
| F10 | `array<T, m>`: values, codecs, wire relation node, `eq`/`cmp`/key, text form, `array_of`, `element`, `try_element`, `slice`, `cardinality`, `concat`, `R0011`/`R0012` | — | `array_type_tests.sysl` | A3 |
| F11 | Array functions and SQL: `ARRAY[…]`, `ARRAY(query)`, `T[]`, `T ARRAY[m]`, `UNNEST(a) [WITH ORDINALITY]`, `= ANY`/`ALL`, `array_agg` and the rest of §2.13 rule 6 | `plan_unnest.sysl` (**contended: `predpush`**), `exec_agg.sysl` | `array_sql_tests.sysl` | F10 |
| F12 | Multisets: `T MULTISET`, `MULTISET[…]`, the §2.14 functions and aggregates, `multiset_eq` | `plan_unnest.sysl` (**contended: `predpush`**), `exec_agg.sysl`, `exec_setop.sysl` | `multiset_tests.sysl` | A2 |
| F13 | Rows: `ROW(…)` types and constructors, composite text form, record key components, `IS [NOT] DISTINCT FROM` | `store_index.sysl` (**contended: `secidx`**) | `row_type_tests.sysl` | A2 |
| F14 | `jsonb`: binary row form, text form, order and key, casts, PostgreSQL's operators and functions of §2.16 rule 5 | `json.sysl` | `jsonb_tests.sysl` | F5 |
| F15 | ISO SQL/JSON functions over `json` and `jsonb` (`json_value`, `json_query`, `json_exists`, `json_table`, the constructors and aggregates, `IS JSON`) | `json.sysl`, `exec_agg.sysl` | `sql_json_tests.sysl` | F14, L8 |
| F16 | `xml`: the type, its casts, `Q3018` on comparison and grouping, the ISO constructors and `xmlparse`/`xmlserialize`, `is_document` | — | `xml_type_tests.sysl` | A2, L6 |
| F17 | `xpath`, `xpath_exists`, `xmlexists`, `xmlquery`, `xmltable` | `exec_agg.sysl` (`xmlagg`) | `xml_query_tests.sysl` | F16, L7 |
| F18 | `range<T>`: canonical forms, codecs, the wire record node, order and key, constructors and operators of §2.19 rule 5 | — | `range_tests.sysl` | A2 |
| F19 | `multirange<T>`, `range_agg`, `range_intersect_agg`, `Unnest` of a multirange | `plan_unnest.sysl` (**contended: `predpush`**), `exec_agg.sysl` | `multirange_tests.sysl` | F18 |
| F20 | Regex and normalization functions: `regexp_*`, `similar_to`, `normalize`, `is_normalized` | — | `regex_functions_tests.sysl` | F1, L4, L5 |

### Named types and collations

| Item | Scope | Files | Tests | Depends |
|---|---|---|---|---|
| N1 | DDL plumbing: `CreateType.def`, the new `TypeChange` variants, `ColumnDef.collation`, `IndexKey.collation` (tree 1.1), `catalog.types` kinds, `catalog.columns.collation`, `catalog.indexes` collation; no behaviour beyond recording them | `catalog_objects.sysl`, `catalog_apply.sysl`, `catalog_alter.sysl`, `catalog_relations.sysl`, `qtree_decode.sysl`, `qtree_encode.sysl` | `ddl_typedef_tests.sysl` | A2 |
| N2 | Enums' PostgreSQL surface: text casts, `enum_first`/`enum_last`/`enum_range`, `CREATE TYPE … AS ENUM`, `ALTER TYPE … ADD VALUE` (and `BEFORE`/`AFTER` as `Q6018`), `RENAME VALUE` | `sql_parser.sysl`, `sql_translate.sysl`, `catalog_alter.sysl` | `enum_sql_tests.sysl` | N1 |
| N3 | Domains: assignment checks (`T3002`, `T3004`), defaults, `AlterType` domain changes with their validation jobs, `CREATE DOMAIN` | `exec_dml.sysl`, `store_jobs.sysl`, `catalog_alter.sysl` | `domain_tests.sysl` | N1, A3 |
| N4 | Distinct types: nominal identity, casts both ways, comparison only within the type, results sent as the source | `qtree_check.sysl` | `distinct_type_tests.sysl` | N1 |
| N5 | Structured types: hierarchy unions, constructors, observers, mutators, `treat`, `is_of`, `specifictype`, subtype coercion, appending a subtype or attribute as a metadata-only retype, `EQUALS ONLY BY STATE` | `catalog_alter.sysl`, `qtree_check.sysl` | `structured_type_tests.sysl` | N1 |
| N6 | Named range types (`CREATE TYPE … AS RANGE`) and their multirange names | `catalog_objects.sysl` | `range_ddl_tests.sysl` | N1, F19 |
| C1 | Collations in the tree: the optional `collation` arguments and fields (tree 1.1), `Q3021`, deterministic comparison and sort under a collation | `qtree_check.sysl`, `exec_ops.sysl` | `collation_tree_tests.sysl` | A2, F1, L3 |
| C2 | Collated keys and nondeterministic collations: sort-key index components (`key_spec` bit 2), unique indexes under a collation, `same`/hash by sort key, index range scans under a collation | `store_index.sysl`, `exec_access.sysl` (**contended: `secidx`**), `exec_join.sysl` (**contended: `hashjoin`**) | `collation_index_tests.sysl` | C1, N1 |
| C3 | SQL collation derivation (`sql.md` §4.2, `Q8014`), `COLLATE` clauses, column collations through the resolver | `sql_parser.sysl`, `sql_translate.sysl` | `sql_collation_tests.sysl` | C1, N1 |

### Closing items

| Item | Scope | Depends |
|---|---|---|
| Z1 | Substrait type mapping (`query-tree.md` §14.1–§14.2) | F1, F2, F3, F4 |
| Z2 | The PostgreSQL integer and float aliases and the `int8` reading, once the user decides Q81 | A2 |
| Z3 | Benchmark rows for the new types (`CLAUDE.md` § Every release ships a benchmark): string and numeric kernels, `jsonb` access, range containment, collated sort, against PostgreSQL and DuckDB where they have the type | the items measured |

## Order and parallelism

1. **Now:** A1, then A2; in parallel with them, every library L1–L8 (no contended files, no shared dispatch
   points).
2. **After A2:** A3, N1, F3, F8, F12, F13, F16, F18 can start; F3, F8, F16 and F18 are value items and wait for
   `rowscan`, `hashjoin` and `secidx`; F12 also waits for `predpush`, F13 for `secidx`.
3. **After A3:** F1, F2, F9, F10, N3.
4. **Chains:** L1 → F5 → F6, F7, F14 → F15; F3 → F4; F10 → F11; F18 → F19 → N6; L6 → F16 → F17 (with L7);
   N1 → N2, N4, N5; L3 + F1 → C1 → C2, C3; F1 + L4 + L5 → F20.
5. **Contended items** (F11, F12, F13, F19, C2, and every value item through `exec_scan.sysl`, `key_encoding.sysl`
   and `value_hash.sysl`) start only once the branch they name has landed on `dev`.
