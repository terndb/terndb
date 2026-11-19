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
