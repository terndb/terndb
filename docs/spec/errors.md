# TernDB error codes

**Status:** normative, draft. This document is the one registry of every error code TernDB reports. The specs
cited below define *when* a code is raised; this file defines the code, its name and its flags. A spec that adds a
code adds its row here in the same commit.

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as in RFC 2119.

*Rationale for a separate file: the codes come from five areas owned by four specs, and the wire carries all of
them unchanged. Putting the registry in `wire-protocol.md` would make the wire the owner of query, transaction and
storage codes it does not define; one neutral file is also what client libraries generate their error constants
from.*

## 1. Form

1. A **code** is one letter naming the area, then four decimal digits: `Q` query tree (validation, `query-tree.md`;
   the `Q6…` group is DDL validation, `ddl.md`; the `Q8…` group is the SQL front end, `sql.md`, raised in the
   client before a tree exists),
   `R` runtime (evaluation, `query-tree.md`), `T` transactions and isolation (`isolation.md`), `W` wire
   (`wire-protocol.md`), `D` storage (`on-disk-format.md`; the `D4…` group is opening a database, `c-api.md` §4),
   `C` the C ABI (`c-api.md`), `S` replication, backup and failover (`replication.md`). The first digit groups
   related codes within an area.
2. Every code has a **name**, unique across the registry, in `snake_case`. The name is an alias: specs, client
   libraries and server messages MAY use either, and they mean the same thing.
3. On the wire and across the C ABI, `ErrorEntry.code` carries the **code**, never the name (`wire-protocol.md`
   §8.1). Clients and tests match on the code.
4. Codes and names are never reused. A withdrawn code stays reserved, with its row kept and marked *withdrawn*.
5. **Retryable** (column R): the request, and for a `T` code the transaction, had no effect and MAY be re-run from
   its beginning (`isolation.md` §5.3). **Fatal** (column F): the connection is closed after the error
   (`wire-protocol.md` §8.1 rule 8). A code with neither mark is neither.
