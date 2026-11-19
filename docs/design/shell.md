# The shell

`terndb` is TernDB's interactive shell, modelled on `psql`. This note records the decisions behind it. The code
is `sh/sysl/terndb/shell*.sysl` in the library and `cli/main.sysl` for the program.

## The program

One binary, `terndb`, built from `cli/` with `sysl build cli` (through marshal). `cli/` is a project of its own
that depends on the package by path. It used to be called `server/`; it was renamed because it now holds more
than the server.

- `terndb serve [--socket p] [--port n] [--database name] [--password-file p]` is the server, with the flags and
  behaviour of `engine.md` § The server, unchanged.
- `terndb [options]` and `terndb shell [options]` run the shell:
  - `-h`/`--socket <path>`, or `TERNDB_SOCKET`: connect to the server on that Unix socket. The password is
    taken from `TERNDB_PASSWORD`, or else asked for on the terminal with echo off (`sysl.posix.tty.hidden`).
    Without a terminal and without `TERNDB_PASSWORD` the shell stops with status 2.
  - With no socket, the shell opens an **embedded** in-memory database in its own process, as the `duckdb` and
    `sqlite3` shells do. Its banner says that the data lasts only for the session.
  - `-d`/`--database` (default `terndb`), `-U`/`--user` (default `terndb`), `-c "<input>"` (run it, then
    exit), `-f <file>` (run the file, then exit), `--version` and `--help`.
  - Standard input that is not a terminal is read as a script, with no prompt and no banner.
- **Exit status**: 0 on success. 1 if any statement or meta-command failed in `-c`, `-f` or a script on standard
  input; the script keeps running after an error, as psql's does without `ON_ERROR_STOP`. 2 when the connection
  cannot be made, or when the command line is wrong. An interactive session exits with 0.

## Connections

Both modes go through the same client: a native-protocol `ClientSession` over a `Link`, which is a byte stream.
Over a socket, the link is the socket. Embedded, the link is a `ServerSession` over an `Engine` in the same
process: the shell's frames are reassembled and handed to the session exactly as `server.sysl` hands them over,
and the session's frames are queued for the shell to receive. So the protocol, authentication, transaction rules
and error reporting are the same in both modes, and only the transport differs. The embedded database logs the
shell in with SCRAM, using a random password made for the session. The shell sends one request at a time and
waits for its last response.

## Statements and lines

A statement may span any number of lines. The REPL never runs a line by itself: it adds each line to a statement
buffer, and runs a statement only once the buffer holds a `;` that ends it. A `;` ends a statement only when it is
in code. It does not end one inside a string literal (`'…'`, with `''` for a quote), a quoted identifier
(`"…"`, with `""`), a line comment (`--`) or a block comment (`/* … */`, which nests). One line may finish a
statement and begin the next, and may hold several statements; they run in order. A statement that is only
comments, or only a `;`, is dropped. Blank lines inside a statement belong to it; blank lines between statements
are ignored. This splitting is lexical, not a parse. Translating each complete statement is the SQL front end's
job.

**Meta-commands** take one line. A line whose first non-blank character is `\` is a meta-command when the buffer
is in code. It runs at once and leaves the buffer as it was, so `\x` typed halfway through a statement switches
the display and the statement continues. Inside an open string or comment, a `\` line is just text.

**End of input.** In a script (`-c`, `-f`, standard input, `\i`), a statement left without its `;` at the end is
run, as psql runs it. An unterminated string reaches the front end and is reported as an error there. In an
interactive session, Ctrl-D drops whatever was buffered and runs nothing, as psql does.

`\i` runs a file with a statement buffer of its own, so a statement left open at the prompt resumes afterwards.
Files may nest 16 deep. `\q` in a file ends the shell, as it does in psql.

## Prompts

The prompt is `<database><mark><txn>> `, which is psql's default `%/%R%x%> `:

| mark | meaning |
|---|---|
| `=` | the buffer is empty |
| `-` | a statement is being continued |
| `'` | inside a string literal |
| `"` | inside a quoted identifier |
| `*` | inside a block comment |
| `(` | inside parentheses |

`<txn>` is empty outside a transaction, `*` inside one, and `!` inside a failed one. So the prompts are
`terndb=> `, `terndb-> `, `terndb=*> ` and `terndb=!> `. The prompt always ends in `>`. psql would show `#` for
a superuser, but the bootstrap role is always one.

## Results

- **Aligned format**: psql's layout. Headings are centred (when the padding is odd, the extra space goes to the
  right), with a rule of `-` and `+` beneath, then the rows, then `(N rows)` (`(1 row)` for one row). Numeric
  columns are right-aligned and all others left-aligned. Widths are measured in terminal columns
  (`sysl.text.columns`): a wide character counts as two and a combining mark as none. Values are never truncated.
  Trailing blanks are dropped from every line. A result with no columns prints `--` and the footer.
- **NULL** shows as an empty cell.
- A **multi-line cell** (text containing line breaks, or a nested relation) makes its row as tall as the cell's
  line count, with the other cells blank on the extra lines. psql would mark continued lines with `+`; the shell
  does not, so that nested tables read cleanly.
- **Nested relations**: a relation of records is drawn as an indented sub-table inside its cell, with headings and
  a rule but no footer. An empty one shows its headings and rule. A relation of non-records, a record and a union
  use a compact form instead: `[1, 2]`, `{id: 1, name: 'ann'}`, and `ready` or `rebuilding {job: 7}` (a variant
  with an empty payload shows its name alone). Inside the compact form, text is single-quoted and null is
  written `null`.
- **Expanded display** (`\x`): one block per row. Each block opens with `-[ RECORD n ]`, ruled with `-` to the
  block's width, followed by one `heading | value` line per column. Extra lines of a value continue under its
  first line. No rows prints `(0 rows)`.
