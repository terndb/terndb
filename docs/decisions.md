# TernDB spec decisions

These are the answers applied to the open questions of the four specs in `docs/spec/`. Each was the director's
recommendation, applied so the specs could move on; **the user may overrule any of them**. Every row names exactly
the spec sections it changed, so a decision is reverted by restoring those sections and putting its question back
in that spec's "Open questions". Abbreviations: QT `query-tree.md`, ISO `isolation.md`, DISK `on-disk-format.md`,
WIRE `wire-protocol.md`, ERR `errors.md`.

## Query tree, types and values

| Id | Question | Decision | Sections changed |
|---|---|---|---|
| Q1 | Literals in the tree? | Add `Literal` nodes for constants of the statement; they are part of the shape and the plan-cache key. Values that vary stay parameters. Injection stays impossible because trees are structured. | QT §1.2, §7 table, §7.3, §10.2, §12.4 (Expr kind 17), §12.6 heading and closing paragraph, §14.1 (literal rows), §15; ERR `Q3017`, `Q1007` |
| Q2 | Unsigned integers and `int8`? | Integers are `int8…int64` and `uint8…uint64` (`int128` kept; see QT Open question 1). | QT §2.1, §4.1 rules 1–2, §4.4 rule 1, §12.3 (kinds 21–25), §12.6, §14.1 types row, §14.2 (`i8` row removed); DISK §4.3, §4.4, §4.6; WIRE §6.2 table, §6.8 |
| Q3 | Unbounded `numeric`? | None in v1; `decimal` takes up to 38 digits. | QT §2.1 note 1; DISK §4.3 (`decimal` 1–38), §4.4 (p > 38 row removed), §4.6 |
| Q4 | `list<int64>` directly? | Yes: scalar relations, exposing one column `value`. | QT §2.5 rule 2, §6.7 (`collect`), §6.14, §14.1, §14.2; DISK §4.3 (tag 0x22), §4.4, §4.5, §4.6; WIRE §6.2 rule 6; ERR `Q3014` |
| Q5 | A `json` type? | Yes, a `json` scalar for interop, held in canonical (RFC 8785) form. Nesting stays relational. | QT §2.1, §3.1, §3.2, §3.3, §3.5, §4.1, §12.3 (kind 26), §12.6; DISK §4.3 (tag 0x17), §4.4, §4.6; WIRE §6.2, §6.8; ERR `Q1007` |
| Q6 | Collations? | None in v1 (binary UTF-8 order). Later they go on the comparison, with ICU sort keys in indexes; hook reserved. | QT §3.1 rule 4; DISK §3.9 (`key_spec` bit 2), §4.6 rule 4 |
| Q7 | Format of the published tree schema? | A JSON description generated from the implementation's source. | QT §13.1 |
| Q8 | Support window for majors? | A server accepts its current major and the two previous ones. | QT §13.3 |
| Q9 | Data-modifying CTEs, grouping sets? | Deferred from 1.0, as the spec already said. | none (question removed from QT §17) |
| Q10 | Substrait outbound, version range? | Import-only, pinned to one Substrait version per release. | QT §14 intro; ERR `Q7004` |
| Q11 | `arbitrary: true` for `Limit` over a bag? | Kept. The GraphQL front end orders by primary key instead of setting it. | QT §6.10 rule 2 |
| Q12 | Canonical text forms of values? | A short "values" section in QT. | QT §3.5 (new), §7.6 |

## Isolation

