# TernDB SQL

**Status:** normative, draft. **Dialect version:** 0.0.1. **Produces:** tree schema 1.0.

This document defines TernDB's own SQL dialect: its lexical structure, the grammar of the statements it has, and for
each construct the query tree (`query-tree.md`, [QT]) or DDL tree (`ddl.md`, [DDL]) it produces. SQL is a **front
end**: it has no meaning beyond the tree it produces ([QT] §1 rule 3), and the native protocol never carries SQL
text ([WIRE] §0 rule 4). The front end is a library that runs in the client (the shell, a driver, later the
Postgres-wire layer); `docs/design/sql.md` documents its API.

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as in RFC 2119. Every MUST is meant to be
checkable by a test that translates SQL text and compares the tree, or the error code and position, with the one
this document gives. *Rationale* text explains a choice and is not a rule. Where nothing here argues otherwise the
dialect follows standard SQL, and PostgreSQL where the standard leaves a choice; where it departs from PostgreSQL it
says so.

## 1. Model

1. **Input.** A **script** is UTF-8 text holding zero or more statements separated by `;`. Translation turns each
   statement into one **action**: a tree envelope ([QT] §5) to execute, or a transaction request ([WIRE] §5) for
   the transaction statements of §6. The first error in a script, lexical, syntactic or of translation, is the
   result of the whole script, and no action is produced.
2. **Positions.** Every error carries the byte offset in the script of the token it is about, and the 1-based line
   and column of that offset, the column counted in characters (Unicode scalar values). Every action carries the
   byte range of its statement, without the `;`.
3. **Names.** A tree names every table by `(schema, name)` ([QT] §8.1). An unqualified name is in the **default
   schema**, which the caller gives and which is `public` when it gives none. Names are case-sensitive in the tree;
   SQL folds unquoted identifiers (§2.2).
4. **Columns of tables.** A tree resolves table and column names at validation, so translation needs no catalog
   to produce a tree. It does need a table's **column list** (names and types, and the primary key) for what SQL
   leaves to the database and a tree spells out: which source an unqualified column belongs to when there are
   several (§5.2), what `*` stands for beside other items or over several tables (§5.4), the columns of an
   `INSERT` without a column list (§3.2), the conflict target and referenced columns that default to a primary key
   (§3.2, §3.5), the type a constant takes from a column (§5.9), and the nullability a retyped column keeps
   (§3.6). The caller supplies column lists through a **resolver**. A table the resolver does not know is
   **opaque**: statements naming it are still translated, and only a construct that needs its columns is refused,
   with `Q8010`.
5. **Scripts change what is known.** A `CREATE TABLE`, `ALTER TABLE` or `DROP TABLE` earlier in the same script
   replaces what the resolver says of that table for the statements after it: the created or altered columns, or,
   after a drop, an opaque table. A rename makes the old name opaque.
6. **Binders.** Every binder a statement's tree introduces is unique in the statement ([QT] §8.2 rule 2). The
   binder of a table or subquery is its alias, else its name; a name already used in the statement is suffixed
   `_2`, `_3`, …. Nodes the translation adds take the binders `select`, `group`, `values`, `one`, `excluded` and
   `subquery`, suffixed the same way. Binders are invisible in results ([QT] §10.2 renames them for the shape).
7. **Spans.** The envelope's `spans` is absent in this version; errors from validation name tree paths.
8. **Depth.** A statement's expressions and queries nest at most 256 levels, the tree's own limit ([QT] §12,
   `Q1006`): each expression, query, `NOT` and unary minus is a level, and each operator of a chain of one
   left-associative operator (`a + b + c`, `a AND b AND c`, `x::T::U`, `UNION`, `JOIN`) one more. Deeper text is
   refused with `Q1006`, at the token that passes the limit, before it is translated.
   *Rationale: the server refuses a deeper tree anyway, and refusing it while reading keeps the front end's own
   recursion bounded, so no input can exhaust its stack.*

## 2. Lexical structure

### 2.1 Whitespace and comments

1. Space, tab, line feed, carriage return and form feed are whitespace. Whitespace separates tokens.
2. `--` starts a comment that ends at the end of the line. `/*` starts a comment that ends at the matching `*/`;
   block comments **nest**. An unterminated block comment is `Q8002`, at its `/*`. Comments are whitespace,
   except that they do not continue a string (§2.4 rule 3).

### 2.2 Identifiers and keywords

1. An unquoted identifier starts with an ASCII letter, `_` or any non-ASCII character, and continues with those,
   ASCII digits and `$`. It is **folded to lower case**, ASCII letters only: `Users`, `USERS` and `users` are one
   name, and `ÉTÉ` is `ÉtÉ`.
2. A quoted identifier is `"…"`, with `""` standing for one `"`. It is kept exactly, may hold any character, and is
   never a keyword. An empty one is `Q8003`; an unterminated one is `Q8002`, at its opening quote.
3. A **keyword** is an unquoted identifier spelled as one. **Reserved** words are identifiers only when quoted:
   `all and any as asc between both case cast check collate constraint create cross current_timestamp default desc
   distinct do else end except exists false fetch for foreign from full group having ilike in inner intersect into
   is join lateral left like limit natural not null offset on only or order outer over primary references returning
   right select similar some table then true union unique using values when where window with`. Every other
   keyword (`key`, `set`, `index`, `first`, `date`, …) is also a name wherever a name is expected. A reserved word
   where a name is expected is `Q8001`.
   *Rationale: folding to lower case matches PostgreSQL, which most SQL TernDB users will have written; only
   ASCII folds, as in PostgreSQL's UTF-8 databases, so folding needs no Unicode tables and never depends on a
   locale.*

### 2.3 Operators and punctuation

