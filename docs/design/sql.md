# The SQL front end

The SQL front end turns SQL text into what a client sends: tree envelopes ([`query-tree.md`](../spec/query-tree.md))
and transaction requests ([`wire-protocol.md`](../spec/wire-protocol.md) §5). It is a library in the client, used by
the `terndb` shell and later by the Postgres-wire layer; the server never sees SQL. The dialect is specified in
[`docs/spec/sql.md`](../spec/sql.md).

## Files

| File | What it holds |
|---|---|
| `sql_lexer.sysl` | Tokens, `SqlError` and its positions, `sql_complete` |
| `sql_parser.sysl` | The syntax tree and the recursive-descent parser, `sql_parse` |
| `sql_translate.sysl` | The translator, the resolver trait and the public API |
| `sql_print.sysl` | The printer: a tree back to SQL, `sql_definition_text`, `sql_query_text`, `sql_ident`, `sql_string` |

## API

```sysl
sql_statements(text: string, schema: string = "public", tables: &SqlTables = SqlNoTables())
    -> Result[Buf[SqlStatement], SqlError]
sql_statement(text: string, schema: string = "public", tables: &SqlTables = SqlNoTables())
    -> Result[SqlAction, SqlError]
sql_complete(text: string) -> bool

struct SqlStatement { start: u64, end: u64, action: SqlAction }   // byte range, without the `;`
enum SqlAction { SqlTree(envelope: Envelope), SqlControl(txn: SqlTxn) }
enum SqlTxn { SqlBegin(options: TxnOptions), SqlCommit, SqlRollback,
              SqlSavepoint(name), SqlRelease(name), SqlRollbackTo(name) }
struct SqlError { code: string, message: string, offset: u64, line: u64, column: u64 }

trait SqlTables { sql_table(self, name: QualName) -> Option[SqlTable] }
struct SqlTable { columns: Buf[Field], primary_key: Buf[string] }
```

- `sql_statements` translates a whole script, one `SqlStatement` per `;`-separated statement, empty ones skipped.
  The first error anywhere in the script is returned instead, its line and column 1-based, the column in
  characters. Nothing is executed, so a shell that wants psql's behaviour (run the statements before the bad one)
  translates each statement's text on its own, using the ranges of an earlier successful translation or
  `sql_complete` to cut the input.
- `sql_complete` is the shell's multi-line test: the text's last token outside strings, quoted identifiers and
  comments is `;`.
- An envelope's `params` are the `$n` the statement used, typed as `sql.md` §2.7 says; the shell binds them, or,
  having none to bind, refuses a statement that declares any.
- A `SqlControl` names the request; the shell picks the transaction handle, sends `Begin`/`Commit`/…, and runs the
  statements in between on that handle.

## The resolver

The translator needs no catalog, but it uses tables' column lists where SQL leaves to the database what a tree
spells out (`sql.md` §1 rule 4). `SqlTables` supplies them:

- `SqlNoTables` knows nothing: every table is opaque, and the constructs that need columns fail with `Q8010`.
- `MemCatalog` implements it, for an embedded engine and for the tests.
- The `terndb` shell answers it from the catalog relations ([DDL] §4) over its connection, in its open
  transaction: `catalog.tables` for the table and its primary key, `catalog.views`, and `catalog.columns` for each
  ready column's structural type. It keeps answers only for the rest of a transaction, and forgets them after DDL
  or a rollback to a savepoint (`shell.md` § The SQL front end).

A script's own `CREATE TABLE`, `ALTER TABLE` and `DROP TABLE` statements update the translator's view of their
tables for the statements after them, so `CREATE TABLE t (…); INSERT INTO t VALUES (…);` needs no round trip.

## How a statement is translated

`SqlTr` holds one script's state: the overlay of tables the script changed, and per statement the binders used, a
stack of scope levels and of CTEs, and the parameters' types. Each `SELECT` pushes a level holding its sources
(alias, binder, columns if known), its `USING` columns, and when grouped the `Aggregate`'s keys and the aggregate
calls collected so far. Column references search the levels from the innermost; a reference to a grouped level
outside an aggregate is rewritten to that level's key or refused (`Q8009`). Constants are typed by the column
beside them before they become literals (`sql.md` §5.9).

## Printing trees as SQL

```sysl
sql_definition_text(e: Expr, columns: Option[Buf[Field]], ty: Option[Type] = None, schema = "public") -> string
sql_query_text(r: Rel, tables: &SqlTables = SqlNoTables(), schema = "public") -> string
sql_ident(name: string) -> string
sql_string(s: string) -> string
```

`sql_print.sysl` writes a tree back as the dialect, for `\d` (`shell.md` § Meta-commands). Its contract is that
the text parses and translates back to the tree it came from, given the resolver it was printed with: a
definition expression through `CHECK (…)` over its table, whose ready columns are `columns`, and a query through
`CREATE VIEW`, the tables it scans described by `tables`.

