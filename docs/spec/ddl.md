# TernDB data definition, the catalog and online schema change

**Status:** normative, draft. **Tree schema version:** 1.0 (statement kinds 32–63, type kind 32).

This document defines TernDB's schema: the objects the catalog holds, the DDL statements that create and change
them, the catalog as ordinary relations, how a table's rows survive a change of its columns, and how every change
runs online. DDL is sent as a tree, like every other statement: there is no SQL on the native protocol
(`docs/decisions.md` W11), so this document is the whole of DDL for a native client. TernDB's SQL dialect has its
own DDL syntax, defined with the SQL front end; it MUST produce the trees defined here and nothing else
(`query-tree.md` §1 rule 3).

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as in RFC 2119. Every MUST is meant to be
checkable by a test that sends trees, reads the catalog relations of §4 and inspects results, error codes, or the
deterministic simulation (`isolation.md` §7.3). *Rationale* text explains a choice and is not a rule.

Abbreviations: [QT] `query-tree.md`, [DISK] `on-disk-format.md`, [ISO] `isolation.md`, [WIRE] `wire-protocol.md`,
[ERR] `errors.md`. The errors this document raises are registered in [ERR]; new ones are the `Q6…` group of §2 there.

## 0. Principles

1. **A schema change never takes the table away.** No DDL statement waits for the table's readers or writers, and
   no operation in §8 blocks them. Work proportional to the table's size runs in a background job (§7) while the
   table stays readable and writable.
2. **Schema versions are metadata.** Every row records the schema version it was written in, and a row of an old
   version is translated when it is read (§5). Adding, dropping, renaming and widening a column rewrite nothing.
3. **DDL is transactional.** Catalog rows are MVCC rows ([DISK] §3.9), so DDL commits and rolls back with its
   transaction, and every snapshot sees one consistent catalog (§9).
4. **The catalog is data.** Every object is a row of a relation in the `catalog` namespace (§4), readable by an
   ordinary `Query`. A typed-client generator reads the schema the way any client reads a table.
5. **Types are structural.** A named type is an alias for a structure ([QT] §2.6); a result type never names one
   ([QT] §12.3).

## 1. Model

1. Every catalog object has an **oid**, a `uint64` below 2⁴⁸, never reused ([DISK] §3.9). Names are in
   **namespaces**; a table, view, index, constraint, sequence and named type is named by a `QualName` (namespace,
   name), unique within the namespace across all those kinds (`Q6002`). Roles and namespaces have unqualified
   names, unique among their kind.
2. A name is 1–255 octets of UTF-8 and compares byte-for-byte ([QT] §8.1); anything else is `Q6019`. The
   namespaces `sys` and `catalog` are **reserved**: they hold the system objects, and every DDL statement that
   would create, change or drop an object in them is `Q6014`.
3. The **catalog version** is the commit sequence number ([ISO] §4.1) of the newest committed transaction that
   changed the catalog. It is what [WIRE] §3.6 `Prepared.catalog_version` reports and what a plan cache keys on
   ([QT] §10.2).
4. Every table has a **schema version**, a `uint16` that starts at 1 when the table is created and increases by
   exactly one with each committed statement that changes its column list (§5.1). Changes to a table's indexes and
   constraints do not change its schema version.

## 2. Catalog objects

### 2.1 Namespaces

A namespace has a name and an owner. It is created by `CreateNamespace` and dropped by `DropNamespace`, which is
`Q6007` while the namespace holds any object, unless `cascade` is set, in which case every object in it is dropped
in the same statement, in dependency order (§2.11).

### 2.2 Tables

1. A table is a B+tree clustered on its primary key ([DISK] §3.3). It has an ordered list of columns, a primary key,
   and any number of secondary indexes and constraints.
2. **Every table MUST have a primary key** (`Q6009`). *Rationale: the table is clustered on it and every secondary
   index points at it ([DISK] §4.9), so a hidden key would be a key nobody can name; a generated client also needs a
   row identity, and an `identity` column (§2.7) costs one line.*
3. A table has at most 65,535 columns (`Q6011`), and its fixed area plus its widest key MUST fit the limit of [DISK]
   §4.7 rule 2 (`Q6011`).
4. A table's **smallest row** MUST fit the inline limit of [DISK] §4.7 rule 1 (`Q6022`): the row header, the key,
   the null bitmap, every fixed-width column at its full width and one `u16` offset for each variable-width column
   must total at most 4,000 octets, because none of these can be moved out of line however small the values are.
   Variable-width columns are `text`, `bytes`, `json`, records, unions, enums and relations (nested and list
   columns included). With no key and no fixed column that is 1,872 variable-width columns. `CreateTable` and
   `AlterTable` (`AddColumn`) refuse a statement that leaves the table past it; §2.13 rule 10 gives the count.

### 2.3 Columns

1. A column has a **column id** (a `uint32`, unique within the table for its whole life, never reused, and stable
   across renames and retypes), a name, a type, and optionally a default, a generation expression or an identity.
   The order of columns is their **ordinal** in the current schema version; `Scan` outputs them in that order
   ([QT] §6.1).
2. The type is any type of [QT] §2, including records, unions, relation types, `json` and every type of [QT]
   §2.7, or a named type reference ([QT] §12.3 kinds 32–34). Nullability is part of it ([QT] §2.2): a column of
   type `T` is `NOT NULL`, and one of type `T?` is not. A column's stored descriptor is the structure ([DISK] §4.3
   rule 2). A length, precision or qualifier is part of the type, so changing one is a retype (§8.5).
3. A column is in one of three **states**: `ready`, `filling` (being backfilled, §8.2) or `converting` (being
   retyped, §8.5). Only a `ready` column is visible to statements: a `Scan` does not output a column in another
   state, and naming one is `Q2003`. A writer still writes every column of its schema version, whatever its state
   (§5.4). That rule is for queries and definition expressions; what an `AlterTable` action may do to a column
   that is not `ready` is §2.13 rule 11.
4. A column of a character string type, or of a record, array or domain holding one, MAY declare a **collation**
   ([QT] §2.21), kept in `sys.columns.collation` (`ucs_basic` when none is declared). The tree does not apply it:
   the SQL front end writes it onto each comparison of the column by ISO's derivation rules (`sql.md` §4.2), and
   an index key on the column uses the collation the index declares, the column's by default. Changing it is a
   metadata-only change unless an index or constraint uses it, when it rebuilds those (§8.9).

### 2.4 The primary key

1. The primary key is a non-empty list of distinct columns of the table. Each MUST have a non-null type that may be
   a key component ([DISK] §4.6 rule 4): a scalar other than `json`, or an enum (§2.8). Anything else is `Q6005`.
2. Primary-key columns are stored in the key only ([DISK] §4). Changing the primary key, or the type of a key
   column, rebuilds the table (§8.11).
3. The primary key is also a constraint named `<table>_pkey`, listed in `catalog.constraints`, and it is the
   table's conflict target for an upsert ([QT] §9.3) like any unique constraint.

### 2.5 Secondary indexes

1. An index belongs to one table and has an ordered list of **key components**, each a column or an expression
   over the row, and each ascending or descending. A component's type MUST be a possible key component ([DISK]
   §4.6 rule 4), else `Q6005`. A nullable component is allowed; its nulls sort first ascending and last descending.
2. **Unique** indexes are allowed. Two rows conflict when every component is non-null and the components are equal
   by `same` ([QT] §3.1); a row with a null component never conflicts. A unique index is an upsert target.
3. **Partial** indexes are allowed: a `predicate` over the row; only rows for which it is `true` have entries. A
   query uses a partial index only when its own filter implies the predicate; the planner MAY decide implication
   conservatively. A partial unique index enforces uniqueness among the rows it covers.
4. **Expression** components are allowed. An expression is a **definition expression** (§2.12) and MUST be
   deterministic. An index on a field of a record column is an expression index (`Field(Col(row, c), f)`).
5. An index has a **state**: `building` while its job runs (§8.9), `ready` once complete, and `failed` when its
   job failed. Only a `ready` index answers queries or enforces uniqueness against committed data.
6. Not in v1: indexes over the elements of a nested relation, a scalar list or a `json` document (multi-valued
   indexes), covering columns, and per-index storage options.

### 2.6 Constraints

Every constraint has a name unique in the table's namespace, and is one of:

| Kind | Definition | Enforced when |
|---|---|---|
| not null | the column's type is non-null (§2.3); not a separate object, but listed in `catalog.constraints` | every write of the column |
| check | a deterministic boolean definition expression over the row (§2.12); a row satisfies it unless the expression is `false` (null passes, as in SQL) | every insert, and every update of a column the expression reads |
| unique | a list of columns; backed by a unique index of the same name, created with it | every write of a key column, through the index |
| primary key | §2.4 | as unique |
| foreign key | `columns` of this table referencing `ref_columns` of `ref_table`, which MUST be exactly the columns of a unique or primary-key constraint of `ref_table`, in order, with types identical apart from nullability (`Q6013`) | see below |

1. **Validity.** A constraint is either **validated** (every committed row satisfies it) or **not validated**
   (every write since the constraint was added satisfies it, and older rows have not yet been checked). A not
   validated constraint is enforced on every write exactly as a validated one is. It becomes validated through the
   job of §8.7. The planner MUST NOT rely on a not-validated constraint (for example to prove a predicate empty).
2. **Foreign keys** use simple matching: a row whose referencing columns include a null is not checked. Each foreign
   key has an `on_delete` and an `on_update` action, one of `no_action`, `restrict`, `cascade`, `set_null` and
   `set_default`, with SQL's meanings; `set_null` requires every referencing column to be nullable (`Q6013`). A
   foreign key is **immediate** (checked at the end of each statement) unless declared **deferred** (checked at
   commit). `restrict` is always checked at the end of the statement, even when deferred.
   - **What is checked.** A row inserted, or updated so that its referencing value changed, is checked for its
     referenced row; an update that leaves the referencing value as it was is not checked again. A row deleted,
     or updated so that its referenced value changed, runs `on_delete` or `on_update` for that value.
   - **When the actions run.** `cascade`, `set_null` and `set_default` run in the statement, after the statement's
     own writes of the referenced table, whether or not the key is deferred; the rows they change are written as
     if by the statement, so their own foreign keys act and are checked in turn (a cascade may reach any depth,
     and a self-referencing key acts on its own table). `restrict` is checked at that point, against the
     referencing rows that exist then. `no_action` is checked after every action of the statement has run (or at
     commit when deferred), and is satisfied when the removed value is held again by a row the transaction sees.
   - **Referencing checks see the statement's own rows**: a statement may insert a row and a row referencing it,
     in either order, or a row referencing itself. A deferred check reads the referencing row as it is at commit;
     a row deleted again before commit owes nothing.
   - `set_default` writes each referencing column's default (null when it has none); a default that no
     referenced row holds fails the check (`T3003`), and a null in a column that is not nullable is `T3004`.
3. **Foreign keys under isolation.** A check that a referenced row exists MUST hold at commit against concurrent
   deletes. At every level the check MUST take the referenced row's write lock as `LOCK_ONLY` ([DISK] §4.1), held to
   the end of the transaction, so a concurrent delete or key update of that row waits or fails under [ISO] §4.2; at
   serializable the check is in addition a point read of the referenced value, logged and validated ([ISO] §4.3).
   *Rationale: a log alone does not protect a serializable referencing transaction from a snapshot or read
   committed delete, which validates nothing and may commit after it (§3.4 allows weaker transactions to cause
   anomalies, but not to break a constraint).* The check reads the referenced value as an insert reads its key
   ([ISO] §4.2 rules 1, 2 and 4): a referenced row another live transaction is writing (other than by `LOCK_ONLY`)
   is waited for; one that holds the value only through a commit after the snapshot, or that held it in the
   snapshot and no longer does, is `serialization_failure` at snapshot and serializable; none is
   `foreign_key_violation`. A deferred check runs at commit, where a referenced row locked by another live
   transaction is `lock_timeout` at every level, a wait whose budget is already spent, since a commit does not wait. A delete or key update of
   a referenced row checks for referencing rows by a scan of the referencing table that is validated at
   serializable and sees the transaction's own rows and, for the rest, the newest committed versions, as a read
   committed write does ([ISO] §4.2 rule 3), so that a referencing row committed after the snapshot is not missed;
   at snapshot and serializable a referencing row found only through such a commit is `serialization_failure`. A
   referencing row another live transaction is inserting holds `LOCK_ONLY` on the referenced row, so the delete
   waits for it.