The tokens are `( ) , ; . + - * / % = < > <= >= <> != || ::`, each taking its longest spelling; `!=` is `<>`. Any
other character outside a string, identifier or comment (`^ & | ! ? [ ] { } : @ # ~` and the like) is `Q8003`.

### 2.4 String constants

1. `'…'` is a string constant, with `''` standing for one `'`. A backslash is an ordinary character.
2. `E'…'` (or `e'…'`, the `E` not part of a longer identifier) also decodes backslash escapes: `\n \t \r \b \f \\
   \' \"`, `\uXXXX` and `\UXXXXXXXX` (hexadecimal, a Unicode scalar value). Any other escape, a `\u` with too few
   hexadecimal digits, and a surrogate or a value past U+10FFFF are `Q8003`, at the backslash.
3. Two string constants separated only by whitespace that includes a line feed are one constant (standard SQL).
4. An unterminated string is `Q8002`, at the quote (or `E`) that opened it.

### 2.5 Typed constants

`type 'text'`, a type name of §4 followed by a string constant, is a constant of that type read from its text form
([QT] §3.5): `DATE '2024-02-29'`, `TIMESTAMP '2024-01-01T00:00:00Z'`, `INTERVAL 'P1D'`, `UUID '…'`. The text forms are the tree's, not
PostgreSQL's: an interval is written `P1D`, not `1 day`. It is
`Literal(type, value)`. A text that is not a value of the type is `Q8004`, at the type name.

### 2.6 Numbers

1. A number is `digits[.[digits]][e[±]digits]` or `.digits[e[±]digits]`: `1`, `1.`, `.5`, `1.5`, `1e10`, `2.5E-3`.
   A sign is an operator. An exponent without digits, and a letter, digit, `_`, `$` or `.` straight after a
   number, are `Q8003`.
2. Without context (§5.9) a number's type is:
   - an integer (digits only) is `int32` when it fits, else `int64`, else `decimal(p, 0)` with `p` its digits;
   - a number with a point and no exponent is `decimal(p, s)`, `s` the digits after the point and `p` the
     significant digits (leading zeros dropped), at least `s` and at least 1: `1.50` is `decimal(3, 2)`, `.5` is
     `decimal(1, 1)`, `1.` is `decimal(1, 0)`;
   - a number with an exponent is `float64`.
   More than 38 digits, and a `float64` out of range, are `Q8004`. A negated number (`-5`) is one constant, so
   `-2147483648` is an `int32`.
   *Rationale: TernDB has exact decimals, so a point means exact, as standard SQL's exact numeric literal; only
   an exponent makes an approximate one.*

### 2.7 Parameters