6. **SQLSTATE.** A code whose meaning has a standard SQLSTATE (ISO/IEC 9075-2's table of SQLSTATE values, or
   PostgreSQL's where ISO has none) names it at the end of its meaning; the PostgreSQL compatibility layer reports
   that SQLSTATE. A code that names none maps to its class's general SQLSTATE there.

## 2. Query tree: validation (`Q`)

Raised by validation, in the phases of `query-tree.md` §11.2. None is retryable; none is fatal except `Q1005`
during the handshake.

| Code | Name | Meaning |
|---|---|---|
| `Q1001` | `malformed_encoding` | Truncated input, bad varint, payload length mismatch, trailing bytes. |
| `Q1002` | `unknown_kind` | Unknown or reserved node, type or enum ordinal for the negotiated version. |
| `Q1003` | `non_canonical_encoding` | Non-canonical encoding (`query-tree.md` §12.1). |
| `Q1004` | `invalid_utf8` | Invalid UTF-8 in a name or text value. |
| `Q1005` | `unsupported_tree_version` | Envelope version not supported or above the negotiated one; no common tree major. |
| `Q1006` | `nesting_too_deep` | Nesting deeper than the server's limit (at least 256). |
| `Q1007` | `invalid_json` | A `json` literal or parameter value is not a JSON text in canonical form (`query-tree.md` §3.5). |
| `Q2001` | `unknown_table` | Unknown table or view. |
| `Q2002` | `unknown_binder` | Unknown binder. |
| `Q2003` | `unknown_column` | Unknown column of a binder. |
| `Q2004` | `duplicate_binder` | Duplicate binder or duplicate CTE name. |
| `Q2005` | `duplicate_output_name` | Duplicate output name where unique names are required. |
| `Q2006` | `binder_not_visible` | Binder exists but is not visible here. |
| `Q2007` | `unknown_function` | Unknown function. |
| `Q2008` | `unknown_cte` | Unknown CTE. |
| `Q2010` | `unknown_tag` | Unknown union tag. |
| `Q2011` | `unknown_field` | Unknown record field. |
| `Q3001` | `type_mismatch` | Type mismatch (`expected`, `actual`). |
| `Q3002` | `no_matching_overload` | No overload matches. |
| `Q3003` | `ambiguous_overload` | Ambiguous overload. |
| `Q3004` | `condition_not_boolean` | Condition is not boolean. |
| `Q3005` | `unexpected_nullable` | Nullable value where a non-null one is required. |
| `Q3006` | `relation_equality` | `eq`/`ne` on a relation type; use `same`. |
| `Q3007` | `unorderable_type` | Ordering comparison on an unorderable type. |
| `Q3008` | `order_required` | Order required but the relation is a bag (`Limit`, `ties`, `ordinality`). |
| `Q3009` | `match_not_exhaustive` | Non-exhaustive or duplicated `Match` arm. |
| `Q3010` | `setop_incompatible` | `SetOp` inputs incompatible. |
| `Q3011` | `decimal_out_of_range` | Decimal precision or scale out of range, or a decimal literal with more than `p` digits (`query-tree.md` §7.3). |
| `Q3012` | `cast_not_permitted` | Cast not permitted. |
| `Q3013` | `misplaced_aggregate` | Aggregate or window function outside `Aggregate`/`Window`. |
| `Q3014` | `invalid_member_type` | Union payload not a record, or relation element not a record, union, scalar, array or relation (`query-tree.md` §2.5 rule 2); a range subtype that is not an orderable scalar (§2.19). |
| `Q3015` | `duplicate_member_name` | Duplicate or empty field or tag name in a type (`query-tree.md` §2.3, §2.4). |
| `Q3016` | `union_too_wide` | A union with no variants or more than 65,536 (`query-tree.md` §2.4: `1 ≤ k ≤ 65,536`). |
| `Q3017` | `invalid_literal` | A `Literal` whose type is not a non-null scalar (`query-tree.md` §7.3). |
| `Q3018` | `incomparable_type` | `eq`, `ne` or an ordering, grouping, `Distinct` or set-operation comparison on `xml`, or on a structured type without an ordering (`query-tree.md` §3.2 rule 5, §2.17, §2.20). SQLSTATE 42883. |
| `Q3019` | `invalid_type_parameter` | A type parameter out of its range: a string or bit length, a datetime or `decfloat` precision, an interval qualifier or precision, an array bound (`query-tree.md` §2.7 rule 1). |
| `Q3020` | `literal_out_of_domain` | A `Literal` value that is not a value of its type (`query-tree.md` §2.7 rule 2). |
| `Q3021` | `unknown_collation` | A collation name that names no collation (`query-tree.md` §2.21). SQLSTATE 42704. |
| `Q4001` | `param_index_out_of_range` | Parameter index out of range. |
| `Q4002` | `param_unused` | Declared parameter never referenced. |
| `Q4003` | `param_type_mismatch` | Bound value's type differs from the declaration. |
| `Q4004` | `param_count_mismatch` | Wrong number of bound values. |
| `Q4005` | `null_param` | Null bound to a non-nullable parameter. |
| `Q4006` | `param_value_out_of_domain` | A bound value that is not a value of its parameter's type: too long for its length, not of a `char(n)`'s or `bit(n)`'s length, a non-canonical range (`query-tree.md` §2.7 rule 2). |
| `Q5001` | `invalid_recursion` | Invalid recursive CTE (non-linear, unstratified, mutual, missing column types). |
| `Q5002` | `node_not_allowed` | Node not allowed here (DML not at root, `Action` outside `returning`). |
| `Q5003` | `invalid_dml_target` | DML target is not a table, or column not writable. |
| `Q5004` | `no_conflict_target` | Conflict target matches no unique constraint or index. |
| `Q5005` | `duplicate_assignment` | Column assigned or listed twice. |
| `Q5006` | `correlated_join` | `Join` right input references the left input; use `Apply`. |
| `Q5008` | `invalid_window_frame` | Invalid window frame. |
| `Q5009` | `arity_violation` | Arity or required-field violation (empty items, missing `mark`, row width, `Scalar` width). |
| `Q6001` | `ddl_parameters` | A DDL envelope declares parameters, or binds other than one empty tuple (`ddl.md` §3.1). |
| `Q6002` | `object_exists` | The name is already used in its namespace, or by another role or namespace. |
| `Q6003` | `unknown_object` | Unknown namespace, index, constraint, sequence, view, role or job. |
| `Q6004` | `unknown_type` | A named type reference names no catalog type (`query-tree.md` §12.3 kind 32). |
| `Q6005` | `invalid_stored_type` | A type that cannot be used where it is: a key component that is not orderable or encodable, a nullable primary-key column, a named type that is not a record or union, a recursive named type. |
| `Q6006` | `invalid_definition_expression` | A default, check, generated, index or fill expression that breaks `ddl.md` §2.12 or §3.3 (volatile where deterministic is required, a subquery, a parameter, more than one of default, generated and identity). |
| `Q6007` | `dependent_objects` | A drop or change would break dependent objects and `cascade` is not set, or cannot be cascaded. |
| `Q6008` | `retype_not_permitted` | No permitted conversion from the column's type to the new one, and no `using`. |
| `Q6009` | `missing_primary_key` | A table without a primary key. |
| `Q6010` | `not_in_transaction` | A statement that may not run inside an explicit transaction (`AwaitJobs`). |
| `Q6011` | `table_too_wide` | More than 65,535 columns or record fields, or a fixed area plus key beyond the on-disk limit. |
| `Q6012` | `schema_versions_exhausted` | The table's next schema version would exceed 65,535; rewrite the table. |
| `Q6013` | `foreign_key_mismatch` | Foreign-key columns do not match a unique key of the referenced table, or `set_null` on a non-null column. |
| `Q6014` | `reserved_namespace` | DDL on an object in `sys` or `catalog`, or dropping, renaming or giving `login` to the predefined role `terndb_replication` (`ddl.md` §2.10 rule 7). |
| `Q6015` | `insufficient_privilege` | The role lacks a privilege the statement needs, or does not own the object (`ddl.md` §2.10). Raised for every statement kind. |
| `Q6016` | `conflicting_alter_actions` | Two actions of one `AlterTable` change the same column. |
| `Q6017` | `invalid_sequence_options` | Sequence options that are inconsistent (zero increment, `min > max`, start outside the range). |
| `Q6018` | `type_change_not_permitted` | An `AlterType` change other than appending or renaming members. |
| `Q6019` | `invalid_name` | A name that is empty, longer than 255 octets or not UTF-8. |
| `Q6020` | `circular_membership` | A role membership that would make a role a member of itself, directly or through others (`ddl.md` §2.10 rule 9). |
| `Q6021` | `privilege_not_applicable` | A `Grant` or `Revoke` of a privilege that does not apply to the object's kind (`ddl.md` §2.10 rules 3, 9). |
| `Q6022` | `row_cannot_fit` | A table whose smallest row (header, key, null bitmap, fixed-width columns, one offset per variable-width column) exceeds the 4,000-octet inline limit, so no row of it could ever be stored; raised by `CreateTable` and by `AlterTable` adding columns (`ddl.md` §2.2 rule 4, §2.13 rule 10). |
| `Q6023` | `column_retyping` | A `retype_column` of a column whose earlier retype is still `converting`: the conversion must finish first (`ddl.md` §2.13 rule 9, §8.5 rule 4). |
| `Q6024` | `column_filling` | A `retype_column` of a column that is still `filling`: its `fill_column` job must finish first (`ddl.md` §2.13 rule 11, §8.5 rule 5). |
| `Q7001` | `substrait_relation_unsupported` | Substrait relation not supported. |
| `Q7002` | `substrait_type_unsupported` | Substrait type not supported. |
| `Q7003` | `substrait_function_unmapped` | Substrait function not in the mapping. |
| `Q7004` | `substrait_version_unsupported` | Substrait plan version is not the one this release pins. |
| `Q8001` | `sql_syntax_error` | SQL text has a token the grammar does not allow there, or ends too early (`sql.md` §3–§6). |
| `Q8002` | `sql_unterminated` | A string constant, quoted identifier or block comment is not terminated (`sql.md` §2). |
| `Q8003` | `sql_invalid_token` | A character that starts no token, a malformed number, parameter or escape, or an empty quoted identifier (`sql.md` §2). |
| `Q8004` | `sql_invalid_constant` | A constant that is not a value of its type: out of range, or a text that does not parse (`sql.md` §2.5, §2.6, §5.9). |
| `Q8005` | `sql_unsupported` | A construct of SQL that TernDB's dialect does not have (`sql.md` §8). |
| `Q8006` | `sql_invalid_type_name` | An unknown or refused type name, or a bad type modifier (`sql.md` §4). |
| `Q8007` | `sql_undetermined_type` | The type of a `NULL`, a parameter or a `VALUES` column cannot be determined, or a parameter is given two types (`sql.md` §2.7, §5.3, §5.9). |
| `Q8008` | `sql_ambiguous_column` | An unqualified column belongs to more than one source (`sql.md` §5.2). |
| `Q8009` | `sql_ungrouped_column` | A grouped query reads a column of its input outside an aggregate and does not group by it (`sql.md` §5.5). |
| `Q8010` | `sql_columns_unknown` | A construct needs a table's column list and the front end does not have it (`sql.md` §1 rule 4). |
| `Q8011` | `sql_order_not_in_select` | An `ORDER BY` or `GROUP BY` position out of range, or an `ORDER BY` that must name an output column and does not (`sql.md` §5.5, §5.6). |
| `Q8012` | `sql_column_count_mismatch` | A column list or `VALUES` row of the wrong length (`sql.md` §3.2, §5.3, §5.4). |
| `Q8013` | `sql_invalid_table_definition` | Two primary keys, a column given twice, `NULL` with `NOT NULL`, or two defaults (`sql.md` §3.4). |
| `Q8014` | `sql_collation_conflict` | Two operands whose implicit collations differ and no `COLLATE` decides (`sql.md` §4.2). SQLSTATE 42P21. |

## 3. Query tree: runtime (`R`)

Raised during evaluation. None is retryable or fatal.

| Code | Name | Meaning |
|---|---|---|
| `R0001` | `arithmetic_overflow` | Arithmetic overflow, or a result outside its type's range. |
| `R0002` | `division_by_zero` | Division by zero. |
| `R0003` | `more_than_one_row` | More than one row for a scalar subquery or `single` join. |
| `R0004` | `null_asserted` | `assert_not_null` on null. |
| `R0005` | `recursion_limit` | Recursion bound exceeded. |
| `R0006` | `invalid_value` | A value does not convert (cast failure), or is out of a node's domain (negative limit, a null or negative window frame offset, a negative `lag`/`lead` offset), or names no time zone (`at_zone`, `from_zone`, `query-tree.md` §7.6). |
| `R0007` | `row_affected_twice` | A target row affected twice by one statement. |
| `R0008` | `sequence_exhausted` | `next_value` or an identity column would pass a non-cycling sequence's bound, or draws from one already exhausted (`ddl.md` §2.7 rules 4 and 6). |
| `R0009` | `string_data_right_truncation` | A value stored into a `varchar(n)`, `char(n)`, `varbinary(n)`, `binary(n)` or `varbit(n)` is longer than `n` beyond trailing spaces (or X'00') (`query-tree.md` §2.8 rule 2, §2.9, §2.18). SQLSTATE 22001. |
| `R0010` | `string_data_length_mismatch` | A `bit(n)` value stored of another length, or bitwise operands of different lengths (`query-tree.md` §2.18 rule 3). SQLSTATE 22026. |
| `R0011` | `array_element_error` | An array subscript outside the array's bounds (`query-tree.md` §2.13 rule 6). SQLSTATE 2202E. |
| `R0012` | `array_data_right_truncation` | An array stored into, or cast to, an array type of smaller maximum cardinality (`query-tree.md` §2.13 rule 3). SQLSTATE 2202F. |
| `R0013` | `invalid_xml_document` | A text parsed as an XML document is not one (`query-tree.md` §2.17 rule 3). SQLSTATE 2200M. |
| `R0014` | `invalid_xml_content` | A text parsed as XML content is not well-formed (`query-tree.md` §2.17 rule 3). SQLSTATE 2200N. |
| `R0015` | `invalid_range_bounds` | A range whose lower bound is above its upper bound (`query-tree.md` §2.19 rule 1). SQLSTATE 22000. |
| `R0016` | `range_not_contiguous` | A range union or difference whose result is not one range (`query-tree.md` §2.19 rule 5). SQLSTATE 22000. |

## 4. Transactions and isolation (`T`)

Raised under the rules of `isolation.md` §5. Levels: SER serializable, SI snapshot, RC read committed.

| Code | Name | Raised when | Levels | R |
|---|---|---|---|---|
| `T1001` | `serialization_failure` | first-updater-wins conflict (`isolation.md` §4.2 rule 2 or 4, including a unique or referenced value found only through a commit after the snapshot, `ddl.md` §2.6 rules 3 and 5); validation failure (§4.3) | SI, SER | yes |
| `T1002` | `deadlock_detected` | the wait-for graph of row locks has a cycle; the victim aborts | all | yes |
| `T1003` | `lock_timeout` | a row-lock wait exceeded `lock_timeout`; a deferred foreign-key check found its referenced row locked by another live transaction at commit, which does not wait (`ddl.md` §2.6 rule 3), as did reconciliation with a unique key added during the transaction (`ddl.md` §6.4 rule 1) | all | yes |
| `T1004` | `schema_changed` | a DDL committed during its lifetime changed a part of a definition this transaction read (§5.6, `ddl.md` §6.3), a DDL statement of the transaction no longer applies to the newest committed catalog at its commit (`ddl.md` §9 rule 8), or a write would re-encode a row in an older schema version (`ddl.md` §5.4) | all | yes |
| `T1005` | `replica_lag_timeout` | `after` or `max_lag` not satisfied within `replica_wait_timeout` (§3.6) | replica | yes |
| `T1006` | `replica_conflict` | WAL replay needed to remove a structure or undo this replica transaction uses (§5.6) | replica | yes |
| `T1007` | `cancelled` | the client, an administrator or a shutdown cancelled it before commit | all | yes |
| `T1008` | `resource_exhausted` | memory (including every buffer-pool frame fixed, `details.frames`), undo space or disk full, or a snapshot past `max_snapshot_age` or `undo_retention_limit` (§5.5); the transaction is aborted | all | yes, after back-off |
| `T2001` | `commit_unknown` | the server cannot say whether the commit happened (e.g. it lost the WAL device mid-commit) | all | **no**: resolve first (§5.4) |
| `T2002` | `commit_not_confirmed` | a commit at `remote` or `remote_apply` waited `synchronous_commit_timeout` for its quorum; the transaction **is committed** on the primary (`replication.md` §6.4) | all | no |
| `T2003` | `token_not_in_history` | `after` names a commit this server's history does not contain, because a failover lost it (§3.6 R3, `replication.md` §8.3) | all | no |
| `T3001` | `unique_violation` | a unique constraint fails on data visible to the transaction | all | no |
| `T3002` | `check_violation` | a check constraint fails | all | no |
| `T3003` | `foreign_key_violation` | a foreign-key constraint fails | all | no |
| `T3004` | `not_null_violation` | a null is written to a `NOT NULL` column | all | no |
| `T4001` | `read_only_transaction` | a write in a read-only transaction or on a replica | all | no |

## 5. Wire (`W`)

Raised under the rules of `wire-protocol.md`. Section references below are to that document.

| Code | Name | Meaning | F | R |
|---|---|---|---|---|
| `W1001` | `malformed_message` | Malformed frame or message: bad reserved field or flag, unknown or wrong-direction type, payload length mismatch, non-canonical primitive | yes | |
| `W1003` | `message_too_large` | Frame larger than `max_frame_size`, or message larger than `max_message_size` | yes | |
| `W1004` | `out_of_order` | Message out of order for the handshake state | yes | |
| `W1005` | `no_common_wire_version` | No common wire protocol major version | yes | |
| `W1006` | `bad_request_id` | Request id zero, or not greater than every earlier one | yes | |
| `W1007` | `fragmentation_error` | Empty `MORE` frame, type change within a message | yes | |
| `W1008` | `bad_batch` | (client-side) The server sent a batch or a result type inconsistent with §6, including a truncated or non-canonical batch | yes | |
| `W2001` | `unknown_transaction` | Unknown transaction handle | | |
| `W2002` | `transaction_already_open` | Transaction handle already open | | |
| `W2003` | `transaction_aborted` | Transaction is aborted; only `Rollback`, `RollbackToSavepoint`, `Commit` accepted; also a `RollbackToSavepoint` after a retryable error, which rolled the transaction back past every savepoint (§5.5 rule 6) | | |
| `W2004` | `unknown_plan` | Shape digest not in the role's plan cache; resend the tree | | yes |
| `W2005` | `unknown_prepared` | Unknown prepared handle | | |
| `W2006` | `prepared_already_registered` | Prepared handle already registered | | |
| `W2007` | `result_type_changed` | Prepared statement's result type changed after a catalog change; handle released | | |
| `W2008` | `too_many_in_flight` | Too many in-flight requests on the connection | | yes |
| `W2010` | `status_horizon_passed` | `txid` older than `status_horizon` (`isolation.md` §5.4) | | |
| `W2011` | `invalid_option` | Invalid or misplaced option (out-of-range value, `implicit` with `txn ≠ 0`) | handshake only | |
| `W2012` | `unknown_savepoint` | Unknown savepoint | | |
| `W2014` | `row_not_encodable` | A single row exceeds an encoding limit (§6.3.8) or `max_message_size` | | |
| `W2016` | `too_many_transactions` | Too many open transactions on the connection | | |
| `W2018` | `unsupported_request` | A request this server does not support; like any error in a transaction it aborts it (§5.3). No request of this protocol version raises it: every server implements savepoints (§5.5) | | |
| `W2017` | `union_not_encodable` | *Withdrawn*: a union wider than 65,536 variants is now refused by validation (`Q3016`) | | |
| `W3001` | `authentication_failed` | Authentication failed | yes | |
| `W3002` | `no_auth_method` | No acceptable authentication method | yes | |
| `W3003` | `tls_required` | Plaintext connection on TCP | yes | |
| `W3004` | `unknown_database` | Unknown database | yes | |
| `W4001` | `shutting_down` | Server shutting down (in `Goodbye`, and for a request after it); retryable on another connection | | yes |
| `W4002` | `idle_timeout` | Idle connection closed (in `Goodbye`) | | |

## 6. Storage (`D`)

Raised under the rules of `on-disk-format.md`. None is retryable.

| Code | Name | Meaning |
|---|---|---|
| `D1001` | `data_corrupted` | A checksum or structural check failed; the server stops serving the object (`isolation.md` §8, `on-disk-format.md` §3.1). |
| `D2001` | `key_too_long` | An encoded key longer than 1,024 bytes (`on-disk-format.md` §3.6). |
| `D2002` | `entry_too_large` | An entry cannot be brought within 4,000 bytes by moving columns out of line (§4.7). |
| `D2003` | `wal_record_unwritable` | A WAL record or mini-transaction the format cannot express: a field past its width, a flag its kind cannot carry, or more bytes than one segment holds (§6.2 rule 2, §6.3, §6.4). |
| `D3001` | `format_unsupported` | The database's format is newer than this release, or a feature flag it does not know is set (§7.2, §3.10); the open is refused. |
| `D3002` | `format_read_only` | A write to a database opened read-only because its format is older than this release can write (§7.2). |
| `D3003` | `clone_unsupported` | An on-disk clone on a filesystem without reflink (§8.4). |
| `D4001` | `database_locked` | Another process, or another open in this one, holds the database's `CONTROL` lock (§2 rule 4; `c-api.md` §4.2). |
| `D4002` | `database_not_found` | Open with `create = never` of a directory that holds no database (`c-api.md` §4.2). |
| `D4003` | `database_exists` | Open with `create = always`, or a snapshot or clone into a directory, that already holds a `CONTROL` (`c-api.md` §4.2, §4.4). |
| `D4004` | `io_error` | A file of the database could not be read, created or synced (`details.errno`, `details.file`). |

## 7. C ABI (`C`)

Raised by `libterndb` itself under the rules of `c-api.md`, never sent on the wire. None is retryable or fatal.

| Code | Name | Meaning |
|---|---|---|
| `C1001` | `invalid_handle` | A freed handle, or a handle of the wrong kind (checked in debug builds, `c-api.md` §2.4). |
| `C1002` | `invalid_argument` | A NULL where one is required, an out-of-range index or value, an unknown or inapplicable configuration key. |
| `C1003` | `wrong_thread` | A blocking call from an engine thread: the log sink or an allocator function (`c-api.md` §2.3). |
| `C1004` | `concurrent_use` | A connection, or a transaction or statement of it, used by two threads at once. |
| `C1005` | `invalid_state` | A call that the handle's state does not allow yet (result counts before the end, an operation's error while pending, a second `terndb_init` with other arguments). |
| `C1006` | `closed` | A call on a handle whose connection or database has been closed. |
| `C1007` | `not_supported` | An operation the database's mode does not support (snapshot or promotion of a disk-mode database). |
| `C1008` | `not_initialized` | A call before `terndb_init`. |
| `C1009` | `abi_mismatch` | The caller's header is of another ABI major, or a newer minor, than the library. |
| `C2001` | `out_of_memory` | The allocator returned NULL outside any transaction. |