4. Constraint violations are the `T3…` codes ([ERR] §4): `T3001` unique, `T3002` check, `T3003` foreign key,
   `T3004` not null.
5. **Unique keys under isolation.** A row written with a non-null value of a unique key (a `unique` constraint or a
   unique index; the primary key is [ISO] §4.2 rule 4 itself) is checked as an insert of that value would be: a row
   holding the value that another live transaction is writing (other than by `LOCK_ONLY`) is waited for, its
   outcome deciding; one holding it in the transaction's own version or in its snapshot is `unique_violation`; one
   holding it only through a commit after the snapshot is `serialization_failure` at snapshot and serializable and
   `unique_violation` at read committed. The check runs once every write of a statement to the table has been
   applied (and again after each cascade's writes to it), so a statement may exchange or shift values among its
   rows, and a value a statement or an earlier statement of the transaction freed is free. A value with a NULL
   component is never checked (§2.5 rule 2), whichever of its components are NULL; its index entry takes the
   non-unique form ([DISK] §4.9 rule 1).

### 2.7 Defaults, generated columns, identity and sequences

1. A **default** is a definition expression (§2.12) of the column's type, evaluated once per inserted row that does
   not name the column ([QT] §9.2 rule 2). It MAY call the volatile functions `statement_timestamp()` and
   `next_value` ([QT] §7.12); it MUST NOT read any column. A column with no default defaults to null, and an
   insert that leaves a non-null column without a default unset is `Q3005`.
2. A **generated** column is **stored**: its value is computed from the row's other columns at every insert and at
   every update of a column it reads, and written like any other value. Its expression is a deterministic
   definition expression and MUST NOT read another generated column (`Q6006`). A generated column MUST NOT be named
   in an `Insert` or set in an `Update` (`Q5003`). Virtual (computed-on-read) columns are not in v1.
3. An **identity** column is an `int16`, `int32` or `int64` non-null column whose default draws from a sequence the
   column owns, created and dropped with it. `always` forbids naming the column in an `Insert` (`Q5003`);
   `by_default` lets an `Insert` supply a value instead.
4. A **sequence** has `start`, `increment` (non-zero), `min`, `max` (as `int64`), `cycle` and `cache` (≥ 1)
   (`Q6017` for an inconsistent set). `next_value(schema, name)` returns the next value. When the next value would
   pass `min` or `max`, it wraps if `cycle` is set and is otherwise `R0008`.
5. **Sequences are not transactional.** A value drawn is never drawn again, whether its transaction commits or
   rolls back, so identity values have gaps. `sys.sequence_state` ([DISK] §3.9) is updated by redo-only
   `NODE_PATCH` mini-transactions with no undo: before handing out a value at or beyond the logged `cached_to`, the
   engine MUST log a new `cached_to` = value + `cache` × `increment`. After recovery, drawing resumes at `cached_to`.
   Because a transaction's commit record follows in the WAL every reservation whose values it used, a value seen by
   a committed transaction is never reissued, even after a crash.
   *Rationale: a transactional sequence serialises every inserter on one row; every database that ships sequences
   makes them non-transactional for that reason.*
6. **A non-cycling sequence that hands out the last value of its range is exhausted**: it keeps that value as its
   `next` with the flag `exhausted` (`sys.sequence_state`, [DISK] §3.9), and every draw from it is `R0008` until an
   `AlterSequence` gives it a next value. A `restart` clears the flag; otherwise the next draw takes the value one
   `increment` past the last one when the altered range holds it, and wraps when `cycle` has been set (§2.13
   rule 8). A draw that wraps a cycling sequence takes `min` for a
   positive `increment` and `max` for a negative one.
7. **What draws, and when.** An insert draws an identity value for each row it leaves the column out of, in the
   order of its source's rows, before any row is written; a statement that then fails, or is blocked on a row lock
   and run again ([ISO] §4.2), has drawn them all the same. `next_value` in a query or DML expression draws once per
   row it is evaluated for, in row order, and not for a row where either argument is null, where it is null
   ([QT] §7.12). An identity column's draws need no privilege beyond the statement's own on the table, and a
   stored default's draws none beyond those its owner was checked for when it was stored (§2.10 rule 8); a
   `next_value` the statement itself writes needs `usage` on the sequence, checked at validation when it names the
   sequence by two text literals and at the draw otherwise, where a sequence that does not exist is `Q6003`.
8. **`AlterSequence` is transactional; the draws are not.** Where a sequence draws from — its next value and
   whether it is exhausted — is one state that every transaction shares (rule 5). An `AlterSequence` is DDL (§9):
   its options are seen by its own transaction's catalog reads at once and by others from its commit, it takes the
   sequence's catalog lock (§6.1) even when it only restarts it, and a rollback takes it back. **It moves no draw
   before it commits**: until then every draw from the sequence, its own transaction's included, takes the next
   value under the sequence's committed options from the shared state. At its commit the alter is applied to the
   shared state as draws have left it — a `restart` sets the next value, and otherwise a range that no longer holds
   it starts again at `start` (§2.13 rule 8) — and the following draws, by every transaction, use its options. A
   rolled-back alter leaves the shared state exactly as the draws left it. A sequence the transaction itself
   created has no committed state, so the transaction's alters of it and draws from it act at once, and a rollback
   drops it with them.
   *Rationale: a draw is seen by every transaction the moment it is made. A draw under an uncommitted alter would
   either make the alter visible to others or come from a private position whose values a rollback would leave
   for the shared state to hand out again; holding the alter's effect on draws until its commit keeps rule 5's
   guarantee and §9's isolation, the way a job a transaction starts waits for its commit (§9 rule 2).*

### 2.8 Named types

1. `CreateType` gives a name to a **record** or **union** type ([QT] §2.3–2.4), and, by its `def` (§3.3, tree
   1.1), defines a **domain**, a **range**, a **distinct** type or a **structured** type ([QT] §2.19–§2.20). An
   **enum** is a named union whose payloads are all `{}` (PostgreSQL's `CREATE TYPE … AS ENUM`); there is no
   separate enum type ([QT] §2.4 rule 1), and the catalog reports the kind `enum` for such a union (§4.2).
   Relation types and nullable types cannot be named, and a scalar only as a domain or a distinct type
   (`Q6005`).
2. A record, union, enum, domain or range name is an **alias**: a reference to it ([QT] §12.3 kind 32) denotes its
   structure, and type identity compares structures. A value of the named type and a value of the same structure
   written out are the same type. *Rationale: [QT] keeps every type structural so that results need no catalog to
   decode; a nominal type would make two clients with the same structure disagree about identity.* Distinct and
   structured types are the exceptions ISO requires: they are **nominal** (kinds 33, 34), and still sent as
   their structure, so results stay decodable without the catalog.
3. A column declared with a named type records it (`sys.columns.type_oid`, `catalog.columns.type_name`), so a
   client generator can emit one named class per catalog type. A column declared with the structure written out
   has no type name, even if a named type with that structure exists.
4. A named type MAY contain references to other named types, but not to itself, directly or through others
   (`Q6005`).
5. **Changing a named type** (`AlterType`, §3.2) changes every column declared with it. The permitted changes are
   the lossless ones of §5.3 rule 3 — append variants to a union, append nullable fields (or fields with a constant
   fill) to a record, rename a tag or field — and they run as a metadata-only retype (§8.5) of every such column, in
   one transaction, giving each affected table a new schema version. Any other change is `Q6018`: define a new
   type and retype the columns. A name or tag rename conflicts with every running transaction that resolved the
   old name (§6.3).
6. Dropping a named type that a column, another type or a view uses is `Q6007`.
7. **Enum values.** PostgreSQL's `ALTER TYPE … ADD VALUE 'v'` is `append_variants` with one `{}` variant, so the
   new label sorts last; `ADD VALUE … BEFORE`/`AFTER`, which would renumber the discriminants every stored value
   and key holds, is `Q6018`. `RENAME VALUE` is `rename_member`.
8. **Domains.** A domain over `T` has an optional default, a `not_null` flag and named checks, each a definition
   expression (§2.12) over the one binder `value` of type `T`. A column declared with the domain and no default
   of its own takes the domain's. Every value assigned to something declared with the domain ([QT] §2.20 rule 2)
   is checked: null against `not_null` (`T3004`), a non-null value against each check (`T3002`, which, as for a
   table's check, passes when the check is null). `AlterType` may add a check (`domain_add_check`), which is
   validated over every column declared with the domain by one `validate_constraint` job per table, as §8.7 adds
   a check; drop a check; set or drop the default; and set `not_null` (validated the same way) or drop it.
9. **Ranges.** `CREATE TYPE r AS RANGE (SUBTYPE = T)` names `range<T>` and, with it, `r`'s multirange name
   (`multirange_name`, else PostgreSQL's default: `r` with `range` replaced by `multirange`, or `r_multirange`)
   for `multirange<T>`. `T` must be orderable and key-encodable (`Q6005`). The PostgreSQL options
   `subtype_diff`, `canonical` and `collation` are not taken: a range over a discrete type is canonical by
   [QT] §2.19 rule 1.
10. **Distinct types.** The source is a predefined type of [QT] §2.1 or §2.7, not a named type (`Q6005`). A
    distinct type cannot be altered except renamed.
11. **Structured types.** `type` is the record of the type's own attributes; `under` names a supertype, which
    must be `NOT FINAL` (`Q6005`), and whose attributes come first. An attribute name that a supertype already
    has is `Q3015`. A new subtype appends a variant, with the hierarchy's attributes, to the structure of every
    column declared with the supertype or one of its supertypes: a metadata-only retype (§8.5 rule 1) of each,
    in the `CreateType`'s transaction. `AlterType` may append an attribute (`add_attribute`, nullable or with a
    constant fill), appended to every payload, and may set the type's ordering (`set_ordering`; `CREATE ORDERING
    FOR t EQUALS ONLY BY STATE`). Methods, reference types (`REF`) and typed tables are **deferred**, not
    refused for good (`docs/decisions.md` Q95); until they land their SQL is `Q8005`.

### 2.9 Views

1. A view is a named `Query` statement ([QT] §5) with no parameters. Its result type is the view's row type, and
   its columns are the root `Rel`'s output columns. A `Scan` of a view reads the stored query in its place, with the
   scan's binder bound to its output ([QT] §6.1).
2. Views are read-only: a DML target that is a view is `Q5003`. A view's stored tree names columns as
   `Col(binder, name)`, so renaming a column a view reads rewrites the view's stored tree in the same transaction
   (§8.4). Dropping or retyping a column a view reads is `Q6007` unless `cascade` drops the view.
3. A view is checked with the privileges of its owner; reading it needs `select` on the view only. The owner's
   privileges are checked again at each validation of a statement that reads the view, so a privilege revoked from
   the owner takes the view away from every reader (`Q6015`). A `next_value` in a view's query draws as the view's
   owner.
4. **How a scan of a view reads.** The stored query is planned on its own, as a statement of its own would be: its
   binders and CTE names are its own and never meet the reading statement's (a view may be scanned twice in one
   statement), it sees no outer row and no parameter, and its rows reach the scan's binder as the view's row type.
   It reads at the reading statement's snapshot, and a serializable read of it logs the reads of the scans inside
   it ([ISO] §4.4) with every column of those tables, since what the reading statement uses of the view is not
   traced through it. A view of a view expands the same way, as deep as views nest.

### 2.10 Roles and privileges

1. A **role** has a name, `login` (whether a connection may authenticate as it, [WIRE] §2.4), `superuser`, and a
   list of roles it is a member of; a member has every privilege of the roles it belongs to, transitively.
   Credentials are not part of the catalog defined here; the wire's authentication methods map a credential to a
   role. A login reads the newest committed catalog: a role it does not hold, or holds without `login`, fails
   authentication as an unknown user does (`W3001`), whatever its credentials. A connection runs every statement as
   the role it authenticated as; a later change to that role (losing `login`, being dropped) stops the next login,
   not the connection, whose statements are checked against their own catalog snapshot (rule 4). There is no
   statement that changes a connection's role.
2. Every object has an **owner** role, initially the role that created it. The owner, a member of the owner, and a
   superuser may alter or drop the object and grant privileges on it.
3. The privileges are: on a table or view, `select`, `insert`, `update`, `delete`, `references` (needed to create a
   foreign key to it); on a namespace, `usage` (to name objects in it) and `create`; on a sequence, `usage` (to call
   `next_value`); on a named type, `usage`. `Grant` and `Revoke` change them; `grantable` lets the grantee grant on.
4. Privileges are checked at validation, against the statement's catalog snapshot: a missing one is `Q6015`. A
   `Revoke` does not abort a running transaction.
5. Every role may read every `catalog` relation (§4). Not in v1: column privileges, row-level security, default
   privileges.
6. Every database has a predefined role `terndb_replication`, without `login`, which cannot be dropped or
   renamed. A role has the **replication right** — it may stream the WAL, take a base backup, rewind or fence
   (`replication.md` §2.2) — iff it is a superuser or a member of `terndb_replication`.
7. **A new database**, embedded or served, holds the superuser `terndb` with `login` (`c-api.md` §4.2 rule 2),
   the role `terndb_replication`, and the namespaces `sys` and `catalog`, both owned by `terndb`. Dropping,
   renaming or giving `login` to `terndb_replication` is `Q6014`.
8. **What each statement needs** (`Q6015` when the statement's role lacks it):
   - `CreateNamespace`, `CreateRole`, `AlterRole` and `DropRole`: a superuser.
   - creating a table, view, sequence or named type: `create` on its namespace. `CreateIndex`, every `AlterTable`
     action and every `Alter…` or `Drop…` of an object: ownership of the object (of the table, for an index),
     rule 2. `CreateView` with `replace` also needs ownership of the view it replaces.
   - naming an existing object of a namespace — a column's named type, a foreign key's `ref_table`, a view
     query's scans, the sequence a stored `next_value` names — needs `usage` on that namespace and the object's
     own privilege: `usage` on the type or sequence, `references` on the referenced table, `select` on each table
     or view a view scans. Every role may name objects of `sys` and `catalog` (rule 5).
   - `Grant` and `Revoke` of a privilege: ownership of the object, or that privilege held `grantable`. Revoking a
     privilege the grantee does not hold changes nothing.
   - a `Query`, `Insert`, `Update` or `Delete`: `usage` on the namespace of every object it names; `select` on each
     table or view it scans, but not on what a view scans, which is checked for the view's owner (§2.9 rule 3);
     on its DML target `insert`, `update` or `delete`, an upsert whose action is `update` needing both `insert` and
     `update`; and `usage` on each sequence a `next_value` of its own names (§2.7 rule 7). The target itself
     needs no `select`, even where the statement's filter or `returning` reads it.
9. A privilege that does not apply to its object's kind (rule 3) is `Q6021`. A `member_of` naming no role is
   `Q6003`, and a membership that would make a role a member of itself, directly or through others, is `Q6020`. A
   role that owns an object, holds or granted a privilege, or has members cannot be dropped (`Q6007`).

### 2.11 Dependencies

`sys.depends` ([DISK] §3.9) records that one object's definition refers to another: an index or constraint to its
table, a foreign key to its referenced table, a view to each object it reads, a column's named type, a type to the
types it refers to, an identity sequence to its column's table. Column-level dependencies are read from the
definitions themselves, which name columns. A `Drop…` or `drop_column` whose target has dependents is `Q6007`
unless `cascade` is set, in which case the dependents are dropped first, in the same statement. `cascade` never
drops a table or a column: a dependent table (through a foreign key) loses only the foreign key.

### 2.12 Definition expressions

A **definition expression** is an [QT] expression stored in the catalog: a default, a check, a generated column, an
index component or a partial-index predicate.

1. The row is visible through the one binder `row`, which no node introduces; `Col(row, c)` names column `c` of the
   table (a `ready` column). Nothing else is visible.
2. It MUST NOT contain `Param`, `Relation`, `Scalar`, `Exists`, `In` over a relation, `Action`, an aggregate or a
   window call (`Q6006`). Literals and named type references are allowed.
3. **Deterministic** means calling only functions that the function catalog does not mark volatile; in [QT] §7.12,
   `statement_timestamp` and `next_value` are the volatile ones. Checks, generated columns and index components
   MUST be deterministic (`Q6006`).
4. It is stored in the canonical encoding ([QT] §12) of the expression, with `row` as its binder, and reported in
   `catalog.*` in the text form ([QT] §15).
5. A stored `next_value` names its sequence by two text `Literal`s, so that the dependency can be recorded
   (§2.11); any other argument is `Q6006`, and a sequence that does not exist is `Q6003`.
6. It is typed as [QT] types an expression: a default against its column's type, a generated column or a `using`
   coercing to its column's type (`Q3001`), a check or predicate as a condition (`Q3004`), naming only the ready
   columns of the table through `row` (`Q2003` for any other).

### 2.13 Rules the catalog applies

These settle what §2 leaves to the catalog; each is checked when the statement executes (§3.5).

1. **A table where a table is required.** `AlterTable`, `DropTable` and `IndexDef.table` naming a view, or
   nothing, are `Q2001`; `DropView` naming a table is `Q6003`; a foreign key whose `ref_table` is a view is
   `Q6013`. A column a statement names that its table does not have is `Q2003`, and a column added under a name
   the table already has is `Q6002`.
2. **The width limit** (§2.2 rule 3) counts each fixed-width column ([DISK] §4.4) that is not in the key at its
   width, and each primary-key column at its key width ([DISK] §4.6: an enum takes 2 octets, `text` and `bytes`
   0, since their length is bounded per entry when a row is written). A `CreateTable` or `AlterTable` whose
   table would pass 3,000 octets is `Q6011`.
3. **The names a table takes.** The primary key is the constraint `<table>_pkey` (§2.4 rule 3) and an identity
   column's sequence is `<table>_<column>_seq`, both in the table's namespace, and each name must be free
   (`Q6002`). A unique constraint and its index share one name. Renaming a table renames neither.
4. **Dependencies the definitions add** (§2.11): a definition that calls `next_value` depends on that sequence, so
   a sequence a definition draws from cannot be dropped, nor the table owning an identity sequence another
   table's definition draws from (`Q6007`). An identity's sequence is dropped with its column, and `DropSequence`
   of it is `Q6007`.
5. **`drop_column`** of a column that a generated column reads is `Q6007` even with `cascade`, since `cascade`
   never drops a column (§2.11, §8.3). With `cascade`, an index that uses the column is dropped, and a constraint
   that uses it (with its index, and the foreign keys that reference its key). An index backing a constraint is
   dropped only with the constraint (`DropIndex` of it is `Q6007`).
6. **What a change leaves.** After an `AlterTable` or `AlterType`, every view is typed again and must have exactly
   the columns it had, and every definition expression of every table must still type, its index key components
   still key components; anything else is `Q6007`. So a column rename or a dropped `not null` whose column a view
   outputs as one of its own columns is `Q6007`, as is an `AlterType` change that a definition or view depends on
   (a renamed field a check reads, a payload added to an enum an index keys on).
7. **`AlterType`** on a type whose definition is not of the kind the change needs (a union for `append_variants`,
   a record for `append_fields`, either for `rename_member`) is `Q6018`. A member added under a name the type
   already has is `Q6002`, a variant payload that is not a record `Q3014`, a fill that does not coerce to its
   field `Q3001`, and a `rename_member` of a member the type lacks `Q6003`. A change that would make the type
   refer to itself, directly or through others, is `Q6005`. The structural changes give every table with a column
   whose type reaches the type a new schema version; `rename` changes only names, which no row encoding holds,
   and adds none. The catalog keeps the fill of each appended field with the type, converted to the field's type
   ([QT] §7.6), and a value of the type stored before the append reads that fill for the field, or null where it
   has none (§5.3 rule 3), wherever in a column the value lies.
8. **`AlterSequence`** may change a sequence's `type` unless an identity owns it (`Q6017`). A `restart` outside the
   resulting range is `Q6017`; a next value the new range excludes restarts from `start`.
9. **`retype_column`** of a column a view reads through a scan of its table is `Q6007`, whether or not the view
   outputs it (§2.9 rule 2). A conversion that is lossless (§5.3 rule 3), has no `using` and leaves the encoding of
   every key the column is in unchanged gives the column its new type at once; any other makes it `converting`
   (§8.5 rule 2), keeping its type and recording the target and the conversion. Either adds a schema version, the
   column's type or state being part of it (§5.1 rule 1). A `retype_column` of a column that is still `converting`
   from an earlier statement is `Q6023` (§8.5 rule 4), checked before the conversion is looked at, so it is never
   `Q6008`; the statement has no effect when refused.
10. **The smallest-row limit** (§2.2 rule 4) is `min = 20 + key + ⌈n / 8⌉ + fixed + 2v` octets, where 20 is the
    row header ([DISK] §4.1), `key` is the sum of the key widths of the primary-key columns (rule 2), n counts the
    columns that are not in the primary key (fixed-width and variable-width, as the current schema version has
    them), `fixed` is the width of the fixed-width ones, and v counts the variable-width ones. The statement is
    `Q6022` when `min` exceeds the inline limit (4,000 octets, [DISK] §4.7 rule 1, derived from the engine's row
    constants and never written into a statement). A column with a default or a null counts the same; a column
    being dropped by the same `AlterTable` does not count. A column being retyped (§8.5) is counted at its new
    type as well as at its current one: the statement is `Q6022` when `min` exceeds the limit with every column
    at its current type, and again when it exceeds it with every `converting` column (one this statement makes
    `converting` or one still converting from an earlier statement) at its target type, so the table fits before
    and after each conversion finishes. A retype to a type that makes the smallest row larger (an int8 to text, a text to an int64) of
    a table with no room is refused here, before anything is recorded; a retype that does not enlarge it (a text to
    an int8, an int64 to an int8) never is, because the table already fitted.
    `Q6011` (rule 2) is checked first. The statement has no effect when refused.
11. **Actions on a column that is not `ready`.** An `AlterTable` action naming a column in state `converting` or
    `filling` finds it by name (it is the statement, not a query, so `Q2003` of §2.3 rule 3 does not apply to it), and
    the actions of one statement apply in order. A statement never sees a column its own earlier action made
    `converting` or `filling`, because a name touched by two actions is `Q6016` (§3.3 rule 3): the state is the one
    the column had before the statement.
    - `retype_column` of a `converting` column is `Q6023` (§8.5 rule 4); of a `filling` column `Q6024` (§8.5 rule 5).
    - `drop_column` of a `converting` column is allowed and abandons its conversion (§8.3).
    - `rename_column` of a `converting` column is allowed and the conversion continues under the new name (§8.4).
    - `set_default` of a `converting` column is allowed only for a default valid at both its current type and its
      target type; a default that is not is `Q3001` as for any column (§8.6).
    A refused action leaves the catalog unchanged and the statement's other actions unapplied. A column that is
    `filling` takes `drop_column`, `rename_column` and `set_default` as a `ready` one does.

## 3. DDL statement trees

### 3.1 Common rules

1. A DDL statement is the `statement` of an [QT] §12.2 envelope, encoded as a `node` whose kind is in §3.2. Fields
   are encoded in the order listed, with the conventions of [QT] §12.4: a name is `str`, a `QualName` is `str
   schema, str name`, a `[X]` is `list<X>`, an `X?` is `opt<X>`, a `bool` is one octet, an enum is a `uvar`
   ordinal in the order listed, and a choice is an enum followed by the chosen alternative's fields. Types are
   [QT] §12.3 `Type` nodes and expressions are [QT] `Expr` nodes. An `svar` holds an `int64` and a `uvar` a
   `uint64`; the statement node is at depth 0 and everything it holds directly at depth 1 ([QT] §12, `Q1006`).
2. A DDL envelope MUST declare no parameters, and its execution MUST bind exactly one empty tuple (`Q6001`).
   Constants are `Literal` nodes ([QT] §7.3). *Rationale: a definition is stored, so a value that varies between
   executions has no meaning in it.*
3. A DDL statement returns **no value** ([QT] §5 rule 2): it has no result type, sends no `Batch`, and its row count
   in `Complete.counts` is 0 ([WIRE] §4.3). The exceptions are `AwaitJobs`, which returns a relation (§3.2), and
   `CancelJob`, which returns none.
4. Validation runs in the phases of [QT] §11.2: decode, then names (unknown objects `Q2001`, `Q6003`, `Q6004`;
   existing names `Q6002`; privileges `Q6015`), then types (`Q3…`, `Q6005`, `Q6008`), then structure (`Q6006`,
   `Q6007`, `Q6009`–`Q6019`). Names are resolved against the statement's catalog snapshot, which includes the
   transaction's own earlier DDL (§9).
5. `if_exists` makes a drop of a missing object succeed with no effect; `if_not_exists` makes a create of an
   existing name succeed with no effect, whatever the existing object is. Neither suppresses any other error.
6. A DDL statement is shape-keyed like any statement ([QT] §10.2), but a server SHOULD NOT cache its plan. Its
   shape form renames only the binders that nodes inside it introduce (a view query's, a `Match` arm's) and the
   CTE names of a view query. The binder `row` of a definition expression (§2.12) is introduced by no node, so it
   is kept; object, column, constraint, role and member names, every option and every literal are kept too. Two
   DDL statements therefore share a shape only when they differ in nothing but those binders and CTE names, and
   a DDL statement with no such binder has its encoding as its shape form.

### 3.2 Statement kinds

| Kind | Statement | Fields |
|---:|---|---|
| 32 | `CreateNamespace` | `name: str, if_not_exists: bool` |
| 33 | `DropNamespace` | `name: str, if_exists: bool, cascade: bool` |
| 34 | `CreateTable` | `table: QualName, columns: [ColumnDef], primary_key: [str], constraints: [ConstraintDef], if_not_exists: bool` |
| 35 | `DropTable` | `table: QualName, if_exists: bool, cascade: bool` |
| 36 | `AlterTable` | `table: QualName, actions: [AlterAction]` |
| 37 | `CreateIndex` | `name: str, def: IndexDef, if_not_exists: bool` |
| 38 | `DropIndex` | `index: QualName, if_exists: bool` |
| 39 | `CreateType` | `type_name: QualName, type: Type, if_not_exists: bool, def: TypeDef` (`def` from tree 1.1; absent, `alias`) |
| 40 | `AlterType` | `type_name: QualName, change: TypeChange` |
| 41 | `DropType` | `type_name: QualName, if_exists: bool` |
| 42 | `CreateSequence` | `sequence: QualName, def: SequenceDef, if_not_exists: bool` |
| 43 | `AlterSequence` | `sequence: QualName, def: SequenceDef, restart: opt<svar>` |
| 44 | `DropSequence` | `sequence: QualName, if_exists: bool` |
| 45 | `CreateView` | `view: QualName, query: node Query, replace: bool` |
| 46 | `DropView` | `view: QualName, if_exists: bool, cascade: bool` |
| 47 | `CreateRole` | `name: str, def: RoleDef` |
| 48 | `AlterRole` | `name: str, def: RoleDef, new_name: opt<str>` |
| 49 | `DropRole` | `name: str, if_exists: bool` |
| 50 | `Grant` | `privileges: [Privilege], object: ObjectRef, grantee: str, grantable: bool` |
| 51 | `Revoke` | `privileges: [Privilege], object: ObjectRef, grantee: str` |
| 52 | `AwaitJobs` | `txid: uvar` |
| 53 | `CancelJob` | `job: uvar` |
| 54–63 | reserved | |

1. `CreateIndex.name` is in the namespace of `def.table`. A `CreateTable` creates the table with every constraint
   validated (the table is empty) and every index ready.
2. `CreateView` with `replace` replaces an existing view of that name only if the new result type's columns begin
   with the old view's columns, by name and identical type (`Q6007` otherwise); a dependent view stays valid.
3. `AlterSequence` changes the options given in `def` that differ from the defaults of `SequenceDef`; `restart`,
   when present, sets the next value.
4. `AwaitJobs` waits until every job started by the committed transaction `txid` (§7) has finished, then returns
   `bag<{job: uint64, kind: text, object: uint64, state: union<succeeded: {}, failed: {code: text, message: text},
   cancelled: {}>}>`, one row per job; an unknown `txid` or one with no jobs returns the empty relation. It MUST NOT
   run inside an explicit transaction (`Q6010`), since it would hold a snapshot for the length of a job. It is
   cancellable ([WIRE] §7.3) without affecting the jobs.
5. `CancelJob` cancels a queued or running job (§7 rule 6); a finished or unknown job is `Q6003`.
6. `CreateView.query` is a [QT] §12.5 statement node of kind 1 (`Query`) whose payload is the view's relation; it
   counts as a node for span indexes ([QT] §12.2), its relation is at depth 2, and any other kind there is
   `Q1002`.

### 3.3 Auxiliary records

Auxiliary records are encoded inline, as their fields in order ([QT] §12.4).

```
ColumnDef     = { name: str, type: Type, default: Expr?, generated: Expr?,
                  identity: enum { none, always, by_default }, sequence: SequenceDef?,
                  collation: opt<str> }                                (collation: tree 1.1, §2.3 rule 4)
ConstraintDef = { name: str, body: choice {
                    check { expr: Expr },
                    unique { columns: [str] },
                    foreign_key { columns: [str], ref_table: QualName, ref_columns: [str],
                                  on_delete: FkAction, on_update: FkAction, deferred: bool } } }
FkAction      = enum { no_action, restrict, cascade, set_null, set_default }
IndexDef      = { table: QualName, unique: bool, keys: [IndexKey], predicate: Expr? }
IndexKey      = { key: choice { column { name: str }, expr { expr: Expr } }, descending: bool,
                  collation: opt<str> }                                (collation: tree 1.1)
SequenceDef   = { type: Type, start: opt<svar>, increment: opt<svar>, min: opt<svar>, max: opt<svar>,
                  cycle: bool, cache: opt<uvar> }
TypeChange    = choice { append_variants { variants: [(str, Type)] },
                         append_fields { fields: [(str, Type, Expr?)] },
                         rename_member { old: str, new: str },
                         rename { new_name: str },
                         domain_add_check { name: str, expr: Expr },          (tree 1.1, 4–9)
                         domain_drop_check { name: str },
                         domain_set_default { default: Expr? },
                         domain_set_not_null { not_null: bool },
                         add_attribute { name: str, type: Type, fill: Expr? },
                         set_ordering { equals_only_by_state: bool } }
TypeDef       = choice { alias {},
                         domain { default: Expr?, not_null: bool, checks: [(str, Expr)] },
                         range { multirange_name: opt<str> },
                         distinct {},
                         structured { under: opt<QualName>, instantiable: bool, final: bool } }
RoleDef       = { login: bool, superuser: bool, member_of: [str] }
Privilege     = enum { select, insert, update, delete, references, usage, create }
ObjectRef     = choice { table { name: QualName }, namespace { name: str },
                         sequence { name: QualName }, type { name: QualName } }
AlterAction   = choice {
    add_column { def: ColumnDef },                                      (0)
    drop_column { name: str, cascade: bool },                           (1)
    rename_column { name: str, new_name: str },                         (2)
    retype_column { name: str, type: Type, using: Expr? },              (3)
    set_default { name: str, default: Expr? },                          (4)  absent: drop the default
    set_nullable { name: str, nullable: bool },                         (5)
    add_constraint { def: ConstraintDef, defer_validation: bool },      (6)
    drop_constraint { name: str, if_exists: bool },                     (7)
    validate_constraint { name: str },                                  (8)
    rename_constraint { name: str, new_name: str },                     (9)
    rename_table { new_name: str },                                     (10)
    set_primary_key { columns: [str] },                                 (11)
    rewrite { },                                                        (12)
    set_owner { role: str } }                                           (13)
```

1. `ColumnDef`: at most one of `default`, `generated` and a non-`none` `identity` (`Q6006`); `sequence` only with
   an identity (its `type` MUST equal the column's), and defaulted when absent. Absent `SequenceDef` options take
   `start` 1, `increment` 1, `min` 1 (or the type's minimum when `increment` < 0), `max` the type's maximum, and
   `cache` 32.
2. In `append_fields`, the `Expr?` is the fill of existing values: a constant `Literal` (or absent for null, which
   requires a nullable field type). A non-constant fill is `Q6006`.
3. `AlterTable` applies its actions in order, as one statement: one new schema version if any action changes the
   column list, and one job per action that needs one (§7). Two actions on the same column, other than a rename
   followed by others naming the new name, are `Q6016`. Precisely: each of `add_column`, `drop_column`,
   `rename_column`, `retype_column`, `set_default` and `set_nullable` **touches** the column name it names (for
   `add_column`, `def.name`; for `rename_column`, the old `name`), and a name touched by two actions is `Q6016`.
   `rename_column` leaves its `new_name` untouched, so one later action may name it; a `rename_column` whose
   `new_name` an earlier action touched is `Q6016` too.
4. `retype_column.using` is a definition expression of the new type that reads the old column; absent, it is
   `Cast(strict, Col(row, name), type)` ([QT] §7.6).
5. `add_constraint.defer_validation` adds the constraint not validated and starts no job; the owner validates it
   later with `validate_constraint`. Without it, a validation job starts at commit (§8.7).

### 3.4 Example

Text form ([QT] §15). A table of users with an enum status and a nested list of addresses, then two online
changes in one statement.

```
(create_type :type_name app.status
  :type union<active: {}, banned: {reason: text}>)

(create_table :table app.users
  :columns [(column :name id :type int64 :identity always)
            (column :name email :type text)
            (column :name status :type app.status
                    :default (variant :type app.status :tag active :expr (record)))
            (column :name addresses :type list<{city: text, zip: text?}>)]
  :primary_key [id]
  :constraints [(constraint :name users_email_key :unique [email])])

(alter_table :table app.users
  :actions [(add_column :def (column :name created_at :type timestamptz?))
            (add_constraint :def (constraint :name email_has_at
               :check (call contains row.email '@'::text)))])
```

The `alter_table` is metadata-only for `created_at` (§8.1), and adds `email_has_at` not validated, enforced at once,
with a validation job that starts when the statement commits (§8.7).

**Worked encoding.** Each statement is its own envelope, with no parameters and no spans. Every field the text form
leaves out takes its empty value: an absent `opt`, `false`, the ordinal 0 (`none` for `identity`), and
`defer_validation` false. Names are shown as their octets; `61 70 70` is `app`.

The `create_type` is these 50 octets:

```
01 00 00                   major 1, minor 0, no parameters
27 2c                      CreateType (39), 44-octet payload
  03 61 70 70  06 73 74 61 74 75 73        type_name app.status
  11 1e 02                                 Type union, two variants
    06 61 63 74 69 76 65  10 01 00         active: record {} (kind 16, no fields)
    06 62 61 6e 6e 65 64  10 0a 01         banned: record, one field
      06 72 65 61 73 6f 6e  08 00          reason: text
  00                                       if_not_exists false
00                         no spans
```

The `create_table` is these 152 octets; its payload length, 145, is the two-octet `uvar` `91 01`:

```
01 00 00
22 91 01                   CreateTable (34), 145-octet payload
  03 61 70 70  05 75 73 65 72 73           table app.users
  04                                       four columns
    02 69 64  03 00  00 00 01 00           id int64, no default, not generated, identity always, no sequence
    05 65 6d 61 69 6c  08 00  00 00 00 00  email text
    06 73 74 61 74 75 73                   status
      20 0b 03 61 70 70 06 73 74 61 74 75 73             Type: named reference app.status (kind 32)
      01 0b 17                                           default: Variant, 23 octets
        20 0b 03 61 70 70 06 73 74 61 74 75 73           its type, app.status
        06 61 63 74 69 76 65  09 01 00                   tag active, value Record with no fields
      00 00 00                                           not generated, no identity, no sequence
    09 61 64 64 72 65 73 73 65 73          addresses
      13 12 10 10 02                                     Type list of a record of two fields
        04 63 69 74 79  08 00                            city: text
        03 7a 69 70  14 02 08 00                         zip: text? (nullable, kind 20)
      00 00 00 00
  01 02 69 64                              primary_key [id]
  01                                       one constraint
    0f 75 73 65 72 73 5f 65 6d 61 69 6c 5f 6b 65 79      users_email_key
    01  01 05 65 6d 61 69 6c                             unique (1), columns [email]
  00                                       if_not_exists false
00
```

The `alter_table` is these 83 octets:

```
01 00 00
24 4d                      AlterTable (36), 77-octet payload
  03 61 70 70  05 75 73 65 72 73           table app.users
  02                                       two actions
    00                                     add_column (0)
      0a 63 72 65 61 74 65 64 5f 61 74  14 02 0d 00  00 00 00 00   created_at timestamptz?
    06                                     add_constraint (6)
      0c 65 6d 61 69 6c 5f 68 61 73 5f 61 74             email_has_at
      00                                                 check (0)
      05 1c 08 63 6f 6e 74 61 69 6e 73 02                Call "contains", two arguments
        01 0a 03 72 6f 77 05 65 6d 61 69 6c              Col row email
        11 04 08 00 01 40                                Literal text '@'
      00                                                 defer_validation false
00
```

None of the three introduces a binder (`row` is introduced by no node, §2.12), so each one's shape form ([QT]
§10.2) is its encoding, and its shape digest is the SHA-256 of these octets (§3.1 rule 6).

### 3.5 Checks on the tree

A server runs these checks on a decoded DDL statement before it reads the catalog, in the phases of §3.1 rule 4,
each phase in pre-order. They need nothing but the tree, so a client library MAY run them too. Every other rule of
§2 and §3 needs the catalog and is checked when the statement executes.

1. **Names** (`Q2…`, `Q6002`). The binders of a view query and of each definition expression follow [QT] §8, a
   definition expression seeing only `row` (§2.12 rule 1); two `ColumnDef`s, or two `ConstraintDef`s, of one
   `CreateTable` with the same name are `Q6002`.
2. **Types** (`Q6005`). A `CreateType` whose type is not what its `def` allows (`alias`: a record, a union or a
   named reference; `domain`: any type; `range`: an orderable, key-encodable scalar, the subtype; `distinct`: a
   predefined scalar; `structured`: a record), or whose type refers to the name it creates, or a structured
   type `under` a `FINAL` type; an identity column (§2.7 rule 3), or a `SequenceDef`, whose type is not
   `int16`, `int32` or `int64`.
3. **Parameters** (`Q6001`). A DDL envelope that declares any parameter.
4. **Structure**:
   - `Q6019`: every name the statement's own fields hold — each part of a `QualName`, a column, constraint, index,
     role, member, field or variant name, a `grantee` — that is empty or longer than 255 octets. Names inside a
     `Type` or an `Expr` follow [QT].
   - `Q6014`: a `CreateNamespace` or `DropNamespace` of `sys` or `catalog`, or a statement whose created, changed or
     dropped object (`table`, `def.table`, `index`, `type_name`, `sequence`, `view`) is in one. `Grant`, `Revoke`
     and a foreign key's `ref_table` name an object without changing it, and are not checked.
   - `Q6006`: §2.12 rule 2 in every definition expression (`default`, `generated`, a check, an index key or
     `predicate`, `retype_column.using`, `set_default.default`); rule 3 for checks, generated columns, index keys
     and predicates, a call of `statement_timestamp` or `next_value` being the volatile case; a default with a
     `Col` or `Row` of `row` (§2.7 rule 1); §3.3 rule 1's one-of rule and a `sequence` without an identity; an
     `append_fields` fill that is present and not a `Literal`, or absent for a field type that is not nullable.
   - `Q6009`: a `CreateTable` with an empty `primary_key`.
   - `Q5005`: a column listed twice in a primary key, a `unique` or `foreign_key` column list, or `set_primary_key`.
   - `Q5009`: an empty `unique` or `foreign_key` column list, `set_primary_key`, index `keys`, `AlterTable.actions`,
     or `Grant`/`Revoke.privileges`.
   - `Q6013`: a foreign key whose `columns` and `ref_columns` differ in number.
   - `Q6016`: §3.3 rule 3.
   - `Q6017`: a `SequenceDef` whose `increment` is 0 or `cache` is 0; whose given `start`, `min` or `max`, or
     `AlterSequence.restart`, is outside its type's range; a `ColumnDef.sequence` whose type is not the column's.
     For a `CreateSequence` or a column's sequence, the options with their defaults (§3.3 rule 1) filled in must
     have `min` ≤ `max` and `min` ≤ `start` ≤ `max`; for an `AlterSequence`, which keeps the options it does not
     give, only when `min` and `max` are both given, and then `start` and `restart` must lie between them.

## 4. The catalog as relations

### 4.1 Rules

1. The relations below are system views in the reserved namespace `catalog`. They are read with an ordinary `Scan`
   ([QT] §6.1), at the statement's snapshot, like any relation, and every role may read them (§2.10 rule 5). They
   are read-only (`Q5003`). A read of them is neither part of the statement's catalog read (§6.2) nor logged as a
   read of rows ([ISO] §4.4): a DDL committed while the reader runs does not abort it.
2. They are the **stable** interface to the catalog. The stored system trees (`sys.*`, [DISK] §3.9) are an internal
   format, may change with the on-disk format, and MUST NOT be scanned by name (`Q2001`).
3. Their column lists are versioned with the tree schema ([QT] §13): a minor version MAY append columns and MAY add
   relations, a variant to a union column, or a field to a record column at its end; anything else is a major
   change. A generator SHOULD name the columns it reads rather than read whole rows.
4. A type is reported twice: `type` is its canonical [QT] §12.3 encoding (structural, with the nullable wrapper,
   never a named reference), which a client decodes with the decoder it already has; `type_text` is the [QT] §15
   text form of the same structural type, so it names no named type either (`type_name` does, where there is one).
   An expression is reported twice as well, its row binder `row` (§2.12): as its [QT] §15 text form, and in a
   column named after that one with `_tree` appended, as the canonical [QT] §12.4 encoding of the expression node
   alone, which a client decodes into the tree (the `terndb` shell prints it back as SQL).
5. Objects not yet visible to the reader's snapshot are absent; objects in a non-`ready` state are present with
   their state, so a tool can watch a change in progress. A generator SHOULD use only `ready` columns.

### 4.2 The relations

Every `oid` is `uint64`. `QualName` in a column is `{namespace: text, name: text}`.

**`catalog.version`** — one row: `catalog_version: uint64` (§1 rule 3), `tree_schema: {major: uint32, minor:
uint32}`.

**`catalog.namespaces`** — `oid`, `name: text`, `owner: text`.

**`catalog.tables`** — `oid`, `namespace: text`, `name: text`, `owner: text`, `schema_version: uint16`,
`primary_key: list<text>` (column names, in key order), `state: union<ready: {}, rebuilding: {job: uint64}>`.

**`catalog.columns`** — `table_oid`, `column_id: uint32`, `ordinal: uint16`, `name: text`, `type: bytes`,
`type_text: text`, `type_name: QualName?`, `nullable: bool`, `in_primary_key: bool`, `default: text?`,
`generated: text?`, `identity: union<always: {}, by_default: {}>?`, `state: union<ready: {}, filling: {job:
uint64}, converting: {job: uint64, to_type: bytes, to_type_text: text}>`, `default_tree: bytes?`, `generated_tree:
bytes?`, `collation: text` (§2.3 rule 4; `ucs_basic` when none was declared). One row per column of the table's
current
schema version, in every state, including the columns of views (`table_oid` the view's oid, no key, no default,
`ready`). `ordinal` counts from 0 in the current schema version; a view's column has `column_id` its ordinal plus 1.

**`catalog.indexes`** — `oid`, `table_oid`, `namespace: text`, `name: text`, `unique: bool`, `keys:
list<{column: text?, expr: text, descending: bool, expr_tree: bytes, collation: text}>` (`column` set when the
component is a plain column, whose `expr` is then `Col(row, column)`; `collation` `ucs_basic` for a binary or
non-text component), `predicate: text?`, `state: union<building: {job: uint64}, ready: {}, failed: {job:
uint64}>`, `predicate_tree: bytes?`.

**`catalog.constraints`** — `oid`, `table_oid`, `namespace: text`, `name: text`, `validated: bool`, `kind:
union<primary_key: {columns: list<text>}, unique: {columns: list<text>, index_oid: uint64}, not_null: {column:
text}, check: {expr: text, expr_tree: bytes}, foreign_key: {columns: list<text>, ref_table_oid: uint64, ref_columns: list<text>,
on_delete: FkAction, on_update: FkAction, deferred: bool}>`, where `FkAction` is `union<no_action: {}, restrict:
{}, cascade: {}, set_null: {}, set_default: {}>`. A `not_null` row is listed for each column whose type is not
nullable; it is no object (§2.6), so its `oid` is 0, its name `<table>_<column>_not_null`, and it is validated.

**`catalog.types`** — `oid`, `namespace: text`, `name: text`, `owner: text`, `kind: union<record: {}, union: {},
enum: {}, domain: {default: text?, not_null: bool, checks: list<{name: text, expr: text}>}, range:
{multirange_name: text}, distinct: {}, structured: {under: QualName?, instantiable: bool, final: bool, ordering:
union<none: {}, equals_only_by_state: {}>}>` (`enum` for a union whose payloads are all `{}`; the members after
`enum` from tree 1.1), `type: bytes`, `type_text: text`, `members:
list<{name: text, type: bytes, type_text: text, type_name: QualName?}>` (the fields of a record or the variants of
a union, with each variant's payload record as its type, in declared order; `type_name` set where the member was
declared with a named type).

**`catalog.sequences`** — `oid`, `namespace: text`, `name: text`, `type: bytes`, `start: int64`, `increment:
int64`, `min: int64`, `max: int64`, `cycle: bool`, `cache: uint32`, `owned_by: {table_oid: uint64, column_id:
uint32}?`. The current value is not reported, since it is not transactional.

**`catalog.views`** — `oid`, `namespace: text`, `name: text`, `owner: text`, `query: text` (its [QT] §15 form),
`query_tree: bytes` (the canonical encoding of its `Query` node). `query` is that node's text form, `(query :rel …)`.

**`catalog.roles`** — `name: text`, `login: bool`, `superuser: bool`, `member_of: list<text>`.

**`catalog.grants`** — `object_oid`, `object_kind: union<table: {}, view: {}, namespace: {}, sequence: {}, type:
{}>`, `grantee: text`, `privilege: text`, `grantable: bool`.

**`catalog.depends`** — `oid`, `ref_oid` (§2.11).

**`catalog.jobs`** — `job: uint64`, `txid: uint64` (the transaction whose DDL started it), `kind: text` (one of
`build_index`, `validate_constraint`, `fill_column`, `convert_column`, `rebuild_table`), `object_oid`, `table_oid`,
`state: union<queued: {}, running: {}, succeeded: {}, failed: {code: text, message: text}, cancelled: {}>`,
`progress: {done: uint64, total: uint64?}` (rows), `started: timestamptz?`, `finished: timestamptz?`. A finished job's
row is kept for at least 7 days (`job_retention`).

**`catalog.schema_versions`** — `table_oid`, `schema_version: uint16`, `committed: uint64` (the commit sequence
number that created it), `live: bool` (whether any stored row or undo record may still carry it, §5.5), `columns:
list<{column_id: uint32, name: text, type: bytes, type_text: text}>`.

## 5. Schema versions

### 5.1 What a version is

1. A schema version of a table is its list of columns at that version: for each, column id, ordinal, name, type,
   nullability, default, fill, generation expression, identity and state ([DISK] §3.9 `sys.columns`).
2. Every committed statement that changes any of those for a table adds exactly one new version; one that changes
   only indexes, constraints, names of indexes or constraints, owner or privileges adds none.
3. A **fill** is recorded for every column added after the table was created: the value a row of an older version
   reads for it. It is the column's default evaluated once when the column is added, if the default is a constant
   (deterministic and reading no column), else null and the column starts `filling` (§8.2).

### 5.2 What a row carries

Every stored row carries the schema version it was encoded in ([DISK] §4.1 `schema_version`), and every undo
record carries the version of the old image it restores ([DISK] §5.2 `old_schema_version`). Nested values carry
no version of their own: the descriptors of every nested record, union and relation are those of the row's
version. Secondary index entries carry none: an index's key is computed by its own definition, and an index whose
key encoding would change is rebuilt (§8.5).

**Invariant V1.** A version visible to a snapshot was encoded in a schema version visible to that snapshot.
(A version committed at `c ≤ s` was written by a transaction whose catalog was read at or before `c`; and §5.4
rule 3 forbids a writer to install a version older than the one it replaces.) Translation therefore only ever runs
forward.

### 5.3 Reading a row of an older version

A reader whose statement planned against version `v` reads a stored version encoded in `r ≤ v` as follows, for
each column of `v` that the statement reads, matched by **column id**:

1. A column present in `r`: decode it with `r`'s descriptor, then convert it to `v`'s type by the composition of
   the conversions of every retype between `r` and `v`. Each is lossless (rule 3) or was proven to succeed for
   every value by its job before it committed (§8.5), so translation MUST NOT fail; a failure is `D1001`
   (`data_corrupted`).
2. A column absent from `r` (added after `r`): its fill.
3. The **lossless conversions** are: `intN → intM` and `uintN → uintM` for M > N; `uintN → intM` for M > N;
   `intN`, `uintN` → `decimal(p, 0)` when `p` holds every value (3, 5, 10, 19 digits for 8–64 bits signed; 3, 5,
   10, 20 unsigned); `decimal(p, s) → decimal(p', s')` when `s' ≥ s` and `p' − s' ≥ p − s`; `float32 → float64`;
   `int8`, `int16`, `uint8`, `uint16` → `float32`; `int8` … `int32`, `uint8` … `uint32` → `float64`; `json → text`;
   the widenings of [QT] §4.1 rules 8–14 (`varchar(n)` to a longer `varchar` or `text`; `char(n)` to a longer
   `char` or `varchar`, or `text`; the binary and bit widenings; every integer and `decimal` to `numeric`; the
   `decfloat` widenings; a larger datetime precision; an interval qualifier that holds every value, and either
   interval family to `interval`; a larger or no array bound; `cidr → inet`; `macaddr → macaddr8`); a domain to
   its base type; `T → T?`; `list<E> → bag<E>`; and, recursively, a record whose fields convert losslessly, with fields appended
   that are nullable or have a constant fill; a union whose payloads convert losslessly, with variants appended at
   the end; a relation whose element converts losslessly; renames of fields and tags (by position, which the
   encoding keys on). Nothing else is lossless.
4. Columns dropped between `r` and `v` are skipped; their bytes are never decoded.
5. **What each conversion does to a value.** A null converts to null. A non-null scalar converts as the strict
   `Cast` of [QT] §7.6 does, which no lossless conversion makes round or fail: an integer keeps its value as a
   wider integer, a `decimal(p, 0)` or a float; a `decimal` keeps its value at the larger scale (its unscaled
   integer multiplied by 10^(s' − s)); `float32 → float64` keeps the number; `json → text` is the value's canonical
   text ([QT] §3.5), which is how a `json` value is held. A `char(n)` value read as `char(m)`, `varchar` or `text`
   is padded to `n` (rows hold it unpadded, [DISK] §4.4) and, for `char(m)`, then to `m`; a `decimal` read as
   `numeric` keeps its scale as its display scale; `macaddr → macaddr8` inserts FF:FE after the third octet;
   the interval conversions are [QT] §2.12 rule 3's. A record converts field by field, a union its variant's
   payload and a relation each element, members matched by position; a record value lacking fields its type has
   since gained reads each one's fill (§2.13 rule 7), or null. A stored value that is not of its version's type, or
   that does not convert (a `decimal` holding more digits than its precision allows), is `D1001`.

*Rationale: a row stores positions and the catalog keeps every version's column list, so translation is a
per-version column map computed once per (table, `r`, `v`) and cached, not a per-row search.*

### 5.4 Writing

1. A write encodes the row in the writer's schema version: the version its statement planned against. A row
   written by an update is re-encoded whole in that version, whatever version it had; the undo record then holds
   every column of the old version ([DISK] §5.2).
2. A writer writes every column of its version, including `filling` and `converting` ones: a `filling` column
   gets its default (evaluated per row), and a `converting` column its value in its current (old) type.
3. **No writer replaces a newer encoding.** A writer whose version is `v` that would install a version over a
   newest committed version encoded in a version later than `v` MUST abort with `schema_changed` (`T1004`). This
   can happen only at read committed, where a write applies to the newest committed version ([ISO] §4.2 rule 3);
   at snapshot and serializable such a version is outside the writer's snapshot and the write already fails with
   `serialization_failure`. *Rationale: re-encoding a newer row in an older version would drop its newer columns.*

### 5.5 Retiring versions

1. A version is **live** while some stored row or undo record carries it. A version that is not live and is not
   current MAY be deleted from `sys.columns` by purge.
2. Rows of old versions stay as they are; a later update re-encodes them (§5.4 rule 1). `AlterTable rewrite` (§8.11)
   re-encodes every row in the current version, after which old versions retire as their undo is purged.
3. A table whose next schema version would exceed 65,535 refuses the statement with `Q6012`; `rewrite` rebuilds
   the table under a new oid with versions starting again at 1 (§8.11).

## 6. Locks, catalog reads and running transactions

### 6.1 Locks

1. A DDL statement takes the row write lock ([DISK] §4.1, [ISO] §4.2) of every `sys.objects` row it changes. Every
   DDL that changes a table's definition — its columns, indexes, constraints, name or owner — changes the table's
   `sys.objects` row, so DDL statements on one table are ordered by that lock, with the waiting, timeout and
   deadlock rules of [ISO] §4.2. A foreign key locks both tables' rows. A statement that creates or renames an
   object also locks the name it takes (and a rename the name it gives up) in its namespace — the unique key of
   `sys.objects` on (namespace, name) — so two transactions creating one name are ordered the same way.
2. DML takes **no** catalog lock, and DDL waits for no reader and no writer of the table. Readers of an object that
   is dropped keep reading it at their snapshot: its storage is freed only by purge, after the horizon passes the
   drop ([DISK] §5.6).
3. **The commit latch.** Each table has a short latch. A transaction that wrote the table holds it shared from the
   start of its reconciliation (§6.4) to the moment its commit record is in the WAL buffer; a DDL transaction on
   the table holds it exclusive while it writes its commit record. So no DDL on a table commits between a writer's
   reconciliation and that writer's commit. The latch is never held while waiting for a lock or for I/O other than
   the WAL buffer.
4. At snapshot and serializable, a DDL whose catalog row was changed by a commit after its snapshot fails with
   `serialization_failure` ([ISO] §4.2 rule 2). At read committed, the DDL statement is validated again against
   the newest committed catalog, and its error, if any, is reported.

### 6.2 The catalog read of a statement

Every statement logs a **catalog read** of each object it uses, as [ISO] §5.6 requires, and the read is
column-precise ([ISO] §4.4, decision Q15). The catalog read of a statement is the set of:

1. the identity (oid) and kind of every object it names: table, view, named type, sequence, index or constraint
   named as a conflict target;
2. for every column it resolves, by `Col` or by a name in `Insert.columns`, `Update.set` or a conflict target: the
   column's id, name, type and nullability;
3. the **whole column list** (ids, names, order, types) of a table or view when all of a `Scan`'s columns reach the
   statement's result or a value it builds: a `Scan` whose binder reaches the root output, a `Relation` or an
   `Insert` source without a `Project` naming its columns, or `Row(b)` of a `Scan` binder `b`;
4. for an `Insert`: the defaults, identities and generation expressions of the columns it does not name, and the
   generation expressions of every generated column for an `Update`.

The constraints and indexes a write enforced or maintained are not part of the read; a write is brought up to date
with those added later by reconciliation (§6.4).

### 6.3 Which changes abort a running transaction

1. A transaction **conflicts** with a committed DDL `D` that it did not see if `D` changed any element of the
   catalog read of any of its statements. Then it MUST abort with `schema_changed` (`T1004`), at commit or, at read
   committed, at its next statement on the table if that statement would plan against the changed definition
   ([ISO] §5.6). A transaction that does not conflict commits. A transaction with an empty write set (no row and
   no catalog entry written) is serialized at its snapshot, before `D` ([ISO] §5.5), and is not aborted at commit:
   a reader of a dropped table finishes and commits.
2. Hence, for a transaction `T` that used table `t` under the definition before `D`:

| `D` | conflicts with `T` when |
|---|---|
| add a column, add an index, add a constraint, drop an index, drop a constraint, validate a constraint, set a default `T` did not read | never (an added column conflicts only through §6.2 rule 3) |
| drop, rename or retype a column; change its nullability | `T` resolved that column, or read the whole column list |
| set or drop a default, generation expression or identity | `T` inserted without naming that column |
| rename or drop the table; change its primary key; rewrite it (a new oid) | `T` used `t` at all |
| change a named type | `T` resolved a column declared with it, or the type by name |

3. *Rationale (serializability).* If `D` changed nothing `T` read, then `T`'s reads and its result types are the
   same under the definition after `D` as before it, and its writes are made equal to what `T` would have written
   after `D` by translation (§5.3) and reconciliation (§6.4). So `T` may be serialized after `D`, in commit order,
   as [ISO] §3 requires. If `D` changed something `T` read, there is an rw edge from `T` to `D` with `T` committing
   later, which validation refuses at every level.

### 6.4 Reconciliation

A transaction `T` that wrote table `t` and commits after a DDL `D` on `t` that its catalog read did not include
MUST, after taking the commit latch (§6.1 rule 3) and before writing its commit record, bring every row version it
wrote in `t` up to the newest committed definition of `t`:

1. **Indexes.** For each index added or rebuilt by `D` (in state `building` or `ready`), insert the entries of each
   row `T` wrote and delete-mark those of each row it removed or changed ([DISK] §4.9 rule 2). A unique index is
   checked as an insert by an aware writer would be, and fails with `unique_violation` or, for a conflict with a
   version committed after `T`'s snapshot, `serialization_failure` (`unique_violation` at read committed, which
   never raises `serialization_failure`). A conflict with a row another live transaction is writing is
   `lock_timeout`, since a commit does not wait (as for a deferred foreign-key check, §2.6 rule 3).
2. **Constraints.** For each check constraint and each not-null change added by `D` (including the guard of a
   retype, §8.5), check every row `T` inserted or updated; for each foreign key in which `t` is the referencing
   table, check and lock the referenced rows as §2.6 rule 3 requires. A failure aborts `T` with the constraint's
   code.
3. **Referenced side.** If `D` added a foreign key that references `t`, and `T` deleted a row of `t` or changed its
   referenced columns, `T` MUST abort with `schema_changed`. *Rationale: running a new key's cascade at commit is a
   statement `T` never planned.*
4. **Columns.** For each row `T` inserted at a version older than the current one, if a column absent from `T`'s
   version has a current default other than its fill, or is generated, `T` rewrites the row in the current
   version, with that column computed (its default, or its generation expression). A `filling` column is computed
   the same way.
5. **Shadows.** If `D` started a table rebuild (§8.11), apply each of `T`'s writes of `t` to the shadow table as a
   dual writer would.

Reconciliation is work proportional to `T`'s writes of `t`, and is needed only when a DDL on `t` committed during
`T`'s lifetime. It is logged as ordinary writes of `T` (secondary inserts with `SECONDARY` undo records, row
updates with `UPDATE` undo records), so a crash before `T`'s commit record undoes it with `T`.

## 7. Background jobs

1. An operation that must visit existing rows runs as a **job**, recorded as a row of `sys.jobs` ([DISK] §3.9) and
   shown in `catalog.jobs`. The DDL statement creates the job row in state `queued` in its own transaction; the job
   starts after that transaction commits, and does not exist if it rolls back. `AwaitJobs` waits for the jobs of a
   transaction (§3.2).
2. **Snapshot.** A job's scan reads at a snapshot taken after the commit of the DDL that started it. Every row
   version committed after that snapshot was written by a transaction that saw the DDL, and so obeyed it (§5.4,
   §2.6), or by one that did not, and so reconciled with it (§6.4). Therefore the job need not wait for any
   transaction, at any isolation level, and no row escapes it (property L3, §10).
3. **Batches.** A job works in key order, in batches. Each batch is an internal transaction at read committed that
   does the batch's writes and updates the job row's `resume_key` and `progress`, and commits. Index entries a job
   inserts are logged as redo-only `NODE_INSERT`/`NODE_BULK` mini-transactions with no undo, since inserting an
   entry for a row that exists is idempotent and harmless if the batch is lost; row writes are ordinary writes with
   undo.
4. **Crash safety.** A job's state is its `sys.jobs` row, which is an ordinary versioned row: after a crash,
   recovery ([DISK] §6.9) restores it as of the last committed batch, and the job resumes from `resume_key`,
   repeating at most one batch. No WAL record kind is specific to DDL.
5. **Failure.** A job fails on a constraint or uniqueness violation found in existing data, or on a conversion that
   fails (§8.5). A failed job records the code in its row and undoes its operation as §8 says for each kind, in one
   internal transaction; a conflict or timeout in a batch is retried, and is not a failure.
6. **Cancellation.** `CancelJob`, or a `Drop` of the job's object, stops the job after its current batch and undoes
   its operation as for a failure, with state `cancelled`. A DDL on the job's table that conflicts with the job's
   operation (dropping the column being filled) cancels it in the same transaction.
7. **Priority.** Jobs run at a priority below user work, and a server SHOULD bound their I/O (`job_io_limit`).
   Several jobs may run at once, on different tables or on one. An engine MAY instead run each job to its end, in
   one batch, after the commit that started it is published and before that commit returns; its final transaction
   then commits under a number of its own, and no statement of another transaction runs while it does. The
   interleavings of §8 then reduce to writers that did not see the DDL, which reconcile at their commit (§6.4).
8. On a replica, jobs do not run: their effects arrive by WAL replay, like all writes.

## 8. Online operations

Each operation below gives: its **kind** (metadata-only, or a job); the **locks** it takes beyond §6.1 rule 1 (none
of them waits for DML); how it meets **running transactions** at each level; its **crash safety**; and how an
**old row** reads afterwards. Unless an entry says otherwise, the interaction is the same at serializable, snapshot
and read committed, and is given by §6.3 (which transactions abort) and §6.4 (what the others do at commit).
Every catalog change is logged as ordinary row changes of the system trees (`NODE_INSERT`, `NODE_PATCH`,
`ROW_HEADER` with `UNDO_APPEND`, [DISK] §6.4) and is durable with its transaction's `TXN_COMMIT`; a crash before
that record rolls it back like any write.

### 8.1 Add a column with a constant default or none

- **Kind:** metadata-only. One new schema version with the column `ready` and its fill (§5.1 rule 3). A non-null
  column needs a constant default (else `Q3005`).
- **Row limit:** the added column counts toward the smallest-row limit of §2.13 rule 10; an `add_column` that
  leaves the table past it is `Q6022` and changes nothing.
  A `retype_column` is held to the same limit with the column at its new type (§8.5, §2.13 rule 10).
- **Running transactions:** none conflicts unless it read the whole column list (§6.2 rule 3). A transaction that
  inserts at the old version and commits later produces rows that read the fill, which is what an insert after the
  change would have written; if the default has since changed, reconciliation rewrites them (§6.4 rule 4).
- **Crash:** the catalog rows only.
- **Old rows:** read the fill. Nothing is written to the table.

### 8.2 Add a column whose default is volatile, or a generated or identity column

- **Kind:** a `fill_column` job. The statement adds the column in state `filling` with a null fill. Writers of the
  new version write its value (§5.4 rule 2). The job updates every row of an older version, in key order, into the
  current version with the column computed: its default evaluated per row, its generation expression, or the next
  identity value. When it reaches the end, it sets the column `ready` in its last batch.
- **Running transactions:** as §8.1; an old-version writer computes the column for its inserted rows at
  reconciliation (§6.4 rule 4). The job's batches are ordinary writers: they wait for a row's lock, and they skip a
  row whose newest committed version is already in a version that has the column. The column is invisible to all
  statements, including the adding transaction's own, until it is `ready`.
- **Crash:** the job row and its committed batches; it resumes from `resume_key`.
- **Failure or cancellation:** a new schema version drops the column. Rows already filled keep its bytes, which are
  never read (§5.3 rule 4).
- **Old rows:** after the job, none remain without the column, other than in undo, which old snapshots read in their
  own version.

### 8.3 Drop a column

- **Kind:** metadata-only. A new schema version without the column. A primary-key column cannot be dropped
  (`Q6007`); nor can a column an index, constraint, generated column or view uses, unless `cascade` drops those.
- **A `converting` column** may be dropped (§2.13 rule 11). The drop abandons the conversion: the column's entry,
  its target type, its `using`, its guard and its replacement indexes (§8.5 rule 2) leave with it in the same
  transaction, and the `convert_column` job is cancelled in that transaction (§7 rule 6), so it finds nothing left
  to do: no converted value survives and its final transaction never runs. The `cascade` rule above applies to the
  column's own indexes and constraints as to any column's. Re-adding a column under the dropped name adds a new
  column with a new column id (§2.3 rule 1), which is `ready` or `filling` by §8.1 and §8.2 and never `converting`.
- **Running transactions:** a transaction that resolved the column, or read the whole column list, conflicts. One
  that never named it commits; its writes at the old version still carry the column's bytes, which no later
  version reads.
- **Crash:** the catalog rows only.
- **Old rows:** keep the bytes until they are next updated or the table is rewritten (§8.11); they are skipped on
  read. Out-of-line values of a dropped column are freed when the row's old version is purged ([DISK] §5.6) after
  such an update.

### 8.4 Rename a column, table, index, constraint or named type

- **Kind:** metadata-only. Renaming a column adds a schema version (the name is part of it); the column id is
  unchanged, so no row is touched. Every stored definition that names the column (checks, index expressions,
  generated columns, defaults, views) is rewritten to the new name in the same transaction. Renaming a table also
  rewrites the views that scan it.
- **A `converting` column** may be renamed (§2.13 rule 11). The column keeps its id, its state and its target, the
  `using` expression and the guard (§8.5 rule 2) are rewritten to the new name with every other definition, and the
  `convert_column` job, which finds its column by id, continues; its final transaction makes the column `ready` in
  its target type under the new name. A `retype_column` of it under the new name is `Q6023`.
- **Running transactions:** a transaction that resolved the old name conflicts; others do not notice.
- **Crash:** the catalog rows only.
- **Old rows:** unchanged; they are matched by column id.

### 8.5 Retype a column

**Row limit:** a retype leaves the table within the smallest-row limit of §2.13 rule 10 both with the column at
its old type and at its new one (`Q6022`, nothing recorded), whether the retype is metadata-only or `converting`.

1. **Lossless** (§5.3 rule 3, with no `using`), and either not used by any index key, index predicate or the
   primary key, or one that leaves the key encoding unchanged ([DISK] §4.6): appending variants to an enum, or
   renaming a tag:
   - **Kind:** metadata-only. A new schema version with the new type.
   - **Running transactions:** a transaction that resolved the column, or read the whole column list, conflicts.
   - **Crash:** the catalog rows only.
   - **Old rows:** converted on read (§5.3 rule 1).
2. **Otherwise** (a conversion that can fail, a `using` expression, or a column in an index):
   - **Kind:** a `convert_column` job, in two committed steps. **Step 1** (the statement): the column becomes
     `converting`, keeping its old type; a **guard** — the constraint that `using` succeeds on the column's value —
     is enforced on every write from now on, as a not-validated check; and for each index that uses the column, a
     replacement index on the converted value is created `building`. **Step 2** (the job): verify the guard on
     every existing row and build the replacement indexes, as §8.7 and §8.9 do; then, in its final transaction,
     add a schema version with the new type and the column `ready`, drop the guard, and swap each replacement
     index in for the one it replaces (the old index is dropped and freed by purge).
   - **Running transactions:** step 1 conflicts with no transaction (the column's type is unchanged); an
     old-version writer that commits later checks the guard on its rows and maintains the replacement indexes
     (§6.4 rules 1–2). Step 2 conflicts with transactions that resolved the column, as rule 1. A query never sees a
     half-converted column: before step 2 every reader sees the old type, after it every reader sees the new type.
   - **Crash:** step 1 and every committed batch are durable; the job resumes. Step 2 is one transaction.
   - **Failure:** a value the guard refuses fails the job with the conversion's code (`R0006`, or `R0001`), and
     its details name the row's key. The guard and the replacement indexes are dropped; the column is `ready` in
     its old type again.
   - **Old rows:** after step 2, converted on read by `using`, which the job has proved succeeds on every row
     version a snapshot after step 2 can see. Rows are not rewritten; `rewrite` does that when wanted.
3. Any other retype of a primary-key column is a table rebuild (§8.11). A conversion that [QT] §7.6 does not permit and no
   `using` provides is `Q6008`.
4. **A column that is still `converting` is not retyped again.** A `retype_column` naming a column in state
   `converting` is refused with `Q6023` ("the column is being retyped; wait for the conversion to finish"), whatever
   the new type or `using`, and the statement has no effect: the catalog is unchanged and the other actions of the
   same `AlterTable` are not applied. This is checked before the conversion of rule 2 and the limits of §2.13 rule
   10, so it takes precedence over `Q6008` and `Q6022`. The column can be retyped again once the job's final
   transaction has made it `ready`, including after a failed job, which returns it to `ready` in its old type. A
   metadata-only retype (rule 1) completes within its statement, so a later retype of the column is never refused
   for it, and a retype of another column of the same table is unaffected by one that converts.
5. **A column that is still `filling` is not retyped.** A `retype_column` naming a column in state `filling` (§8.2)
   is refused with `Q6024` ("the column is being filled; wait for the fill to finish"), whatever the new type or
   `using`, with the effects of rule 4: checked at the same point, before rule 2's conversion and §2.13 rule 10's
   limits, nothing recorded, the statement's other actions unapplied. It is a code of its own, not `Q6023`, because
   `Q6023` says the column is being *retyped* and a fill is not a retype, and a client's remedy differs (wait for
   the `fill_column` job). The column can be retyped once the job has made it `ready`. A column added by the same
   `AlterTable` is not retyped in it either, but that is `Q6016` (§3.3 rule 3), not this code.
6. **The types of [QT] §2.7.** Which retypes are metadata-only (rule 1) follows from §5.3 rule 3 and the key
   encodings of [DISK] §4.6:
   - **Metadata-only even when indexed** (the key encoding is unchanged): `varchar(n)` to a longer `varchar` or
     `text`; `varbinary(n)` to a longer one or `bytes`; `varbit(n)` to a longer one; a larger precision of
     `time`, `timestamp`, `timestamptz` or `timetz`; an interval qualifier widening within a family; a larger or
     no array bound; `cidr → inet`; a domain to its base; and a type to a distinct type over it, or back (the
     encodings are one).
   - **Metadata-only only when not indexed** (lossless, but the key changes): `char(n)` to a longer `char`, to
     `varchar` or to `text` (the key pads to the length); an integer or `decimal(p ≤ 38)` to `numeric` or a
     `decimal(p > 38)`; the `decfloat` widenings; `macaddr → macaddr8`; an interval family to `interval`.
   - **Rule 2, a `convert_column` job**: every narrowing — a shorter `varchar(n)`, `char(n)`, `varbinary(n)`,
     `binary(n)` or `varbit(n)`, or a smaller array bound, whose guard is the store assignment of [QT] §4.1 rule
     15 and fails with `R0009`, `R0010` or `R0012`; a smaller datetime precision (the guard always passes, the
     values round); a type to a domain over it, or to another domain (the guard is the target domain's
     constraints, `T3002`/`T3004`); and every retype with a `using`.
   - `timestamp ↔ timestamptz` has no conversion ([QT] §7.6): `Q6008` unless `using` names the zone
     (`from_zone(c, 'UTC')`).

### 8.6 Set or drop a default; drop or set not null

- **Set or drop a default:** metadata-only, a new schema version. The fill of existing columns does not change.
  Setting the default of a `converting` column (§2.13 rule 11) checks it twice, against the column's current type
  and against its target type (§8.5 rule 2); it is accepted only if both accept it, and otherwise `Q3001`, the code
  of a default that does not fit its column. A default that is null is valid at both when both types are nullable.
  Dropping the default is always allowed, and the default stays when the conversion finishes or fails.
  Transactions that inserted without naming the column conflict (§6.3).
- **Drop not null** (`set_nullable` true): metadata-only, a new schema version, lossless (`T → T?`). Transactions
  that resolved the column conflict, since its type changed.
- **Set not null** (`set_nullable` false): runs as an added check `c IS NOT NULL` (§8.7) named `<column>_not_null`;
  when its validation succeeds, its final transaction adds a schema version with the column's type non-null and
  drops the check. The same transactions conflict at that point as for drop not null. On failure the column stays
  nullable and the check is dropped.

### 8.7 Add a check, unique or foreign-key constraint

- **Kind:** a `validate_constraint` job, unless `defer_validation` is set. The statement adds the constraint **not validated**:
  from its commit it is enforced on every write by aware transactions (§2.6 rule 1) and by reconciliation for the
  others (§6.4 rule 2). A unique constraint creates its index `building` (§8.9); its job is the index build. The
  job scans every row at its snapshot (§7 rule 2) and checks it; for a foreign key, it looks up each referenced
  row at the same snapshot. When it finishes, it marks the constraint validated.
- **Locks:** a foreign key locks both tables' catalog rows (§6.1); the job takes no row locks, since a referenced
  row deleted after its snapshot is deleted by a transaction that checked the new foreign key.
- **Running transactions:** none conflicts (§6.3); every one that wrote the table and commits after the statement
  reconciles with the new constraint, and fails with its code if one of its rows violates it. A transaction that
  deleted a row the new foreign key references aborts with `schema_changed` (§6.4 rule 3).
- **Crash:** the not-validated constraint and the job row are durable; the job resumes.
- **Failure:** a violating row fails the job with the constraint's `T3…` code and the row's key in its details.
  The constraint **stays**, not validated and still enforced on writes; the owner fixes the data and runs
  `validate_constraint`, or drops it. A unique constraint's failed index is dropped with the constraint, since an
  incomplete unique index enforces nothing.
- **Old rows:** unchanged.

`validate_constraint` starts the same job for a not-validated constraint, and does nothing for a validated one.

### 8.8 Drop a constraint

Metadata-only. A unique or primary-key constraint's index is dropped with it, but a primary key cannot be dropped,
only changed (§8.11). No transaction conflicts: a transaction that still enforces the dropped constraint does more
checking than it must, which cannot produce a history the new definition forbids. A running job validating the
constraint is cancelled.

### 8.9 Create an index

- **Kind:** always a `build_index` job; there is no blocking form. *Rationale: Postgres needs `CONCURRENTLY`
  because its default blocks writers; with reconciliation the online build costs no more than a blocking one.*
  The statement creates the index `building`, with an empty tree. From its commit, aware writers maintain it like
  a ready index, and unaware writers maintain it at reconciliation (§6.4 rule 1). The job scans the table at its
  snapshot (§7 rule 2) and inserts each visible row's entry if absent. When it finishes, it sets the index `ready`
  in its final transaction.
- **Unique indexes:** while `building`, aware writers check uniqueness against the entries present and the job
  checks each entry it inserts against them, waiting for the writer of an entry whose row is uncommitted, so two committed rows with equal keys are found by whichever of the
  two arrives second, which fails: a writer with `unique_violation`, or the job, which then fails with
  `unique_violation` (`T3001`) recorded in its row. A row the index's predicate or an expression component cannot be
  evaluated over fails the job with that evaluation's error, and a write of such a row fails with it. Each candidate
  conflict is confirmed against the clustered row's version visible at the newest snapshot ([DISK] §4.9 rule 2), so
  a stale or delete-marked entry is never a conflict.
- **Running transactions:** none conflicts. The planner uses only `ready` indexes, so no query reads a partial
  index.
- **Crash:** the `building` index, its entries so far and the job row are durable (entries by their redo-only
  records, §7 rule 3); the job resumes and re-inserts idempotently.
- **Failure or cancellation:** the index is set `failed` and dropped in one transaction; purge frees its tree
  ([DISK] §5.6).
- **Old rows:** indexed by their translated values (§5.3), so an index over a column added later indexes the fill
  for rows that predate it.

### 8.10 Drop an index

Metadata-only. The `sys.objects` row is deleted; readers whose snapshot sees the index may still use it, and purge
frees the tree after the horizon passes the drop ([DISK] §5.6). No transaction conflicts: a writer that maintains a
dropped index writes entries that no later reader looks at. An index that backs a constraint is dropped only with
the constraint (`Q6007`).

### 8.11 Change the primary key; retype a key column; rewrite

All three **rebuild** the table into a new clustered tree under a new oid.

- **Kind:** a `rebuild_table` job in two committed steps. **Step 1** (the statement): create the **shadow** table —
  a new oid, state `building`, the new primary key (for `rewrite`, the same), the table's columns with the same
  column ids in the current version, and a shadow copy of every secondary index, keyed by the new primary key. The
  table's `catalog.tables.state` becomes `rebuilding`. From step 1's commit every writer of the table also applies
  each write to the shadow (**dual writing**): an insert inserts there too; an update or delete of a key absent
  from the shadow first inserts the version it replaces there, then applies the change, so a rollback restores
  that version. A delete leaves a ghost ([DISK] §4.1 `DELETED`) in the shadow. Writers unaware of step 1 dual-write
  at reconciliation (§6.4 rule 5). The job then copies every row, in the old key order: for each key, it takes the
  old row's lock as `LOCK_ONLY`, reads the newest committed version, and inserts it into the shadow (and its
  indexes) only if the shadow has no entry for that key, live or ghost; it waits for a locked shadow entry's writer
  to finish before deciding. A duplicate of the new primary key fails the job. **Step 2** (the job's final
  transaction): the shadow takes the table's name, grants, constraints, dependents and foreign keys referencing it
  (repointed to the new oid), its state becomes `ready`, and the old table's `sys.objects` row is deleted.
- **Locks:** the table's catalog row in both steps. Step 2 also locks the catalog rows of tables whose foreign
  keys it repoints.
- **Running transactions:** step 1 conflicts with none. Step 2 replaces the table's oid, so every transaction that
  used the table under the old definition and has not committed conflicts (§6.3); readers with older snapshots
  keep reading the old tree. Transactions planned after step 2 see only the new table.
- **Crash:** step 1, the shadow's pages and the job's batches are durable through ordinary records; the job
  resumes. Step 2 is one transaction.
- **Failure or cancellation:** the shadow's `sys.objects` row is deleted, and purge frees its trees.
- **Old rows:** every row of the new table is encoded in its first schema version; old versions of the old table
  remain only in the old tree, for old snapshots, until purge frees it.

### 8.12 Create and drop tables, namespaces, sequences, types and views

- Creating any object is metadata-only, plus an empty tree for a table. A new object is invisible to transactions
  whose snapshot predates its commit, which therefore cannot conflict with it.
- Dropping any object is metadata-only: the `sys.objects` row is deleted and its storage is freed by purge after
  the horizon ([DISK] §5.6), so a reader with an older snapshot finishes its read. A transaction that used the
  object conflicts (§6.3). A transaction that wrote a dropped table aborts with `schema_changed` at commit.
- Changing a named type: §2.8 rule 5, applied per affected table as §8.5 rule 1.

## 9. Transactional DDL

1. DDL statements MAY appear in an explicit transaction, any number of them and mixed with DML, at every isolation
   level. They commit or roll back with it, including to a savepoint. A DDL statement in an implicit transaction
   commits by itself ([WIRE] §4.5).
2. A transaction sees its own catalog changes in its later statements, at every level: a table it created can be
   written, a column it added can be named. A job it started does not run until it commits (§7 rule 1), so an index
   it created stays `building`, and a column it added as `filling` stays invisible, for the rest of the transaction.
   Likewise an `AlterSequence` of a committed sequence moves no draw until it commits (§2.7 rule 8).
3. Catalog reads follow [ISO] §5.6: at snapshot and serializable, at the transaction's snapshot; at read
   committed, at each statement's snapshot. A transaction's DDL is itself a write of catalog rows and obeys
   first-updater-wins ([ISO] §4.2), so two transactions that change one object cannot both commit at snapshot or
   serializable.
4. A serializable transaction that reads a table's data and then changes its definition is serialized at its
   commit like any other; its earlier reads are validated against commits during its lifetime ([ISO] §4.3).
5. DDL in a read-only transaction, or on a replica, is `read_only_transaction` (`T4001`).
6. A DDL statement that fails rolls back to the start of the statement, and aborts the transaction under [ISO] §5.1.
7. After a DDL commits, a prepared statement whose result type it changed is released on its next execution
   (`W2007`, [WIRE] §4.4); one whose shape is still valid is re-validated against the new catalog and keeps running.
   The catalog version a statement is validated and cached against is the one its transaction sees: a
   transaction's own uncommitted DDL gives it a version no other transaction sees ([WIRE] §4.4 rule 6).
8. **The catalog a transaction commits.** Its DDL statements changed only rows it holds locked (§6.1), but other
   DDL may have committed on other objects since. At commit, its DDL is the newest committed catalog with its own
   statements applied, in order, each creating its objects under the oids it created them under when it ran (so
   rows written under those oids stay theirs). A statement that no longer applies to that catalog — an object it
   depended on and did not lock was dropped or changed by a commit it did not see — aborts the transaction with
   `schema_changed` (`T1004`). At read committed, the view each later statement plans against is built the same
   way over the statement's snapshot.

## 10. Checkable properties

The simulation of [ISO] §7.3 runs workload W9 (DML traffic at every level while other sessions run every operation
of §8, with crashes injected at every WAL record) and checks:

| Id | Property |
|---|---|
| L1 | Every row version visible to every snapshot decodes, and translates to the snapshot's current version without error (§5.3; V1). |
| L2 | After any crash and recovery, the catalog relations equal those of the committed prefix, every job is in its last committed state, and resuming it gives the same final catalog and data as an uncrashed run. |
| L3 | When a `validate_constraint` or `build_index` job succeeds, every committed row satisfies the constraint or has its entry (D22), whatever transactions overlapped it. |
| L4 | No operation of §8 makes a DML statement wait on a catalog lock; the only waits are row-lock waits on rows the job writes. |
| L5 | A transaction that did not conflict with a DDL (§6.3) is never aborted with `schema_changed`; one that did is never committed. |
| L6 | A row inserted by a transaction straddling an added column reads, after commit, exactly what a serial execution in commit order would give. |
| L7 | The final state of every W9 history equals that of some serial execution of its committed transactions and DDL in commit order (with [ISO] I1–I26). |
| L8 | `catalog.columns.type` decodes with the [QT] §12.3 decoder to the type a `Scan` of the table reports in its result type. |
| L9 | A failed or cancelled job leaves the definition it started from (§8.2, §8.5, §8.7, §8.9, §8.11), and purge eventually frees every page it allocated (D21). |

## 11. Open questions

1. **Width-independent key encoding.** Widening an integer column that is part of a key rebuilds its index or the
   table (§8.5, §8.11), because [DISK] §4.6 encodes each width differently. An order-preserving encoding shared by
   every integer width (a length-prefixed magnitude) would make that widening metadata-only for keys as well, at a
   byte or two per key. Should keys use it?
2. **Shared row locks for foreign keys.** At every level, a foreign-key check locks the referenced
   row exclusively as `LOCK_ONLY` (§2.6 rule 3), so concurrent inserts of children of one parent serialize on it.
   Should v1 add a shared lock mode to the row header, or accept the contention?
3. **Multi-valued indexes.** Indexing the elements of a nested relation or a scalar list (the GIN-shaped index of
   §2.5 rule 6) is likely the first index users of nested values ask for. Is it a v1.x item, and should it share the
   job machinery of §8.9?
4. **Catalog visibility.** Every role can read every `catalog` relation (§2.10 rule 5). Should a role see only the
   objects it has some privilege on, as Postgres's `information_schema` does?
5. **Virtual generated columns.** Only stored generated columns exist (§2.7 rule 2). Are computed-on-read columns
   wanted, given that a view does the same?
6. **Moving objects between namespaces** and **renaming a namespace** are not defined. Are they needed in v1?
7. **Strictness of the catalog read.** §6.3 aborts a transaction that inserted without naming a column whose
   default then changed, and one that resolved a column whose nullability was dropped, although neither result
   could differ. Is the simple rule worth those rare retries, or should the conflict table be refined?
