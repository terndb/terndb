<p align="center"><img src="docs/assets/terndb-mascot.png" alt="TernDB" width="320"></p>

# TernDB

A relational, ACID database written in [sysl](https://github.com/sysl-lang/sysl). A query result can be a nested relation, so a
GraphQL-shaped answer of users, their posts and each post's comments comes back from one query, planned as one
batched join per level. The native input is a typed query tree, and SQL and GraphQL are front ends that produce it.
One storage engine runs in memory for development and tests, and on disk for production.

TernDB is in design. What runs so far is an in-memory server over a Unix-domain socket:

```
sysl build server
TERNDB_PASSWORD=secret server/terndb --socket /tmp/terndb.sock
```

A client logs in as the role `terndb` with that password (SCRAM-SHA-256) and speaks the
[native protocol](docs/spec/wire-protocol.md). TCP needs TLS, which is not served yet. Flags and design:
[the server](docs/design/engine.md#the-server).

## What it does differently

- **Nested results.** Relation-valued columns are a first-class type at any depth, with tagged-union elements. The
  planner decorrelates each level instead of running a subquery per parent.
- **Trees, not text.** Clients send a typed query tree. Values are always parameters, so injection is impossible by
  construction, and plans are cached by the tree's shape.
- **No VACUUM.** Undo-log MVCC keeps the newest version in place, and old versions go when no snapshot needs them.
- **Serializable by default**, at close to the cost of snapshot isolation.
- **Embedded or server.** The same engine is a library behind a C ABI (`libterndb`) and a server (`terndb`).
- **In-memory mode.** It has the same semantics as the on-disk mode, plus copy-on-write database clones for
  per-test fixtures.

## Specifications

| Spec | Covers |
|---|---|
| [Query tree](docs/spec/query-tree.md) | Node kinds, the type system, parameters, versioning, Substrait |
| [Wire protocol and result format](docs/spec/wire-protocol.md) | The native protocol, and the one nested result encoding used by both the C ABI and the wire |
| [On-disk format](docs/spec/on-disk-format.md) | Pages, the WAL, undo records, format versioning |
| [Isolation](docs/spec/isolation.md) | What each isolation level guarantees, written as a checkable history model |
| [C ABI](docs/spec/c-api.md) | `libterndb`'s header: handles, threads, sync and async execution, results as bytes or walked, errors, versioning |
| [DDL and the catalog](docs/spec/ddl.md) | Catalog objects, DDL statement trees, the catalog as relations, schema versions and online schema change |
| [Replication](docs/spec/replication.md) | One primary and streaming read replicas: base backup, the WAL stream, synchronous commit, replica reads, failover with fencing, rewind |
| [Error codes](docs/spec/errors.md) | The one registry of every error code and its name, across all the specs |
| [Decisions](docs/decisions.md) | The answers applied to the specs' open questions, each with what to revert |

## Why "TernDB"?

A tern flies, dives and swims. TernDB works in more than one element too: the sky is memory and the water is disk.
The name is a backronym for *TERN Engine: Relational and Nimble*.

## Licence

MIT. See [LICENSE](LICENSE).