## 8. Replication (`S`)

Raised under the rules of `replication.md`, on replication connections (§2.1 there) and by the administrative
promotion of a replica. A request that fails with one of them ends its stream; none is fatal to the connection.
Section references below are to `replication.md`.

| Code | Name | Meaning | R |
|---|---|---|---|
| `S1001` | `not_primary` | The node is not the primary of its term, or is a fenced old primary; on a fenced primary every new transaction gets it (§5.1, §10.4) | yes, on another node |
| `S1002` | `database_mismatch` | The peer's `db_uuid` is not this database's (§3, §5.1) | |
| `S1003` | `history_diverged` | The replica's history does not agree with the primary's at `start_lsn`, or a requested history file is unknown; rewind or re-seed (§1.2, §5.1, §10.6) | |
| `S1004` | `wal_unavailable` | The WAL at `start_lsn` is in neither `wal/` nor `archive/` (§5.1, §5.7) | |
| `S1005` | `wal_format_unsupported` | The primary's WAL format is above what the replica can apply (§5.1, §11) | |
| `S1006` | `replication_not_permitted` | The role is neither a superuser nor a member of `terndb_replication` (§2.2) | |
| `S1007` | `too_many_replicas` | `max_replicas` streams are already open (§5.1) | yes |
| `S1008` | `stale_term` | A `Fence` whose term does not exceed the node's fence term (§10.2); `details.fence_term` | |
| `S1009` | `connection_in_replication_mode` | A non-replication request on a replication connection, a replication request on another connection, or a second replication stream (§2.1) | |
| `S2001` | `unknown_slot` | The named slot does not exist and `create_slot` is false (§5.1) | |
| `S2002` | `slot_in_use` | The slot, or the synchronous replica name, is held by another stream (§4.2, §5.1, §6.2) | |
| `S2003` | `slot_invalidated` | The slot fell more than `max_slot_wal_keep` behind and no longer retains WAL (§5.8) | |
| `S3001` | `fence_quorum_unreached` | Promotion could not fence `n − k + 1` members of the synchronous set within `promotion_timeout`, or `fence_command` failed (§10.2) | |
| `S3002` | `not_most_advanced` | A fenced member has flushed more WAL than the candidate; `details.member` names it (§10.2) | |
| `S3003` | `term_exhausted` | A new term would exceed 65,535 (§1.2) | |
| `S3004` | `not_a_replica` | Promotion asked of a node that is not a replica (§10.2) | |
