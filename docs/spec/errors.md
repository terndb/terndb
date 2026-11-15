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
   the `Q6…` group is DDL validation, `ddl.md`),
   `R` runtime (evaluation, `query-tree.md`), `T` transactions and isolation (`isolation.md`), `W` wire
   (`wire-protocol.md`), `D` storage (`on-disk-format.md`). The first digit groups related codes within an area.
2. Every code has a **name**, unique across the registry, in `snake_case`. The name is an alias: specs, client
   libraries and server messages MAY use either, and they mean the same thing.
3. On the wire and across the C ABI, `ErrorEntry.code` carries the **code**, never the name (`wire-protocol.md`
   §8.1). Clients and tests match on the code.
4. Codes and names are never reused. A withdrawn code stays reserved, with its row kept and marked *withdrawn*.
5. **Retryable** (column R): the request, and for a `T` code the transaction, had no effect and MAY be re-run from
   its beginning (`isolation.md` §5.3). **Fatal** (column F): the connection is closed after the error
   (`wire-protocol.md` §8.1 rule 8). A code with neither mark is neither.

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
| `Q3011` | `decimal_out_of_range` | Decimal precision or scale out of range. |
| `Q3012` | `cast_not_permitted` | Cast not permitted. |
| `Q3013` | `misplaced_aggregate` | Aggregate or window function outside `Aggregate`/`Window`. |
| `Q3014` | `invalid_member_type` | Union payload not a record, or relation element not a record, union or scalar. |
| `Q3015` | `duplicate_member_name` | Duplicate or empty field or tag name in a type (`query-tree.md` §2.3, §2.4). |
| `Q3016` | `union_too_wide` | A union with no variants or more than 65,536 (`query-tree.md` §2.4: `1 ≤ k ≤ 65,536`). |
| `Q3017` | `invalid_literal` | A `Literal` whose type is not a non-null scalar (`query-tree.md` §7.3). |
| `Q4001` | `param_index_out_of_range` | Parameter index out of range. |
| `Q4002` | `param_unused` | Declared parameter never referenced. |
| `Q4003` | `param_type_mismatch` | Bound value's type differs from the declaration. |
| `Q4004` | `param_count_mismatch` | Wrong number of bound values. |
| `Q4005` | `null_param` | Null bound to a non-nullable parameter. |
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
| `Q6014` | `reserved_namespace` | DDL on an object in `sys` or `catalog`. |
| `Q6015` | `insufficient_privilege` | The role lacks a privilege the statement needs, or does not own the object (`ddl.md` §2.10). Raised for every statement kind. |
| `Q6016` | `conflicting_alter_actions` | Two actions of one `AlterTable` change the same column. |
| `Q6017` | `invalid_sequence_options` | Sequence options that are inconsistent (zero increment, `min > max`, start outside the range). |
| `Q6018` | `type_change_not_permitted` | An `AlterType` change other than appending or renaming members. |
| `Q6019` | `invalid_name` | A name that is empty, longer than 255 octets or not UTF-8. |
| `Q7001` | `substrait_relation_unsupported` | Substrait relation not supported. |
| `Q7002` | `substrait_type_unsupported` | Substrait type not supported. |
| `Q7003` | `substrait_function_unmapped` | Substrait function not in the mapping. |
| `Q7004` | `substrait_version_unsupported` | Substrait plan version is not the one this release pins. |

## 3. Query tree: runtime (`R`)

Raised during evaluation. None is retryable or fatal.

| Code | Name | Meaning |
|---|---|---|
| `R0001` | `arithmetic_overflow` | Arithmetic overflow, or a result outside its type's range. |
| `R0002` | `division_by_zero` | Division by zero. |
| `R0003` | `more_than_one_row` | More than one row for a scalar subquery or `single` join. |
| `R0004` | `null_asserted` | `assert_not_null` on null. |
| `R0005` | `recursion_limit` | Recursion bound exceeded. |
| `R0006` | `invalid_value` | A value does not convert (cast failure), or is out of a node's domain (negative limit). |
| `R0007` | `row_affected_twice` | A target row affected twice by one statement. |
| `R0008` | `sequence_exhausted` | `next_value` would pass a non-cycling sequence's bound (`ddl.md` §2.7). |

## 4. Transactions and isolation (`T`)

Raised under the rules of `isolation.md` §5. Levels: SER serializable, SI snapshot, RC read committed.

| Code | Name | Raised when | Levels | R |
|---|---|---|---|---|
| `T1001` | `serialization_failure` | first-updater-wins conflict (`isolation.md` §4.2 rule 2 or 4); validation failure (§4.3) | SI, SER | yes |
| `T1002` | `deadlock_detected` | the wait-for graph of row locks has a cycle; the victim aborts | all | yes |
| `T1003` | `lock_timeout` | a row-lock wait exceeded `lock_timeout` | all | yes |
| `T1004` | `schema_changed` | a DDL committed during its lifetime changed a part of a definition this transaction read (§5.6, `ddl.md` §6.3), or a write would re-encode a row in an older schema version (`ddl.md` §5.4) | all | yes |
| `T1005` | `replica_lag_timeout` | `after` or `max_lag` not satisfied within `replica_wait_timeout` (§3.6) | replica | yes |
| `T1006` | `replica_conflict` | WAL replay needed to remove a structure or undo this replica transaction uses (§5.6) | replica | yes |
| `T1007` | `cancelled` | the client, an administrator or a shutdown cancelled it before commit | all | yes |
| `T1008` | `resource_exhausted` | memory, undo space or disk full, or a snapshot past `max_snapshot_age` or `undo_retention_limit` (§5.5); the transaction is aborted | all | yes, after back-off |
| `T2001` | `commit_unknown` | the server cannot say whether the commit happened (e.g. it lost the WAL device mid-commit) | all | **no**: resolve first (§5.4) |
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
| `W2003` | `transaction_aborted` | Transaction is aborted; only `Rollback`, `RollbackToSavepoint`, `Commit` accepted | | |
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
| `D3001` | `format_unsupported` | The database's format is newer than this release, or a feature flag it does not know is set (§7.2, §3.10); the open is refused. |
| `D3002` | `format_read_only` | A write to a database opened read-only because its format is older than this release can write (§7.2). |
| `D3003` | `clone_unsupported` | An on-disk clone on a filesystem without reflink (§8.4). |