| Id | Question | Decision | Sections changed |
|---|---|---|---|
| Q13 | Wait or abort on a locked row? | Wait, bounded by `lock_timeout` and deadlock detection; the only policy. | ISO §4.2 rule 1 |
| Q14 | Element-level versioning for nested relations? | None in v1: one value per row, stored through the overflow chain. | ISO §4.5 closing paragraph; DISK §4.5 (`form`), §4.8 rule 5 |
| Q15 | Projection-aware validation? | Yes: validation is column-precise; only columns read are checked. | ISO §2.2 (item read), §2.3 (rw), §4.3 (pseudo-code, Precision), §4.6 (serializable proof) |
| Q16 | `synchronous = remote`? | In Phase 3, with replication. | ISO §6, §8 |
| Q17 | Fencing the old primary? | A term number in the WAL; details in `replication.md`. | ISO §8; DISK §6.6 (`term` at offset 36) |
| Q18 | Bounded-staleness replica reads? | Optional `max_lag`. | ISO §3.6 (R3a); WIRE §3.5 `TxnOptions.max_lag_ms`; ERR `T1005` |
| Q19 | `max_snapshot_age` default? | 1 hour, configurable (0 = no limit). | ISO §5.5; DISK §5.5; ERR `T1008` |
| Q20 | Language of the model? | sysl, run inside the simulator in CI. | ISO §7.4 |
| Q21 | Online DDL? | Its own spec, `ddl.md`, next. | ISO §5.6 (last bullet) |
| Q22 | `REPEATABLE READ`, `READ UNCOMMITTED` synonyms? | Accepted only in the Postgres wire layer. | ISO §1 |

## On-disk format

| Id | Question | Decision | Sections changed |
|---|---|---|---|
| Q23 | Overflow values in the WAL? | Written in full to the WAL in v1. | DISK §4.8 rule 4 |
| Q24 | Replica feedback? | Only through opt-in feedback (`replica_feedback`, off by default); otherwise the replica query is cancelled with `replica_conflict`. | DISK §5.5, §6.11 rule 3; ISO §5.6 |
| Q25 | Writing the previous format? | A release can still write format N−1 until `--upgrade-format`. | DISK §7.2 rules 2–5; ERR `D3001`, `D3002` |
| Q26 | Per-core undo lanes? | From the start. | DISK §5.1 |
| Q27 | In-engine CoW clones on disk? | No: on-disk clones are reflink copies only. | DISK §8.4; ERR `D3003` |
| Q28 | Asynchronous commit? | It is `synchronous = off`; no separate feature. | ISO §6; DISK §6.7 rule 2 |
| Q29 | Encryption at rest? | Reserve the fields now: CONTROL `feature_flags` bit 1, page `key_version`, WAL segment `key_version`. | DISK §2.2, §3.1 (`owner` narrowed to 48 bits, rule 2), §3.9, §6.6 |

## Wire protocol

| Id | Question | Decision | Sections changed |
|---|---|---|---|
| W1 | Default port? | 7365. | WIRE §1.1 rule 1 |
| W2 | `bool` width in results? | One octet, as specified. | none (question removed) |
| W3 | Columnar parameter input? | Bulk relation parameters may use the result encoding. | WIRE §4.2 (`columnar` list, rules 2–5); QT §10.4 |
| W4 | Mutual TLS? | In Phase 2. | WIRE §2.4 rule 9 |
| W5 | Token format? | Opaque random tokens, stored hashed. | WIRE §2.4 rule 6 |
| W6 | Plaintext TCP? | Never; local unencrypted access is the Unix socket with `peer` auth. | WIRE §1.1 rules 3–4, §1.2, §2.4 rule 3; ERR `W3003` |
| W7 | Plan-cache scope for digests? | Keyed per role. | WIRE §4.4 rule 5; ERR `W2004` |
| W8 | Compression? | zstd per batch, negotiated, off by default. | WIRE §3.4 (`ClientHello.compression`, `ServerHello.compression`), §6.4 rule 7 |
| W9 | Notifications? | Later; the message types stay reserved. | WIRE §3.2 |
| W10 | Ordered implicit transactions? | No. | WIRE §4.6 rule 3 |
| W11 | `ExecuteSql`? | No. | WIRE §0 rule 4 |
| W12 | Arrow C Data Interface? | Export comes later. | WIRE §6.8 rule 3 |
| W13 | Union discriminant width? | Keep 1 or 2 octets by arity; unions capped at 65,536 variants. | WIRE §6.2 rule 5; QT §2.4 rule 1; ERR `Q3016`, `W2017` withdrawn |
| W14 | Error-code style? | One style everywhere: area letter (`Q R T W D`) and four digits, with a `snake_case` name as alias, registered in the new `docs/spec/errors.md`. | ERR (new); QT §11.3; ISO §5.2; WIRE §8.1 rules 1, 5, 8, §8.2, §8.3; DISK §1 rule 9 and the sites that now name `D` codes (§3.1, §3.6, §3.10, §4.7, §7.2, §8.4) |