`$n`, `1 ≤ n ≤ 100000000`, is parameter `n − 1` of the envelope ([QT] §7.3, §10.1). `$0`, a larger `n`, a `$`
without digits (dollar quoting is not supported) and digits followed by a letter are `Q8003`. A parameter's type is never inferred by the server
([QT] §4), so the front end decides it: from a cast written on it (`$1::int64`, which is the parameter itself, not
a `Cast`), from the column it is assigned to (its exact type, nullability included), or from the column it is
compared with (that column's type without nullability) (§5.9). A parameter whose type nothing gives, and one given
two different types, are `Q8007`. Every `$1 … $n` up to the highest written MUST be used.

### 2.8 Statements and complete input

1. Statements are separated by `;`. Empty statements are skipped.
2. Text is **complete**, for a shell reading a statement line by line, when its last token outside strings,
   quoted identifiers and comments is `;`, followed by nothing or only whitespace and comments. Text ending inside
   an unterminated string, quoted identifier or block comment is not complete. A lexical error of another kind
   does not make text incomplete, so that the shell sends it and reports the error.

## 3. Statements

The grammar is given in a compact EBNF: `[x]` optional, `{x}` zero or more, `a | b` alternatives, keywords in
upper case (any case is accepted), `name` an identifier of §2.2, `qname` = `[name .] name`.

### 3.1 Script

```
script    = [statement] { ";" [statement] }
statement = query | insert | update | delete | create_table | drop | alter_table | create_index
          | create_schema | create_view | transaction
```

A `query` statement is `Query { rel }` ([QT] §5) with `rel` per §5.

### 3.2 INSERT

```
insert   = INSERT INTO qname [AS name] [ "(" name {"," name} ")" ] ( query | DEFAULT VALUES )
           [ ON CONFLICT [ "(" name {"," name} ")" | ON CONSTRAINT name ]
             DO ( NOTHING | UPDATE SET set {"," set} [WHERE expr] ) ]
           [ RETURNING item {"," item} ]
set      = name "=" expr
```

1. It is `Insert { table, as, old: absent, columns, source, on_conflict, returning }` ([QT] §9.2). `as` is the alias
   or the table name (§1 rule 6).
2. `columns` is the column list, or every column of the table in order when there is none (§1 rule 4; `Q8010` for
   an opaque table). `DEFAULT VALUES` is `columns = []` and `source = Values(values, [], [[]])`, one row of no
   columns, so every column takes its default.
3. A `VALUES` source with no `ORDER BY`/`LIMIT` is `Values(values, [(c, T)…], rows)` with one column per target,
   `T` the target column's type, made nullable (`T?`) when a value in that column is `NULL`, and each constant
   typed as `T` (§5.9, as an assignment). So a `NULL` into a `NOT NULL` column is refused as a nullable source
   column, `Q3005` ([QT] §9.2 rule 1). With an opaque table it is
   the `VALUES` query of §5.3, its columns named after the targets. A row with another number of values than
   targets is `Q8012`; `DEFAULT` as a value is `Q8005`.
4. Any other source is the query of §5, with the targets' types handed to it as assignments (§5.9 rule 1)
   where the table is known: each item of its select list that is a constant (§5.9), counted after `*` is
   expanded, is typed as the target in its position, and so is each column of a `VALUES` list whose values are
   all constants, made nullable when one is `NULL`. This holds in each input of a set operation and in the body
   of a `WITH`, not in a CTE, a derived table or a subquery, which are queries of their own. So `INSERT INTO t
   (small) SELECT 1` writes an `int16`, `SELECT NULL` a `Null(T)` (`Q3005` into a `NOT NULL` column, as in rule 3),
   and a constant that is not a value of its target is `Q8004`. An item with no target, or an item that is not a
   constant, keeps its own type, and validation refuses a type the target does not take (`Q3001`).
   *Rationale: PostgreSQL types an untyped constant by the column it is inserted into; the user chose this
   reading on 2026-11-19 (Q74).*
5. `ON CONFLICT` is `OnConflict { target, excluded, action }` ([QT] §9.3): the column list, `Constraint(name)`, or,
   when neither is written, the primary key's columns (`Q8010` when not known). `excluded` is the binder
   `excluded`, which `excluded.c` names; `DO NOTHING` is `nothing`, `DO UPDATE SET … [WHERE …]` is `update { set,
   where }`, each value an assignment to its column (§5.9).
6. `RETURNING` items are named as select-list items (§5.4) and see the target. `*` is every column of the target,
   in order (`Q8010` for an opaque table).

### 3.3 UPDATE and DELETE

```
update = UPDATE qname [[AS] name] SET set {"," set} [FROM from {"," from}] [WHERE expr] [RETURNING item {"," item}]
delete = DELETE FROM qname [[AS] name] [USING from {"," from}] [WHERE expr] [RETURNING item {"," item}]
```

1. `UPDATE` is `Update { table, as, old: absent, from, filter, set, returning }` and `DELETE` is `Delete { table, as,
   old: absent, using, filter, returning }` ([QT] §9.4). `from`/`using` is the `FROM` list of §5.1 (comma items
   cross-joined); `filter` is the `WHERE`; each `SET` value is an assignment to its column (§5.9).
2. A `SET` target names a column of the table (`Q2003` when the table is known and has none). `SET c = DEFAULT`
   and assigning a parenthesised row of columns are `Q8005`.
3. The `FROM`/`USING` items and the target are one scope; a `FROM` item named as the target is `Q2004`.

### 3.4 CREATE TABLE

```
create_table = CREATE TABLE [IF NOT EXISTS] qname "(" element {"," element} ")"
element      = column | table_constraint
column       = name ( type | SMALLSERIAL | SERIAL | BIGSERIAL ) { column_constraint }
column_constraint
             = [CONSTRAINT name] ( NOT NULL | NULL | DEFAULT bexpr | PRIMARY KEY | UNIQUE | CHECK "(" expr ")"
             | REFERENCES qname ["(" name {"," name} ")"] {fk_option}
             | GENERATED ALWAYS AS "(" expr ")" STORED | GENERATED ( ALWAYS | BY DEFAULT ) AS IDENTITY )
table_constraint
             = [CONSTRAINT name] ( PRIMARY KEY names | UNIQUE names | CHECK "(" expr ")"
             | FOREIGN KEY names REFERENCES qname [names] {fk_option} )
fk_option    = ON DELETE action | ON UPDATE action | DEFERRABLE [INITIALLY (DEFERRED | IMMEDIATE)]
             | NOT DEFERRABLE | MATCH SIMPLE
action       = NO ACTION | RESTRICT | CASCADE | SET NULL | SET DEFAULT
```

1. It is `CreateTable { table, columns, primary_key, constraints, if_not_exists }` ([DDL] §3.2).
2. A column's type is `T` when it is `NOT NULL` or in the primary key, and `T?` otherwise ([QT] §2.2 rule 3).
   Writing both `NULL` and `NOT NULL`, or two `DEFAULT`s, or a column twice, or two primary keys, is `Q8013`.
3. `DEFAULT` is the column's `default`: a definition expression seeing no column ([DDL] §2.7 rule 1), its constants
   typed as the column (§5.9). `bexpr` is an expression without a top-level `AND`, `OR`, `NOT` or `IS`, so `DEFAULT 0 NOT NULL` reads as a default and a constraint; parenthesise anything else.
4. `GENERATED ALWAYS AS (e) STORED` is `generated`; `GENERATED ALWAYS AS IDENTITY` and `… BY DEFAULT AS IDENTITY`
   are `identity` `always` and `by_default`, `sequence` absent (defaulted, [DDL] §3.3 rule 1). `SMALLSERIAL`,
   `SERIAL` and `BIGSERIAL` are `int16`, `int32` and `int64` `NOT NULL GENERATED BY DEFAULT AS IDENTITY`. Identity
   options in parentheses are `Q8005`.
5. `PRIMARY KEY` (of a column, or of the table) is `primary_key`; it is always named `<table>_pkey` ([DDL] §2.4
   rule 3), so a `CONSTRAINT name` on it is `Q8005`.
6. `UNIQUE`, `CHECK` and `REFERENCES`/`FOREIGN KEY` are `ConstraintDef`s, in the order written. One without a
   `CONSTRAINT name` is named as PostgreSQL names it: `<table>_<columns>_key` (columns joined by `_`),
   `<table>_check`, `<table>_<columns>_fkey`, a later one that would repeat a name taking the suffix `1`, `2`, ….
   A column-level constraint's columns are that column.
7. `CHECK`, `GENERATED` and index expressions are definition expressions ([DDL] §2.12): the row's columns are read
   as `Col(row, c)`, written `c` or `<table>.c`.
8. `REFERENCES t` without columns references `t`'s primary key (`Q8010` when not known; a table referencing itself
   uses its own). `ON DELETE`/`ON UPDATE` default to `no_action`; `DEFERRABLE INITIALLY DEFERRED` is `deferred`;
   any other `MATCH` than `SIMPLE` is `Q8005`.