- **Command tags** are psql's: `INSERT 0 n`, `UPDATE n` and `DELETE n` (summed over parameter tuples), then the
  statement name for DDL (`CREATE TABLE`, `CREATE NAMESPACE`, …), and `BEGIN`, `COMMIT`, `ROLLBACK`, `SAVEPOINT`
  and `RELEASE`. DML with `returning` prints its rows, then its tag. A query has no tag; its footer stands in for
  one.
- **Errors** go to standard error as `ERROR:  <code>: <message>`, one line per error entry. When an entry carries
  a source position, a `LINE n:` excerpt of that line follows, with a `^` under the character. The column is
  measured in terminal columns. Front-end errors take the same form, and their code is omitted when they have
  none. Transaction misuse follows psql: `BEGIN` inside a transaction, and `COMMIT` or `ROLLBACK` outside one,
  print a `WARNING:` and the tag. `COMMIT` of a failed transaction rolls it back and prints `ROLLBACK`. A
  savepoint command outside a transaction is an error.

## Meta-commands

`\q`, `\?`, `\r` (forget the buffer), `\x [on|off]`, `\timing [on|off]` (prints `Time: n.nnn ms` after each
statement, including one that fails), `\i FILE` (the rest of the line, quotes optional), `\conninfo`, `\dt`,
`\d` and `\d NAME`. An unknown command prints `invalid command \x` and `Try \? for help.`, and counts as an error.

`\dt`, `\d` and `\d NAME` are queries over the catalog relations (`ddl.md` §4). They are built as query trees and
read their columns by name:

- `\dt` lists `catalog.tables`; `\d` lists tables and views (`catalog.views`). Both are titled, sorted by
  namespace and then name, and show namespace, name, type and owner. With nothing to list they print
  `Did not find any tables.` or `Did not find any relations.`
- `\d NAME` (or `NAMESPACE.NAME`) prints `Table "ns.name"` and then:
  - its columns from `catalog.columns`, in ordinal order, with type, `not null`, and the default (or the
    generated or identity clause);
  - `Primary key: (…)`;
  - `Indexes:` from `catalog.indexes`, with `UNIQUE`, key expressions, `DESC`, `WHERE`, and `BUILDING` or
    `INVALID` for an index that is not ready;
  - `Unique constraints:`, `Check constraints:` (with `NOT VALID` for unvalidated ones) and
    `Foreign-key constraints:` from `catalog.constraints`. Not-null and primary-key constraints are not listed
    there, since the columns and the key line already show them.

  A view prints its columns and its definition. A name that matches nothing is an error:
  `Did not find any relation named "x".`

## Line editing

Interactive input uses `sysl.term.edit`, the sysl library's line editor, with the terminal in raw mode while a
line is being read. It gives cursor movement, Home and End, Delete, Ctrl-U, Ctrl-K, and Up and Down through the
session's history (the last 64 lines). Two limits come from the library: the history is not saved between
sessions, and a line wider than the terminal redraws incorrectly.

## The SQL front end

The shell's statements are TernDB's SQL ([`sql.md`](../spec/sql.md)), translated by the SQL front end
([`docs/design/sql.md`](sql.md)) in the shell's process: the server never sees SQL. The shell takes SQL through the
`SqlFrontEnd` trait: the text of one complete statement and a resolver go in, and either a list of `ShellRequest`s
(a tree to run, or a transaction-control request) or the front end's `SqlError` comes out. `DialectFrontEnd` is
the dialect, over the default schema `public`, and `shell(…)` uses it unless it is given another front end; the
tests give one only to see the exact text handed over, or to run a tree SQL cannot write.

- **Errors** print as `ERROR:  <code>: <message>` and the `LINE n:` excerpt with its caret. The offset is relative
  to the statement's own text, which starts at its first non-blank character, so a statement that follows
  another on its line, or starts on a later line of a script, is excerpted from its own first line. A
  statement the front end refuses sends nothing: inside a transaction, the transaction stays open and is not
  failed, unlike psql, whose server is the one that finds the error.
- **The resolver.** SQL leaves to the database the column lists that a tree spells out (`sql.md` §1 rule 4: `*`
  beside other items or over several tables, unqualified columns over several tables, `INSERT` without a column
  list, a constant's type from a column). The shell answers the front end from the catalog relations
  (`catalog.tables`, `catalog.views` and `catalog.columns`, `ddl.md` §4), read over its own connection in its open
  transaction, so a table created earlier in the transaction is known. It translates, looks up every table the
  front end asked about and did not know, and translates again, until nothing new is asked. A column's type is
  read structurally (`catalog.columns.type`) and only ready columns count; the primary key is
  `catalog.tables.primary_key`. Names in the namespace `catalog` are answered from the catalog's own column
  lists, without a query.
- **What it remembers.** Outside a transaction every statement looks its tables up afresh, so DDL by another
  session is seen at once. Inside one, answers are kept until the transaction ends, until it runs DDL, or until a
  rollback to a savepoint. In a failed transaction, which can read nothing, the committed catalog is read in a
  transaction of its own, so the statement still translates and the server refuses it with `W2003`, as it
  refuses any statement there.
- **A missing table.** A table the catalog does not have is opaque to the front end. Where the statement needed
  its columns (an `INSERT` without a column list, say) the front end's `Q8010` is printed as the server would
  put it, `Q2001: no table or view 'ns.name'`, with the caret under the table's name. Where it did not (`SELECT * FROM t`), the tree
  is sent and the server reports the missing table.

`terndb` serves, and embeds, a database that starts with the namespace `public` (`sql_catalog`), owned by the
bootstrap role, so unqualified names work from the first statement.