- **Precedence.** Parentheses only where `sql.md` §5.8 needs them: a right operand of the same precedence
  (`a - (b - c)`), a comparison under a comparison (`(a < b) = c`), `-(-x)`, `NOT (a AND b)`.
- **Names.** An identifier is quoted only when it would not read back as itself: an upper-case letter, a reserved
  word, a character outside `[a-z0-9_$]` and non-ASCII, a leading digit or `$`; `"` is doubled. A definition's
  columns are written unqualified, a query's as `binder.column`, each source aliased by its binder.
- **Constants** are written so that the type the front end gives them (`sql.md` §5.9) is the literal's own. The
  printer mirrors the translator's `pair` and `sibling`: beside a column (whose type the resolver gives), a cast
  or a typed constant, a constant of that type is bare (`a = 5`, `d = '2024-01-01'`, `a = NULL`); with no such
  neighbour, a literal whose own type (§2.6) is its type is bare (`5`, `1.50`, `1.5e0`, `'x'`, `TRUE`); every
  other is written with its type, a number as `5::int64` or `(-5)::int64`, text as `'x'::text`, a float special
  as `'nan'::float64`, `NULL` as `NULL::int64`, and the other scalars as typed constants (`date '2024-02-29'`,
  `json '{}'`). A cast's or a unary minus's operand always carries its type, since a cast of a bare constant reads
  as a literal of the target and `-5` as one negated constant. Text with a control character is an `E'…'` string.
- **Operators.** `eq … ge`, `add … mod`, `concat` with two arguments, `neg`, `not`, `and`, `or`, `is_null`,
  `is_not_null` and three-argument `like` are their operators (`x NOT LIKE p`, `x NOT IN (q)` in place); `If`
  is a searched `CASE`; `statement_timestamp()` is `now()`; another call is `f(…)` when `f` reads back as a plain
  call. `BETWEEN` and `IN (list)` are never written: the tree they translate to (`and(ge, le)`, an `or` of `eq`)
  prints as itself and reads back the same.
- **Queries** are read top down as §5.1 builds a `SELECT` bottom up: `Distinct`, `Project`, `Limit`, `Sort`, a
  `Filter` over an `Aggregate` (`HAVING`), the `Aggregate` (`GROUP BY`, its key and aggregate columns written in
  place), a `Filter` (`WHERE`) and the `FROM` items (`Scan`, `CteRef`, joins, and anything else as `(query) AS
  binder`). A `Limit` or `Sort` over a `DISTINCT`, `VALUES` or set operation is its tail, naming output columns.
  `SetOp` inputs are each parenthesised; `With` is `WITH`; `Values` is `VALUES` with each constant typed.

**What the front end normalises**, so that the tree read back differs from the one printed:

1. A `Cast`'s target reads back nullable (`sql.md` §5.8 rule 4): `Cast(x, int64)` prints `x::int64`, which is
   `Cast(x, int64?)`.
2. A `Param` prints as `$n`, which takes its type from its context or fails `Q8007`; a parameter's declared type
   is the envelope's, which a printed expression does not carry.
3. Binders: the front end names its own (`select`, `group`, `subquery`, suffixes), so a query reads back equal up
   to binder names (its shape form, `query-tree.md` §10.2). An `Aggregate`'s keys and aggregates are renamed
   `k1…`, `a1…` in order of first use, and an aggregate written twice is one.
4. A `Null` of a nullable type reads back as the null of its base type; a `VALUES` column's type is the common
   type of its values, nullable only when one is `NULL`.
5. The first input of a `SetOp` that is itself the same set operation is flattened into it (§5.11 rule 1).
6. A `Select` whose shape §5.1 cannot build (a `Project` over a `Project`, a `Filter` over a `Sort`) is written
   with derived tables, which read back as the same nodes.

**What has no SQL spelling** and so prints in its §15 text form, that node alone (`sql.md` §4.1 rule 3, §8):
`Record`, `Field`, `Variant`, `Row`, `Relation`, `Match`, `Action`; a `try` cast and a cast to a record, union,
relation or named type; a `Null` of such a type; a call whose name is not a plain lower-case identifier, is
reserved, is an aggregate's or is `now`; `ParamScan`, `Apply`, `Window`, `Nest`, `Unnest`, `Tag`; a semi, anti,
mark or single join or one with a mark or binder; an outer join without `on`; `WITH RECURSIVE` and a `CteDef`
with columns; a `Limit` whose `arbitrary` disagrees with the `Sort` below it, or whose count is not an `int64`
literal or parameter; an `Aggregate` call SQL cannot write.