### 3.5 DROP, CREATE INDEX, CREATE SCHEMA, CREATE VIEW

```
drop          = DROP ( TABLE | VIEW | SCHEMA ) [IF EXISTS] qname [CASCADE | RESTRICT]
              | DROP INDEX [IF EXISTS] qname [RESTRICT]
create_index  = CREATE [UNIQUE] INDEX [IF NOT EXISTS] name ON qname "(" key {"," key} ")" [WHERE expr]
key           = ( name | "(" expr ")" ) [ASC | DESC]
create_schema = CREATE SCHEMA [IF NOT EXISTS] name
create_view   = CREATE [OR REPLACE] VIEW qname AS query
```

1. `DropTable { table, if_exists, cascade }`, `DropView`, `DropNamespace { name, … }` and `DropIndex { index,
   if_exists }` ([DDL] §3.2). A list of names is `Q8005`; `DROP INDEX … CASCADE` is `Q8005`.
2. `CreateIndex { name, def: { table, unique, keys, predicate }, if_not_exists }`: a column key is `column`, a
   parenthesised one `expr` (§3.4 rule 7); `NULLS FIRST`/`LAST` is `Q8005` (an index orders nulls itself, [DDL]
   §2.5 rule 1), and so is `CONCURRENTLY`. An index needs a name.
3. `CreateNamespace { name, if_not_exists }`; `CreateView { view, query, replace }`, the query per §5. A view
   column list is `Q8005`.

### 3.6 ALTER TABLE

```
alter_table = ALTER TABLE qname action {"," action}
action      = ADD [COLUMN] column | ADD table_constraint [NOT VALID]
            | DROP [COLUMN] name [CASCADE | RESTRICT] | DROP CONSTRAINT [IF EXISTS] name [RESTRICT]
            | RENAME [COLUMN] name TO name | RENAME CONSTRAINT name TO name | RENAME TO name
            | ALTER [COLUMN] name ( [SET DATA] TYPE type [USING expr] | SET DEFAULT bexpr | DROP DEFAULT
                                  | SET NOT NULL | DROP NOT NULL )
            | VALIDATE CONSTRAINT name
```

It is `AlterTable { table, actions }` with one `AlterAction` per action, in order ([DDL] §3.3):

| SQL | AlterAction |
|---|---|
| `ADD COLUMN c …` | `add_column` (the `ColumnDef` of §3.4), then an `add_constraint` for each of its `UNIQUE`, `CHECK` and `REFERENCES` |
| `ADD PRIMARY KEY (…)` | `set_primary_key` |
| `ADD` other constraint `[NOT VALID]` | `add_constraint { def, defer_validation }`, `defer_validation` set by `NOT VALID` |
| `DROP COLUMN c [CASCADE]` | `drop_column { c, cascade }` |
| `DROP CONSTRAINT [IF EXISTS] n` | `drop_constraint { n, if_exists }` |
| `RENAME COLUMN a TO b`, `RENAME CONSTRAINT a TO b`, `RENAME TO t` | `rename_column`, `rename_constraint`, `rename_table` |
| `ALTER c TYPE T [USING e]` | `retype_column { c, T or T?, e }`, keeping the column's nullability (`Q8010` when its type is not known); `e` a definition expression typed as the new type |
| `ALTER c SET DEFAULT e`, `DROP DEFAULT` | `set_default { c, e }`, `set_default { c, absent }` |
| `ALTER c SET NOT NULL`, `DROP NOT NULL` | `set_nullable { c, false }`, `set_nullable { c, true }` |
| `VALIDATE CONSTRAINT n` | `validate_constraint` |

`ADD COLUMN … PRIMARY KEY`, `ADD COLUMN IF NOT EXISTS`, `DROP COLUMN IF EXISTS`, `DROP CONSTRAINT … CASCADE`
and `ALTER TABLE IF EXISTS` are `Q8005`.

## 4. Types

### 4.1 Type names

| SQL | Query-tree type |
|---|---|
| `bool`, `boolean` | `bool` |
| `int8` | `int8` (8 bits: TernDB's name, **not** PostgreSQL's `bigint`) |
| `int16`, `smallint` | `int16` |
| `int32`, `int`, `integer` | `int32` |
| `int64`, `bigint` | `int64` |
| `int128` | `int128` |
| `uint8`, `uint16`, `uint32`, `uint64` | the unsigned integers |
| `decimal(p)`, `decimal(p, s)`, `numeric(…)` | `decimal(p, s)`, `s` 0 when absent; outside `1 ≤ p ≤ 38`, `0 ≤ s ≤ p` is `Q8006` |
| `float32`, `real` | `float32` |
| `float64`, `float`, `double precision`, `double` | `float64` |
| `text` | `text` |
| `bytes`, `bytea` | `bytes` |
| `json` | `json` |
| `date` | `date` |
| `time`, `time without time zone` | `time` |
| `datetime`, `timestamp without time zone` | `datetime` |
| `timestamp`, `timestamptz`, `timestamp with time zone` | `timestamp` |
| `interval` | `interval` |
| `uuid` | `uuid` |

1. `decimal` and `numeric` without a precision are `Q8006`: TernDB has no arbitrary-precision decimal ([QT] §2.1
   rule 1). `varchar`, `char` and `character` are `Q8006` (a length limit is a `CHECK`); a modifier on any other
   type (`timestamp(3)`), `time with time zone` and an unknown name are `Q8006`; array types are `Q8005`.
2. `int2`, `int4`, `float4`, `float8` and `jsonb` are not type names. `double` alone is `double precision`
   (Q76), so `double` followed by `precision` is one type name: `x::double precision` is a cast, never a cast
   to `double` with an output name `precision`.
   *Rationale: the dialect is TernDB's own, and its type names are the tree's. PostgreSQL's `int8` is a 64-bit
   integer and TernDB's an 8-bit one; accepting PostgreSQL's numbered names beside it would make `int8` mean the
   one thing in a dialect that otherwise reads like the other. `timestamp` names the instant, as in the tree,
   because `datetime` exists for the reading without a zone.*
3. Records, unions, relation types and named types have no SQL spelling in this version.

## 5. Queries and expressions

```
query     = [WITH cte {"," cte}] body [ORDER BY order {"," order}]
            [LIMIT (expr | ALL)] [OFFSET expr [ROW | ROWS]] [FETCH (FIRST | NEXT) [expr] (ROW | ROWS) (ONLY | WITH TIES)]
cte       = name ["(" name {"," name} ")"] AS "(" query ")"
body      = term { (UNION | EXCEPT) [ALL | DISTINCT] term }
term      = primary { INTERSECT [ALL | DISTINCT] primary }
primary   = select | VALUES row {"," row} | "(" query ")"
select    = SELECT [DISTINCT | ALL] item {"," item} [FROM from {"," from}] [WHERE expr]
            [GROUP BY expr {"," expr}] [HAVING expr]
item      = "*" | name "." "*" | expr [[AS] name]
from      = from_primary { [INNER | LEFT [OUTER] | RIGHT [OUTER] | FULL [OUTER]] JOIN from_primary (ON expr | USING names)
                         | CROSS JOIN from_primary }
from_primary = qname [[AS] name] | "(" query ")" [[AS] name [names]] | "(" from ")"
order     = expr [ASC | DESC] [NULLS (FIRST | LAST)]
```

### 5.1 SELECT

A `SELECT` is the tree below, bottom to top; a part that is absent leaves its node out.

1. `FROM`: each item is a `Scan(table, binder)`, a `CteRef(name, binder)` of a `WITH` name in scope, or a
   subquery's relation; joins are `Join(kind, left, right, on)` with `INNER`/`CROSS` → `inner`, `LEFT` → `left`,
   `RIGHT` → `right`, `FULL` → `full`; `CROSS JOIN` and comma-separated items have no `on`. `USING (c, …)` is `on =
   and(eq(l.c, r.c), …)`, and makes `c` one column of the join: unqualified `c` reads `l.c` (`r.c` for `RIGHT`,
   `coalesce(l.c, r.c)` for `FULL`), and `*` lists it once, first. Without `FROM` the input is `Values(one, [], [[]])`,
   one row of no columns.
2. `WHERE` is `Filter(input, predicate)`.
3. **Grouping** (§5.5): `Aggregate(input, group, keys, aggs)`, then `HAVING` as a `Filter`.
4. `ORDER BY` (§5.6) is a `Sort` here, **below** the select list, so it may read columns the list does not output.
5. `LIMIT`/`OFFSET` (§5.7) is a `Limit` here.
6. The select list is `Project(input, select, items)` (§5.4). A lone `*` over one source, a lone `*` over several
   whose columns are not all known, and a lone `t.*` of the only source add no `Project`: the query is its input.
7. `DISTINCT` is `Distinct` above the `Project`; with `DISTINCT`, `ORDER BY` and `LIMIT` go above it instead of rules
   4 and 5, and an `ORDER BY` key MUST then name an output column (§5.6).

### 5.2 Scopes and names

1. Each `SELECT` is a **level**; a subquery's level sees its own sources first, then the enclosing levels' (a
   **correlated** reference, [QT] §8.3). A subquery in `FROM` sees the enclosing queries' levels but not its
   sibling `FROM` items (there is no `LATERAL`).
2. A source is named by its alias, else its table (or CTE) name; two sources of one `FROM` with one name are
   `Q2004`. A subquery in `FROM` needs no alias; without one its columns are reached unqualified only.
3. `t.c` is column `c` of the innermost source named `t`: `Col(binder, c)`. No source `t` is `Q2002`; a known
   source without `c` is `Q2003`.
4. Unqualified `c`, level by level from the innermost: a `USING` column `c` (§5.1 rule 1); else the one known
   source that has `c` (two are `Q8008`); else, when the level has exactly one source and its columns are not
   known, that source; a level with an opaque source and no unambiguous answer is `Q8010`. A level with no
   candidate passes the search outward; none anywhere is `Q2003`.
5. `CHECK`, `GENERATED`, index and `USING` expressions of DDL see one source, the table, read through `row`
   (§3.4 rule 7).

### 5.3 VALUES

`VALUES (…), (…)` is `Values(values, [(column1, T₁), …], rows)`, a `list` ([QT] §6.2). Rows of different lengths
are `Q8012`. Column `j`'s type is the common type ([QT] §4.2) of its values' own types: a number's (§2.6), a
`TRUE`/`FALSE`'s, a typed constant's or cast's, a column's; `text` when only strings give one (a string is read as
the column's type when another value gives it); nullable when a value is `NULL`. A column whose values have no
common type is `Q3001`, and one where none has a type (all `NULL` or parameters) is `Q8007`. Each constant is then typed as its column (§5.9, as an assignment). Column names are `column1`, `column2`,
…, or those of a `WITH` or `FROM` alias list. An `ORDER BY`/`LIMIT` on it are a `Sort`/`Limit` above it over its
columns (§5.6).

### 5.4 The select list

1. Each item is `(name, expr)` of the `Project`. `t.*` is every column of `t` in order and `*` every column of
   every source in `FROM` order (`USING` columns first, §5.1); they need the columns (`Q8010`) unless rule 6 of
   §5.1 applies.
2. An item's name is its alias; else, for a column reference the column's name, for a function call the function's
   name as written, for a cast the name its operand would have if that is a column, call or cast; and otherwise
   `column<n>`, `n` the item's 1-based position.
3. A derived name (not an alias) that an alias or an earlier item already has is suffixed `_1`, `_2`, … (the first
   free one): `SELECT a.id, b.id` outputs `id` and `id_1`. Two equal aliases are left for validation (`Q2005`).
   *Rationale: [QT] §5 rule 5 requires unique output names, and PostgreSQL's duplicate names cannot be kept; a
   suffix keeps an unaliased query running, while two aliases the user chose are a mistake to report.*
4. A `WITH` or `FROM` column alias list renames the items in order; a list of another length is `Q8012`.

### 5.5 Grouping and aggregates

1. A `SELECT` is **grouped** when it has `GROUP BY` or `HAVING`, or an aggregate call in its select list or `ORDER
   BY`. Its `Aggregate` has binder `group`, keys `k1, k2, …` (the `GROUP BY` expressions, duplicates dropped) and
   aggregates `a1, a2, …` (each distinct call once, in the order first written).
2. A `GROUP BY` entry is an expression over the `FROM` sources; an integer constant `n` is the `n`-th select item
   (`Q8011` out of range); a bare name that is no input column but a select-list alias is that item. An aggregate
   in `GROUP BY` or `WHERE` is `Q3013`.
3. Above the `Aggregate`, an expression equal to a key reads that key, `Col(group, kᵢ)`; an aggregate call reads its
   column `Col(group, aᵢ)`; any other column of the input, at any depth (also from a correlated subquery), is
   `Q8009`.
4. The aggregates are `count(*)` → `count()`, `count(x)`, `sum`, `avg`, `min`, `max`, `bool_and`, `bool_or` and
   `string_agg`, each `AggCall { function, args, distinct, filter, order }` ([QT] §6.7): `DISTINCT` sets `distinct`,
   `FILTER (WHERE e)` `filter`, and `ORDER BY` inside the call `order`. An aggregate inside an aggregate's
   arguments is `Q3013`; `f(*)` for another `f` and an aggregate without arguments are `Q8001`.

### 5.6 ORDER BY

1. A key is `SortKey { expr, desc, nulls_first }`. `ASC` is the default. **`NULLS FIRST` is the default for `DESC`
   and `NULLS LAST` for `ASC`**, so nulls sort as if larger than every value, as in PostgreSQL.
2. Below the select list (§5.1 rule 4), an integer constant `n` is the `n`-th item's expression (`Q8011` out of
   range), a bare name that is an output name is that item's expression, and anything else is an expression over
   the input (or the groups).
3. Above a `DISTINCT`, a `VALUES` list or a set operation, a key MUST name an output column: by position, by name,
   or (for `DISTINCT`) as an expression equal to an item's. Anything else is `Q8011`; a position over output
   columns that are not known is `Q8010`.

### 5.7 LIMIT, OFFSET and FETCH

1. `LIMIT n`, `FETCH FIRST n ROWS ONLY` and `OFFSET m` are `Limit(input, count, offset, ties, arbitrary)`; `count` and
   `offset` are `int64` literals or `int64` parameters ([QT] §6.10 rule 1). `LIMIT ALL` and `LIMIT NULL` are no
   count, and `OFFSET NULL` no offset (with neither, no `Limit`); `FETCH FIRST ROWS` is a count of 1. Any other expression is `Q8005`.
2. **`arbitrary` is set exactly when the query has no `ORDER BY`**: SQL's unordered `LIMIT` takes any rows, which the
   tree requires to be said ([QT] §6.10 rule 2).
3. `WITH TIES` sets `ties` and requires `ORDER BY` (`Q8001`); the `Limit` is then directly above the `Sort`.
4. A query has at most one `ORDER BY`, one `LIMIT`/`FETCH` and one `OFFSET`, counting a parenthesised query's own
   (`Q8001`).

### 5.8 Operators and precedence

From lowest to highest; all binary operators are left-associative except the comparisons, which do not chain
(`a < b < c` is `Q8001`).

| Precedence | Operators | Tree |
|---|---|---|
| 1 | `OR` | `or` |
| 2 | `AND` | `and` |
| 3 | `NOT` | `not` |
| 4 | `IS [NOT] NULL` | `is_null`, `is_not_null` |
| 5 | `= <> < <= > >=` | `eq ne lt le gt ge` |
| 6 | `[NOT] BETWEEN a AND b`, `[NOT] IN (…)`, `[NOT] LIKE p [ESCAPE e]` | see below |
| 7 | `\|\|` | `concat` |
| 8 | `+ -` | `add sub` |
| 9 | `* / %` | `mul div mod` |
| 10 | unary `-`, `+` | `neg` (a negated number is a constant, §2.6) ; `+` is nothing |
| 11 | `::` | `Cast` |

1. `x BETWEEN a AND b` is `and(ge(x, a), le(x, b))`; `NOT BETWEEN` is `not` of that.
2. `x IN (a, b, …)` is `or(or(eq(x, a), eq(x, b)), …)` (one item: `eq(x, a)`), which has SQL's three-valued
   result; `NOT IN` is `not` of that. `x IN (query)` and `(x, y) IN (query)` are `In([x…], rel)` ([QT] §7.11);
   `NOT IN` is `not(In(…))`. A row constructor `(x, y)` anywhere else is `Q8005`.
3. `x LIKE p` is `like(x, p, '')`; with `ESCAPE e` it is `like(x, p, e)`. **There is no default escape
   character**, as in standard SQL (PostgreSQL's default is `\`).
4. `CAST(x AS T)` and `x::T` are `Cast(x, T?, strict)`: the target is nullable, because a nullable value casts
   only to a nullable type ([QT] §7.6) and the front end does not know whether `x` is one. A constant is the
   exception, read as a `T` where it can be (§5.9): `'2024-01-01'::date` is `Literal(date, …)`, `NULL::int64` is
   `Null(int64)`, `$1::int64` is `Param(0)`, and a constant that is not read so is `Cast(c, T, strict)`. A cast
   result assigned to a `NOT NULL` column is therefore refused (`Q3005`); `assert_not_null(CAST(x AS T))` is the
   non-null form.
5. `CASE WHEN c THEN v … [ELSE e] END` is `If([(c, v)…], e)`; `CASE x WHEN w THEN v …` compares with `eq(x, w)`.
   `ELSE NULL` is no `else`. A constant result takes the type of another result whose type is known.
6. `EXISTS (q)` is `Exists(rel)`; a parenthesised query as a value is `Scalar(rel)`.
7. **An integer beside a float widens.** In `+ - * / %` and in each comparison `= <> < <= > >=` (those `BETWEEN`,
   `IN (…)` and a simple `CASE` are made of included), after the constants are typed (§5.9), an operand of an
   integer type (signed or unsigned, any width) beside one of a float type (`float32`, `float64`) is
   `Cast(x, float64?, strict)`, or `Cast(x, float64, strict)` when `x` is a constant or a parameter declared
   non-null; a `NULL` of an integer type becomes `Null(float64)`. The tree refuses the mix ([QT] §4.4 rule 4);
   PostgreSQL converts `int8` to `float8` implicitly, and so does TernDB's SQL. The cast's target is nullable for
   the reason of rule 4, so a widened value assigned to a `NOT NULL` column is `Q3005` until wrapped in
   `assert_not_null`. An `int64` beyond 2⁵³ is rounded to the nearest `float64`, as in PostgreSQL.
   1. The front end widens only where it knows both operands' types: a constant, a cast, a parameter whose
      type is decided, a column whose type the resolver gives, a grouping key (its expression's type), a core
      aggregate (`count` is `int64`; `sum` is `int128` over integers, `float64` over floats; `avg` is a decimal
      over integers and decimals, `float64` over floats; `min`/`max` have the argument's type, [QT] §6.7 rule 4),
      and `-`, `coalesce` and arithmetic over such operands. Anything else is left as written, and validation
      decides.
   2. The integer operand is computed in its own type first, so `(2147483647 + 1) * 2.5e0` overflows (`R0001`)
      before it widens, while `2147483647 + 1 * 2.5e0` adds a widened `2147483647` to `2.5`.
   3. Nothing else widens: a decimal beside a float, `avg` of an integer beside a float, and every other mix the
      tree refuses stay `Q3002`; `||` is not arithmetic.
   *Rationale: the user chose PostgreSQL's reading on 2026-11-19 (Q73).*

### 5.9 Constants take their type from context

A **constant** is a number, a negated number, a string, `NULL` or a parameter. Where its context gives a type `T`,
it is written as a value of `T` rather than of its own type (§2.6):

1. **Assignment**: an `INSERT` value, an `UPDATE`/`ON CONFLICT` `SET` value, a `DEFAULT`, a `VALUES` value (its
   column's type), a cast. A number that is not a value of a numeric `T` (out of range, or written with a point or
   an exponent into an integer type, so `2.0` too) is `Q8004`; a decimal with more digits after the point than `T`'s scale is rounded half away from zero, as a
   cast is ([QT] §7.6). A string is read from `T`'s text form, `Q8004` when it is not one.
2. **Comparison**: a constant compared with, `BETWEEN`, `IN` or `LIKE` beside, `||`-joined to, or combined
   arithmetically with a column reference (or a cast or typed constant) takes that operand's type without
   nullability, when it is exactly a value of it; otherwise it keeps its own type. The same holds among
   `coalesce`'s arguments and `CASE` results.
3. `NULL` is `Null(T)`; without context it is `Q8007`. A string without context is `text`.
4. **A boolean context** gives `NULL` the type `bool`: written as the whole condition of `WHERE` (of a `SELECT`,
   `UPDATE`, `DELETE` or `ON CONFLICT … DO UPDATE`), `HAVING`, `JOIN … ON`, a searched `CASE`'s `WHEN`, an
   aggregate's `FILTER (WHERE …)`, or as an operand of `AND`, `OR` or `NOT`, it is `Null(bool)`. A filter keeps no
   row on it, and `NOT NULL` as an expression is `not(Null(bool))`, which is null. `NULL` anywhere else inside a
   condition (`NULL = 1`, `NULL IS NULL`, `NULL + 1 > 0`) still needs a context of rule 2 (`Q8007`).
   *Rationale: PostgreSQL accepts `WHERE NULL`; the user chose its reading on 2026-11-19 (Q75).*

*Rationale: the tree has no untyped constant and coerces implicitly only losslessly ([QT] §4.1), so `INSERT INTO t
(small) VALUES (1)` and `WHERE day = '2024-01-01'` would otherwise be type errors. Typing a constant by the
column beside it is what PostgreSQL's unknown-typed literals do, restricted to where the front end can see the
column.*

### 5.10 Functions

A call `f(args)` is `Call(f, args)` with `f` as written (folded), checked by validation ([QT] §7.12, `Q2007`), except:
the aggregates of §5.5; `now()` and `current_timestamp` (no parentheses), which are `statement_timestamp()`; and
`coalesce`, whose constants are typed by §5.9. `DISTINCT`, `ORDER BY` and `FILTER` on a function that is not an
aggregate are `Q8001`. A schema-qualified function name, `OVER` (window functions) and `WITHIN GROUP` are `Q8005`.

### 5.11 Set operations and WITH

1. `UNION`, `INTERSECT` and `EXCEPT` are `SetOp(op, all, select, inputs)` ([QT] §6.12); `INTERSECT` binds tighter.
   A chain of one operator with one `ALL` is flattened from the left into one `SetOp`. Output names are the first
   input's; `ORDER BY`/`LIMIT` follow §5.6 rule 3.
2. `WITH c [(names)] AS (q), … body` is `With(false, [CteDef(c, absent, rel)…], body)` ([QT] §6.13); each CTE is in
   scope in the later ones and the body, where `FROM c` is `CteRef(c, binder)`. A name twice in one `WITH` is
   `Q2004`. `WITH RECURSIVE` and `MATERIALIZED` are `Q8005`.

## 6. Transactions

| SQL | Request ([WIRE] §5) |
|---|---|
| `BEGIN [WORK \| TRANSACTION] [mode {, mode}]`, `START TRANSACTION [mode …]` | `Begin` with `TxnOptions` |
| `COMMIT [WORK \| TRANSACTION]`, `END` | `Commit` |
| `ROLLBACK [WORK \| TRANSACTION]`, `ABORT` | `Rollback` |
| `SAVEPOINT n` | `Savepoint(n)` |
| `RELEASE [SAVEPOINT] n` | `ReleaseSavepoint(n)` |
| `ROLLBACK [WORK \| TRANSACTION] TO [SAVEPOINT] n` | `RollbackToSavepoint(n)` |

1. A mode is `ISOLATION LEVEL l`, `READ ONLY` or `READ WRITE` (`TxnOptions.read_only`). `SERIALIZABLE` →
   `serializable`; `SNAPSHOT` and `REPEATABLE READ` → `snapshot`; `READ COMMITTED` and `READ UNCOMMITTED` →
   `read_committed`. Modes may be separated by commas or spaces; a mode given twice is `Q8001`. Fields not written
   are absent, taking the session's defaults ([WIRE] §5.2 rule 2).
   *Rationale: PostgreSQL's repeatable read is snapshot isolation, which TernDB calls by its name; reading
   uncommitted data is not a level TernDB has, and standard SQL lets a level be stronger than asked.*
2. The front end only names the request. Choosing the transaction handle, sending it, and running the other
   statements of the transaction on it are the client's.
3. `AND CHAIN` and `DEFERRABLE` modes are `Q8005`.

## 7. Errors

The front end's own codes are the `Q8…` group of `errors.md` §2. It also reports, before validation would, the
validation codes `Q2002` (no such table or alias in scope), `Q2003` (a known table has no such column), `Q2004`
(a name twice in one `FROM` or `WITH`), `Q3001` (a `VALUES` column without a common type) and `Q3013` (an aggregate
where none may be), and the tree's `Q1006` (nesting past the limit, §1 rule 8). Everything else is found by validation, with [QT] §11's codes.

| Code | When |
|---|---|
| `Q8001` | The parser met a token the grammar does not allow there, or the end of the text too early. |
| `Q8002` | A string, quoted identifier or block comment is not terminated. |
| `Q8003` | A character starts no token; a malformed number, parameter or escape; an empty quoted identifier. |
| `Q8004` | A constant that is not a value of its type: out of range, or a text that does not parse. |
| `Q8005` | A construct of SQL that the dialect does not have (§8). |
| `Q8006` | An unknown or refused type name, or a bad modifier (out of range, too large, or not an integer). |
| `Q8007` | The type of a `NULL`, parameter or `VALUES` column cannot be determined. |
| `Q8008` | An unqualified column belongs to more than one source. |
| `Q8009` | A column of a grouped query's input is read outside an aggregate and is not grouped by. |
| `Q8010` | A construct needs a table's columns (§1 rule 4) and they are not known. |
| `Q8011` | An `ORDER BY` or `GROUP BY` position out of range, or an `ORDER BY` that must name an output column and does not. |
| `Q8012` | A column list or `VALUES` row of the wrong length. |
| `Q8013` | An invalid table definition: two primary keys, a column twice, `NULL` and `NOT NULL`, two defaults. |

## 8. Out of scope for 0.0.1

Each of these is refused with `Q8005` (or `Q8006` for a type), so a script that uses one fails before it runs:

- window functions (`OVER`, `WINDOW`), `WITHIN GROUP`, grouping sets (`ROLLUP`, `CUBE`, `GROUPING SETS`);
- nested-relation syntax: records, unions, relation-valued columns, `collect`, `Nest`/`Unnest`, array types and
  constructors, `ROW(…)`;
- `WITH RECURSIVE`, `MATERIALIZED`, `LATERAL`, `NATURAL JOIN`, aliases on a parenthesised join, column aliases on a
  table, `ONLY`, table functions, `TABLE t`;
- `SELECT DISTINCT ON`, `SELECT INTO`, `ANY`/`SOME`/`ALL` comparisons, `IS [NOT] DISTINCT FROM`, `IS TRUE` and
  kin, `ILIKE`, `SIMILAR TO`, `BETWEEN SYMMETRIC`, `ORDER BY … USING`, `NULLIF`, the special syntax of `EXTRACT`,
  `POSITION`, `SUBSTRING` and `TRIM`;
- `DEFAULT` as a `VALUES` or `SET` value, row assignments in `SET`, data-modifying CTEs, `RETURNING` with `old`;
- `CREATE TABLE AS`, `LIKE`/`EXCLUDE` in `CREATE TABLE`, identity options, `CREATE INDEX CONCURRENTLY`, index
  `NULLS`, view column lists, sequences, named types, roles and privileges (`GRANT`, `REVOKE`), `DROP` of several
  objects, `ALTER` of anything but a table, `IF [NOT] EXISTS` on columns;
- `COMMIT AND CHAIN`, `DEFERRABLE` transactions, `SET`, `SHOW`, `EXPLAIN`, `COPY`, `PREPARE`, dollar-quoted
  strings, typed parameters other than by a cast or context, and source spans in envelopes (§1 rule 7).

These are features the tree or engine has (or will have) that the dialect does not spell yet; none needs a change
to the tree to add.
