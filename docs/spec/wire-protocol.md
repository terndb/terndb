# TernDB native wire protocol and result encoding

**Status:** normative, draft. **Wire protocol version:** 1.0. **Tree schema version carried:** 1.0.

This document defines how a client talks to a TernDB server over a byte stream, and the **one nested result
encoding** that the wire and the C ABI (`libterndb`) both use. A client in any language can be written from this
document together with `query-tree.md` (which defines the trees it sends and the type system its results use) and
`isolation.md` (which defines what its transactions guarantee). Nothing in it depends on sysl.

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as in RFC 2119. Every MUST is meant to be
checkable by a test that sends bytes and inspects the bytes, the error code or the effect that comes back; §10
lists the properties and the golden vectors. Text marked *Rationale* explains a choice and is not itself a rule.
Text marked *Informative* has no force.

References: `query-tree.md` is cited as **[QT]**, `isolation.md` as **[ISO]**. RFC 5802 and RFC 7677 (SCRAM),
RFC 9266 (TLS channel binding `tls-exporter`), RFC 8446 (TLS 1.3), RFC 7301 (ALPN).

## 0. Scope and relation to other protocols

1. This is TernDB's **native protocol**: typed query trees in, nested columnar results out. Every TernDB typed
   client (the sysl client, the avin client, the C ABI's network mode) uses it.
2. **The Postgres wire protocol is a separate Phase 2 compatibility layer**, served on its own port by its own
   front end, which parses SQL text in TernDB's dialect, builds query trees, and runs them through the same
   validator and executor. It shares nothing with this protocol on the wire, and nothing in this document depends
   on it. A result delivered through it is flattened to Postgres rows by that layer; this protocol never flattens.
3. **Replication** (the WAL stream from a primary to its replicas) is deferred to its own specification,
   `replication.md`. This document reserves message types for it (§3.2) and says nothing else about it.
4. **DDL** reaches the server only as SQL text until `ddl.md` exists ([QT] §1.5). The SQL front end is not part of
   this protocol in version 1.0; see Open question 11.

## 1. Transport and framing

### 1.1 Transports

1. A server MUST accept connections on TCP and SHOULD accept them on a Unix-domain stream socket. The default TCP
   port is provisionally **7365** (Open question 1).
2. On TCP the connection MUST be **direct TLS 1.3**: the client's first bytes are a TLS ClientHello, with ALPN
   protocol id `terndb`. There is no in-band upgrade request.
   *Rationale: direct TLS costs no extra round trip (Postgres's `SSLRequest` costs one) and leaves no plaintext
   prefix for a downgrade attack.*
3. A server MAY be configured to accept **plaintext TCP**, but only on loopback addresses. It tells the two apart by
   the first octet: `0x16` is a TLS record, `0x54` (`T`, the first octet of the preface, §1.2) is plaintext. Any
   other first octet MUST close the connection without a response.
4. A Unix-domain socket carries no TLS. Its security is the file system's.
5. A server MUST support at least 100,000 concurrently open connections. Connection state that is not in use (no
   open transaction, no in-flight request) SHOULD cost at most 16 KiB of server memory, so that clients need no
   connection pooler.
   *Rationale: the server is thread-per-core with asynchronous I/O; a connection is a socket and a small state
   record, not a process.*

### 1.2 Preface

Immediately after the TLS handshake (or at once on a plaintext or Unix connection), the client MUST send the
8-octet **preface** `54 45 52 4E 44 42 0D 0A` (`TERNDB\r\n`). The server MUST check it, and MUST close the
connection without a response if it differs. The server sends no preface.
*Rationale: a client that reached an HTTP server, a Postgres server or a stray port fails at once and visibly.*

### 1.3 Frames

After the preface, each direction is a sequence of **frames**. A frame is a 16-octet header followed by a payload.

| Offset | Size | Field | Encoding |
|---|---|---|---|
| 0 | 4 | `length` | `u32` little-endian: the payload's length in octets (the header is not counted) |
| 4 | 1 | `type` | message type (§3.2) |
| 5 | 1 | `flags` | bit 0 = `MORE`; bits 1–7 MUST be 0 |
| 6 | 2 | `reserved` | MUST be `00 00` |
| 8 | 8 | `stream` | `u64` little-endian: the request id the frame belongs to; 0 is the connection itself |
| 16 | `length` | payload | |

1. Every multi-octet fixed-width integer in this protocol is little-endian.
2. A receiver MUST refuse a frame with a non-zero reserved field or flag bit 1–7, an unknown `type`, or a `type`
   whose direction (§3.2) is wrong, with `W1001`, and close the connection.
3. `length` MUST NOT exceed the receiver's `max_frame_size` (§2.3); a larger frame is `W1003` and closes the
   connection. Until the handshake has set it, `max_frame_size` is 65,536 in both directions.
4. The header is fixed-width so that a server can parse it from a ring buffer without a varint loop, and so that a
   payload starts 8-octet-aligned whenever its frame does (§6.4).

### 1.4 Messages and fragmentation

1. A **message** is one frame with `MORE = 0`, or a sequence of frames with the same `stream` and the same `type`
   in which every frame but the last has `MORE = 1`. The message's payload is the concatenation of the frames'
   payloads. A frame with `MORE = 1` MUST have a non-empty payload (`W1007`).
2. Frames of messages on different streams MAY interleave. On one stream, a sender MUST NOT start a new message
   before the last frame of the previous one; a frame whose `type` differs from the open message's on the same
   stream is `W1007`.
3. A reassembled message MUST NOT exceed the receiver's `max_message_size` (§2.3); a larger one is `W1003`.
4. Every message type's payload is decoded only after reassembly. Its length MUST be exactly what its grammar
   consumes: missing octets or trailing octets are `W1001`.
   *Rationale: fragmentation lets a large parameter upload or a large result row cross without one huge frame
   blocking every other stream on the connection.*

### 1.5 Streams and request ids

1. The client opens a **stream** by sending a request message (§3.2) whose `stream` is a new request id. Request
   ids MUST be non-zero and strictly greater than every request id the client has used before on this connection
   (`W1006`, fatal). Ids need not be consecutive.
   *Rationale: increasing 64-bit ids never wrap and are never reused, so a late `Cancel` cannot hit a newer request.*
2. The server's responses to a request carry that request's id. Each request receives **exactly one terminal
   message** (§3.3), after which its stream is closed.
3. `Cancel` and `Credit` (§7) carry the id of the request they act on and do not open a stream; they are exempt
   from rule 1. One that names an id that is closed or was never opened MUST be ignored.
4. Stream 0 carries the handshake, `Ping`/`Pong`, `Terminate`, `Goodbye`, and connection-fatal `Error`s.

### 1.6 Encoding primitives

Payloads use [QT] §12.1's primitives with their canonical rules (`uvar`, `svar`, `bool`, `str`, `list<X>`,
`opt<X>`, `enum`, `node`), plus:

| Primitive | Encoding |
|---|---|
| `u8`, `u16`, `u32`, `u64` | fixed width, little-endian |
| `bin` | `uvar` length, then that many octets (not required to be UTF-8) |
| `digest` | 32 octets |
| `(A, B)` | `A` then `B` |

A non-canonical primitive in a control message (a non-minimal `uvar`, a `bool` or `opt` tag other than 0/1) is
`W1001`. Inside an embedded tree envelope or parameter block, the [QT] codes apply instead (`Q1001`, `Q1003`).

## 2. Handshake, version negotiation and authentication

### 2.1 Sequence

```
client                                        server
  [TLS 1.3, ALPN "terndb"]                     (TCP only)
  preface
  ClientHello            (stream 0)  ─────▶
                                     ◀─────   ServerHello      (stream 0)
  AuthResponse           (stream 0)  ─────▶                    (zero or more rounds,
                                     ◀─────   AuthChallenge     method-dependent)
                                     ◀─────   Ready            (stream 0)
  requests …
```

1. The client MUST send `ClientHello` first and MUST NOT send any request (§3.2) before it has received `Ready`
   (`W1004`, fatal).
2. The server MUST answer `ClientHello` with `ServerHello` or with a fatal `Error` on stream 0. After an `Error` on
   stream 0 the server MUST close the connection.
3. The server MUST NOT send `Ready` until authentication has succeeded, and MUST close the connection after a
   failed one with `W3001`, whatever the reason (wrong password, unknown user, method not permitted for the user).
   *Rationale: one code reveals nothing about which part was wrong.*
4. A server SHOULD close a connection that has not reached `Ready` within 10 seconds.

### 2.2 Version negotiation

1. `ClientHello` offers two lists of `(major, max_minor)`: the **wire protocol** versions and the **tree schema**
   versions ([QT] §13) the client supports. For each list the server picks the highest major present in both, and
   for it `min(client max_minor, server max_minor)`. `ServerHello` reports both choices.
2. No common wire major is `W1005`; no common tree major is `Q1005` ([QT] §13.4). Either is fatal.
3. The wire version and the tree version are independent: a tree-schema minor (new node kinds) needs no wire
   change, and a wire minor needs no tree change.
4. A wire **minor** version MAY add message types, flags, enum values, error codes and fields. An added field MUST
   be appended to the end of its message's payload. A sender MUST NOT emit a message type, field or value beyond
   the negotiated minor; a receiver therefore never sees one, and the exact-length rule (§1.4.4) stays strict.
5. A wire **major** version MAY change anything after the preface. The preface, the frame header layout, and the
   `ClientHello`/`ServerHello` layouts up to and including their version fields MUST NOT change in any version.
   *Rationale: those are what make negotiation itself possible.*

### 2.3 Limits

1. `ClientHello.max_frame_size` is the largest frame the client will receive; `ServerHello.max_frame_size` the
   largest the server will receive. Each MUST be at least 65,536 and at most 2³¹ − 1 (`W1001`). Each side MUST
   send only frames within the other's limit. The server SHOULD offer at least 1 MiB.
2. `Ready.max_message_size` is the largest reassembled message the server will receive. It MUST be at least
   16 MiB. The client's limit for messages it receives is unbounded by this protocol; a client MAY close a
   connection that sends a larger message than it can hold.
3. `Ready.max_in_flight` is the number of requests a client may have open at once on the connection; a request
   beyond it is refused with `W2008` (retryable, not fatal). It MUST be at least 16.
4. `Ready.max_open_txns` is the number of explicit transactions a client may have open at once on the connection
   (`W2016` beyond it). It MUST be at least 1.

### 2.4 Authentication methods

| Method name | Transport | Exchange |
|---|---|---|
| `scram-sha-256-plus` | TLS | SCRAM-SHA-256 with channel binding `tls-exporter` (RFC 5802, RFC 7677, RFC 9266) |
| `scram-sha-256` | any | SCRAM-SHA-256 without channel binding (`n,,` GS2 header) |
| `token` | TLS or Unix | one message: the token's octets |
| `peer` | Unix only | none: the server reads the peer's OS user from the socket |

1. A server MUST implement all four. A client MUST implement `scram-sha-256-plus` and `scram-sha-256`, and SHOULD
   implement `token`.
2. **No method sends a reusable password.** There is no cleartext-password and no MD5 method.
   *Rationale: SCRAM is what Postgres deployments already provision; the server stores only salted verifiers;
   `-plus` binds the exchange to the TLS session, defeating a proxy that terminates TLS. A token covers services
   and short-lived credentials without a password at all. `peer` gives a local administrator a login with no
   secret to manage.*
3. A client MUST NOT send a `token` over plaintext TCP, and MUST refuse a server that chooses `scram-sha-256`
   (without `-plus`) on a TLS connection where the client offered `-plus`, unless the client is configured to
   allow it.
   *Rationale: the downgrade from `-plus` is the attack channel binding exists to stop.*
4. The server chooses the **first** method in the client's list that it permits for this user, database and
   transport. None permitted is `W3002` (fatal; it is not `W3001`, because no credential was checked).
5. **SCRAM exchange.** `ClientHello.initial` carries the SCRAM client-first-message when the client's first method
   is a SCRAM method. `ServerHello.auth_data` carries the server-first-message. `AuthResponse` carries the
   client-final-message. `Ready.auth_final` carries the server-final-message, which the client MUST verify before
   sending any request; a failed verification MUST close the connection. If the server chose a method other than
   the client's first, it MUST ignore `initial`, and the client sends its first message for the chosen method in
   an `AuthResponse`.
6. **Token.** The token's octets are `ClientHello.initial` (or the first `AuthResponse`). Its format is opaque to
   this protocol: the server validates it against its own configured issuer (Open question 5).
7. **Peer.** The server sends `ServerHello` with `auth_method = "peer"` and then `Ready` without waiting.
8. `AuthChallenge` exists for methods with more than one server round; neither SCRAM nor token uses it in 1.0.
9. TLS client certificates are not an authentication method in 1.0 (Open question 4).

## 3. Message catalogue

### 3.1 Conventions

Each message is given as a grammar over §1.6's primitives, fields in order. An `enum` lists its values from
ordinal 0. Option records such as `TxnOptions` are encoded inline, field after field.

### 3.2 Types

The high bit of `type` is the direction: `0x00–0x7F` client to server, `0x80–0xFF` server to client.

| Type | Name | Stream | Kind |
|---|---|---|---|
| `0x01` | `ClientHello` | 0 | handshake |
| `0x02` | `AuthResponse` | 0 | handshake |
| `0x03` | `Terminate` | 0 | control |
| `0x04` | `Ping` | 0 | control |
| `0x10` | `Prepare` | new | request |
| `0x11` | `Execute` | new | request |
| `0x12` | `ExecutePrepared` | new | request |
| `0x13` | `ClosePrepared` | new | request |
| `0x20` | `Begin` | new | request |
| `0x21` | `Commit` | new | request |
| `0x22` | `Rollback` | new | request |
| `0x23` | `Savepoint` | new | request |
| `0x24` | `RollbackToSavepoint` | new | request |
| `0x25` | `ReleaseSavepoint` | new | request |
| `0x26` | `TransactionStatus` | new | request |
| `0x30` | `Cancel` | target | control |
| `0x31` | `Credit` | target | control |
| `0x81` | `ServerHello` | 0 | handshake |
| `0x82` | `AuthChallenge` | 0 | handshake |
| `0x83` | `Ready` | 0 | handshake |
| `0x84` | `Pong` | 0 | control |
| `0x85` | `Goodbye` | 0 | control |
| `0x90` | `Prepared` | request | terminal |
| `0x91` | `ResultHeader` | request | partial |
| `0x92` | `Batch` | request | partial |
| `0x93` | `Complete` | request | terminal |
| `0x94` | `Ok` | request | terminal |
| `0xA0` | `TxnBegun` | request | terminal |
| `0xA1` | `TxnCommitted` | request | terminal |
| `0xA2` | `TxnStatus` | request | terminal |
| `0xC0` | `Error` | request or 0 | terminal |

Types `0x40–0x4F` and `0xD0–0xDF` are reserved for notifications (Open question 9); `0x60–0x6F` and
`0xE0–0xEF` are reserved for `replication.md`. Every other unlisted type is reserved. In 1.0 all are `W1001`.

### 3.3 Responses per request

| Request | Responses, in order | Terminal |
|---|---|---|
| `Prepare` | `Prepared` | `Prepared` or `Error` |
| `Execute`, `ExecutePrepared` | `ResultHeader`, then zero or more `Batch`, then `Complete` | `Complete` or `Error` |
| `ClosePrepared`, `Rollback`, `Savepoint`, `RollbackToSavepoint`, `ReleaseSavepoint` | `Ok` | `Ok` or `Error` |
| `Begin` | `TxnBegun` | `TxnBegun` or `Error` |
| `Commit` | `TxnCommitted` | `TxnCommitted` or `Error` |
| `TransactionStatus` | `TxnStatus` | `TxnStatus` or `Error` |
| `Ping` | `Pong` (stream 0) | — |
| `Cancel`, `Credit`, `Terminate` | none | — |

1. An `Error` MAY replace any not-yet-sent response, including after a `ResultHeader` and some `Batch`es; it is
   then the terminal message. Nothing follows a terminal message on its stream.
2. Responses to different requests MAY interleave at frame granularity and MAY complete in any order, subject to
   the ordering rules of §4.6.

### 3.4 Handshake and control messages

```
ClientHello   := list<(uvar major, uvar max_minor)> wire_versions     (non-empty, majors strictly descending)
                 list<(uvar major, uvar max_minor)> tree_versions     (same rule)
                 str  user
                 str  database
                 str  application                                     (informative; MAY be empty)
                 list<str> auth_methods                               (non-empty, client's preference order)
                 opt<bin>  initial                                    (first auth message for auth_methods[0])
                 uvar max_frame_size
                 SessionDefaults

SessionDefaults := opt<Isolation> isolation
                   opt<bool> synchronous
                   opt<uvar> lock_timeout_ms
                   opt<uvar> statement_timeout_ms
                   opt<uvar> replica_wait_timeout_ms

ServerHello   := uvar wire_major, uvar wire_minor
                 uvar tree_major, uvar tree_minor
                 str  server_version                                  (informative)
                 uvar max_frame_size
                 str  auth_method
                 bin  auth_data                                       (empty when the method sends none)

AuthResponse  := bin data
AuthChallenge := bin data

Ready         := uvar connection_id
                 enum role { primary, replica }
                 uvar max_in_flight
                 uvar max_open_txns
                 uvar max_message_size
                 uvar horizon                                         ([ISO] §4.1 visible horizon h at Ready)
                 uvar catalog_version
                 bin  auth_final

Ping          := u64 opaque
Pong          := u64 opaque                                           (the Ping's value)
Terminate     := (empty)
Goodbye       := str code, str message, uvar grace_ms
Cancel        := (empty)
Credit        := uvar octets
```

1. A list in `wire_versions` or `tree_versions` that is empty or not strictly descending by major is `W1001`.
2. `user` and `database` MUST be non-empty; an unknown database is `W3004` and is reported only after
   authentication has succeeded (before it, the failure is `W3001`).
3. `SessionDefaults` gives the connection's defaults for every transaction it opens (§5.2). An absent field takes
   the server's configured default. An out-of-range value is `W2011` (fatal during the handshake).
4. `Terminate` asks the server to roll back every open transaction, cancel every in-flight request, and close.
   The server sends nothing further.
5. `Goodbye` announces that the server will close: it accepts no new request after sending it (a request that
   arrives later is refused with `W4001`), lets in-flight requests run for up to `grace_ms`, cancels the rest with
   `cancelled` (`details.reason = "shutdown"`), and closes. `code` is a `W4…` code.
6. The server MUST answer every `Ping` with a `Pong` carrying the same value, in the order the pings arrived.

### 3.5 Request messages

```
Prepare         := uvar handle                                        (0 = describe only, do not register)
                   bin  envelope                                      ([QT] §12.2, unaltered)

Execute         := uvar txn                                           (0 = an implicit transaction)
                   opt<TxnOptions> implicit                           (MUST be absent when txn ≠ 0)
                   ExecOptions
                   bin  envelope
                   bin  params                                        (§4.2)
                   opt<uvar> prepare_as                               (register this tree under a handle)

ExecutePrepared := uvar txn
                   opt<TxnOptions> implicit
                   ExecOptions
                   PlanRef
                   bin  params

PlanRef         := enum { handle, digest }, then uvar handle | digest shape

ClosePrepared   := uvar handle

ExecOptions     := opt<uvar> credit                                   (§7.2; absent = unlimited)
                   opt<uvar> batch_rows                               (most top-level rows per Batch)
                   opt<uvar> batch_bytes                              (target Batch payload size)
                   opt<uvar> statement_timeout_ms

Begin           := uvar txn, TxnOptions
Commit          := uvar txn
Rollback        := uvar txn
Savepoint       := uvar txn, str name
RollbackToSavepoint := uvar txn, str name
ReleaseSavepoint    := uvar txn, str name
TransactionStatus   := uvar txid

TxnOptions      := opt<Isolation> isolation
                   opt<bool> synchronous
                   opt<bool> read_only
                   opt<uvar> after                                    ([ISO] §3.6 R3 causal token)
                   opt<uvar> lock_timeout_ms
                   opt<uvar> replica_wait_timeout_ms

Isolation       := enum { serializable, snapshot, read_committed }
```

### 3.6 Response messages

```
Prepared      := digest shape
                 uvar catalog_version
                 opt<node Type> result_type                          (absent: the statement returns a count)

ResultHeader  := uvar txid
                 uvar snapshot
                 digest shape
                 opt<node Type> result_type                          (absent: the statement returns a count)

Batch         := §6.4

Complete      := list<uvar> counts                                    (one per parameter tuple)
                 opt<uvar> commit                                     (implicit transaction only)

Ok            := (empty)

TxnBegun      := uvar txid
                 uvar snapshot
                 Isolation effective

TxnCommitted  := uvar txid
                 opt<uvar> commit                                     (absent when the write set was empty)

TxnStatus     := enum { committed, aborted, in_progress }
                 opt<uvar> commit                                     (present iff committed)

Error         := §8.1
```

## 4. Requests

### 4.1 The tree envelope

1. `envelope` is the canonical encoding of a [QT] §12.2 envelope, byte for byte. The server MUST decode and
   validate it exactly as [QT] specifies, reporting [QT] codes (§8). The protocol MUST NOT alter, re-frame or
   compress it.
2. The envelope's version MUST have the negotiated tree major and a minor no greater than the negotiated tree
   minor (`Q1005`, [QT] §13.5).

### 4.2 Parameter blocks

```
params := uvar tuple_count                    (≥ 1)
          tuple_count × ( for each ParamDecl in declaration order: the value, encoded per [QT] §12.6 )
```

1. Parameter values use [QT] §12.6 exactly: row-major, untagged, decoded by the declared types. A relation-valued
   parameter is its `uvar` count and its elements, as there.
   *Rationale: parameters are small or are a single relation-valued parameter; one row encoding, already
   specified, is enough. Columnar bulk input is Open question 3.*
2. A statement with no parameters still sends `tuple_count = 1` and no values. `tuple_count = 0` is `Q4004`.
3. Values that do not decode, or octets left over in `params`, are `Q1001`; a non-canonical value is `Q1003`;
   invalid UTF-8 in a `text` value is `Q1004`; null in a non-nullable position is `Q4005`. The error's `path` is
   `/params/<tuple>/<index>` (0-based), followed by `/<field or element index>` steps into a composite value.
4. `tuple_count > 1` is a **parameter batch** ([QT] §10.3): the statement runs once per tuple, in order, within one
   transaction, atomically. The results for each tuple are kept apart (§6.4 `tuple`) and `Complete.counts` has
   one entry per tuple.

### 4.3 Execute

1. `Execute` validates the envelope, binds the parameters and runs the statement. Its first response, if
   validation and binding succeed, MUST be a `ResultHeader`; then the result `Batch`es (§6); then `Complete`.
2. `ResultHeader.txid` is the id of the transaction the statement runs in (explicit or implicit), and
   `ResultHeader.snapshot` the snapshot it reads ([ISO] §4.1; at read committed, the statement's own snapshot).
   The server MUST send `ResultHeader` before it begins to commit an implicit transaction.
   *Rationale: a client that loses the connection during an implicit commit already holds the txid it needs for
   `TransactionStatus` (§5.6).*
3. `ResultHeader.result_type` is the statement's result type ([QT] §5.3), which is always a relation type for a
   `Query` or a DML statement with `returning`. It is absent for a DML statement without `returning`, whose
   per-tuple affected-row count is reported in `Complete.counts` and which sends no `Batch`.
4. `Complete.counts[i]` is the number of top-level rows returned for tuple `i`, or for a DML statement without
   `returning` the number of rows it affected.
5. `ResultHeader.shape` is the SHA-256 digest of the statement's shape form ([QT] §10.2). It MUST equal the digest
   `EXPLAIN` reports for the same statement.
6. `prepare_as`, when present and non-zero, registers the statement under that handle exactly as `Prepare` would
   (§4.4), at the moment validation succeeds. A handle already in use is `W2006`, reported before execution.

### 4.4 Prepare and prepared execution

1. `Prepare` validates the envelope without executing it and answers `Prepared` with the shape digest, the catalog
   version it was validated against, and the result type. This is the describe step of [QT] §5.3.
2. `handle` is **chosen by the client**, connection-scoped. A non-zero handle registers the statement until
   `ClosePrepared` or the end of the connection. A handle already registered is `W2006`. `handle = 0` registers
   nothing.
   *Rationale: a client-chosen handle can be used in the very next pipelined request; a server-assigned one would
   cost a round trip before the first execution.*
3. `ClosePrepared` releases a handle and always answers `Ok`, even for an unknown handle (it is idempotent).
4. `ExecutePrepared` with `PlanRef.handle` runs the registered statement with new parameters. An unknown handle is
   `W2005`. Before running, the server MUST re-validate the statement if the catalog version has changed since it
   was registered: a validation failure is reported with its [QT] code; a result type different from the one in
   `Prepared` is `W2007`, and the handle is released. The server MAY re-plan at any time.
5. `ExecutePrepared` with `PlanRef.digest` runs the statement whose shape digest is given, **from the server-wide
   plan cache**, on any connection. The server MUST either run it, exactly as if the client had sent a tree of that
   shape, or refuse with `W2004` (`unknown_plan`, retryable) and run nothing. A client receiving `W2004` resends
   the full tree with `Execute`.
   *Rationale: a client library keeps `digest → tree` and sends 32 octets in the common case; the server is free
   to evict, because the fallback is defined.*
6. A server MUST NOT run a digest against a shape form whose catalog version differs from its current one without
   re-validating it as in rule 4 (with `W2004` in place of `W2007` when the type would change).
7. The parameter block of an `ExecutePrepared` is decoded by the parameter types of the registered or cached
   statement.

### 4.5 Implicit transactions

1. `txn = 0` runs the statement in its own **implicit transaction**, which commits when the statement succeeds and
   rolls back when it fails. `implicit` gives its options; absent fields take the connection defaults.
2. Results of an implicit transaction are **provisional until `Complete`**: if the commit fails, an `Error`
   follows the `Batch`es already sent and the statement had no effect. A client MUST NOT treat any row as
   committed before `Complete`.
3. `Complete.commit` is present iff the implicit transaction wrote and committed; it is the commit sequence number
   ([ISO] §3.6 R3).
4. `implicit` with `txn ≠ 0` is `W2011`.

### 4.6 Pipelining, multiplexing and ordering

1. A client MAY send any number of requests (up to `max_in_flight`) without waiting for responses.
2. **Requests on one explicit transaction** (`txn` equal, `≠ 0`) MUST be executed one at a time, in the order the
   server received them; a request on a transaction starts after the previous request on it has sent its terminal
   message. `Begin`, `Commit`, `Rollback` and the savepoint requests are requests on their transaction.
3. **Requests in implicit transactions, and requests on different transactions, are independent**: the server MAY
   run them concurrently and they MAY complete in any order. Each is atomic and isolated as [ISO] specifies; one
   pipelined behind another is not guaranteed to see its effects unless it starts after the other's terminal
   message ([ISO] §3.5).
4. A request naming a prepared handle MUST be processed after every earlier request on the connection that
   registers that handle (`Prepare`, `Execute.prepare_as`) has completed. A request naming a transaction handle
   MUST likewise follow the `Begin` that opened it.
5. `TransactionStatus`, `ClosePrepared`, `Ping` and the control messages are independent of every transaction.
6. *Informative.* A client that waits for one request's result before granting credit (§7.2) to an earlier
   request on the same transaction deadlocks itself; client libraries should grant credit independently of
   consumption order or use `credit` absent.

## 5. Transactions

### 5.1 Handles

1. An explicit transaction is named by a **transaction handle** `txn`, a non-zero `uvar` **chosen by the client**,
   connection-scoped. `Begin` opens it; `Commit` or `Rollback` closes it, after which the handle may be used again
   by a later `Begin`.
   *Rationale: as with prepared handles, `Begin, Execute, Execute, Commit` can be sent as one pipelined flight.*
2. `Begin` on a handle that is open is `W2002`. Any other request on a handle that is not open is `W2001`.
3. A transaction belongs to its connection. When the connection ends, every open transaction MUST be rolled back.
   A connection may hold up to `max_open_txns` open transactions at once, running independently (§4.6).

### 5.2 Begin

1. `Begin` takes a transaction id and a snapshot as [ISO] §4.1 specifies and answers `TxnBegun` with `txid`, the
   `snapshot` ([ISO] §3.6 R3: "every begin returns `snapshot`"; at read committed, the horizon at begin), and the
   `effective` isolation level.
2. Each `TxnOptions` field absent takes the connection's `SessionDefaults` value, and failing that the server's
   default. The defaults are `serializable` and `synchronous = true` ([ISO] §1, §6).
3. On a replica every transaction is read-only and serializable: `effective` MUST be `serializable` whatever was
   asked ([ISO] §3.6), and `read_only = false` explicitly requested is `read_only_transaction`.
4. `after = n`: the server MUST NOT take the snapshot until its horizon is at least `n`, and MUST fail the `Begin`
   with `replica_lag_timeout` after `replica_wait_timeout_ms` ([ISO] §3.6 R3). On a primary the horizon already
   includes every acknowledged commit, so `after` never waits there for a value the client obtained from that
   primary.
5. `synchronous` selects [ISO] §6's `on` (`true`) or `off` (`false`) for this transaction.
6. `read_only = true` declares the transaction `READ ONLY` ([ISO] §5.5): a write in it is `read_only_transaction`.
7. The same rules apply to an implicit transaction's `TxnOptions`.

### 5.3 Errors inside a transaction

1. Any error in a request on an open transaction — a validation error, a runtime error, an abort — aborts the
   transaction ([ISO] §5.1) unless it occurred after a savepoint that the client then rolls back to. The `Error`
   reports `txn_state = aborted` (§8.1).
2. Every later request on an aborted transaction other than `Rollback`, `RollbackToSavepoint` and `Commit` MUST fail
   with `W2003` (`transaction_aborted`) without running.
3. `Commit` of an aborted transaction MUST fail with an `Error` whose first entry carries the code of the error
   that aborted it, and MUST close the handle (`txn_state = closed`).
   *Rationale: a pipelined `Begin … Commit` then gets one definite answer from the `Commit`, naming the real
   cause.*

### 5.4 Commit and Rollback

1. `Commit` answers `TxnCommitted` only after the commit is published and, at `synchronous = true`, durable
   ([ISO] §4.1, §6). `commit` is the commit sequence number; it is absent for a transaction whose write set is
   empty, which is serialized at its snapshot ([ISO] §5.5).
2. A `Commit` whose outcome the server cannot determine fails with `commit_unknown` (not retryable) and closes the
   handle; the client resolves it as in §5.6.
3. `Rollback` rolls the transaction back and answers `Ok`. It **pre-empts**: if a request on the transaction is
   running, the server MUST cancel it (it ends with `cancelled`), and every request queued on the transaction ahead
   of the `Rollback` MUST fail with `cancelled` without running. `Rollback` of an unknown handle is `W2001`.
4. `Commit` cannot be cancelled: a `Cancel` naming a `Commit` MUST be ignored once the server has received it.
   *Rationale: a cancellable commit would add a second unknown-outcome path.*

### 5.5 Savepoints

1. `Savepoint` sets a savepoint named `name` in the transaction; a name already set replaces the older one with the
   same name for the purposes of rules 2 and 3 (the older one becomes unreachable by name).
2. `RollbackToSavepoint` undoes everything after the named savepoint, keeps the savepoint, and clears the aborted
   state if the error that caused it came after the savepoint. An unknown name is `W2012`.
3. `ReleaseSavepoint` forgets the named savepoint and every one set after it, keeping their effects. An unknown
   name is `W2012`.
4. Savepoints are valid on an aborted transaction only through `RollbackToSavepoint`.

### 5.6 Unknown outcomes

1. A client whose connection was lost after it sent a `Commit` (or an `Execute` in an implicit transaction that
   writes), or that received `commit_unknown`, MUST NOT retry a non-idempotent transaction until it has resolved
   the outcome ([ISO] §5.4).
2. It resolves it on any connection to the same primary with `TransactionStatus(txid)`, using the `txid` from
   `TxnBegun` or `ResultHeader`. The server answers `committed` with the commit number, `aborted`, or
   `in_progress` ([ISO] §5.4). A txid older than `status_horizon` is `W2010`.
3. `in_progress` means the client should ask again; a server MUST NOT answer `in_progress` for a transaction whose
   connection has ended and whose commit record is not in the WAL.

## 6. The result format

This section is **the one nested result encoding**. The wire carries it in `ResultHeader` and `Batch`; the C ABI
hands the same octets to its caller (§9). One decoder serves both.

### 6.1 Overview

1. A result is **self-describing**: `ResultHeader.result_type` is the result's [QT] type, encoded as a [QT] §12.3
   `Type` node. It is always a relation type `bag<E>` or `list<E>`, where `E` is the **element type**.
2. The rows follow in zero or more **batches**. A batch holds a run of consecutive top-level elements, laid out
   **columnar**: one **column** per position in the type tree, each a set of **buffers**. The layout of every batch
   is fixed by the type; only lengths and contents vary.
3. Nested relations are offsets into a child column, unions are a discriminant and an offset into a child per
   variant, and nullability is a validity bitmap — in each case Arrow's layout (§6.8).
4. The result type MUST be **structural**: a server MUST NOT send a named catalog type (kinds 21–63 of [QT] §12.3);
   it sends the structure that name denotes.

### 6.2 The column tree

The column tree of a type is built by `Col(T)`, which yields one **column node**. Each node has a **node kind**, a
**nullable** flag, an ordered list of **buffers** and an ordered list of **child nodes**.

1. If `T = U?`, `Col(T)` is `Col(U)` with nullable set. Otherwise nullable is clear. ([QT] §2.2: `U??` does not
   exist, so a node is nullable at most once.)
2. For non-nullable `U`:

| `U` | Node kind | Buffers (after validity) | Children |
|---|---|---|---|
| `bool` | fixed, width 1 | `values` | — |
| `int16`, `int32`, `int64`, `int128` | fixed, width 2, 4, 8, 16 | `values` | — |
| `float32`, `float64` | fixed, width 4, 8 | `values` | — |
| `decimal(p, s)` | fixed, width 16 | `values` | — |
| `date` | fixed, width 4 | `values` | — |
| `time`, `datetime`, `timestamp` | fixed, width 8 | `values` | — |
| `interval`, `uuid` | fixed, width 16 | `values` | — |
| `text`, `bytes` | varbin | `offsets`, `data` | — |
| `{n₁: T₁, …, nₖ: Tₖ}` | record | — | `Col(T₁)`, …, `Col(Tₖ)` |
| `union<t₀: R₀, …, tₖ₋₁: Rₖ₋₁>` | union | `discriminants`, `offsets` | `Col(R₀)`, …, `Col(Rₖ₋₁)` |
| `bag<E>`, `list<E>` | relation | `offsets` | `Col(E)` |

3. A nullable node has one extra buffer, `validity`, **first** in its list. A non-nullable node has none.
4. The **result's column tree** is `Col(E)` for the element type `E`. Its nodes are numbered in **pre-order**
   (node, then its children in order), and its buffers are numbered in the order of a pre-order walk, each node's
   buffers in the order of the table. Both numberings are what the batch uses.
5. A union discriminant has width 1 octet when `k ≤ 256`, else 2 octets. A union with `k > 65,536` cannot be
   encoded: a statement whose result type contains one is refused with `W2017` at validation.

### 6.3 Slots and column contents

A column node with `n` **slots** holds `n` values of its type. In a batch with `N` top-level rows the root has `N`
slots; a record's children have as many slots as the record; a relation's child has `offsets[n]` slots; a union's
child `j` has as many slots as there are non-null slots with discriminant `j`.

1. **validity**: `⌈n/8⌉` octets; slot `i` is valid iff bit `i mod 8` (least significant first) of octet `⌊i/8⌋` is 1.
   Unused bits of the last octet MUST be 0. **When the node's `null_count` is 0 the buffer MUST be empty** (byte
   length 0) and every slot is valid; otherwise it MUST be present. A non-nullable node's `null_count` MUST be 0.
2. **values** (fixed): `n × width` octets; slot `i` occupies octets `[i·width, (i+1)·width)` and holds the value
   encoded exactly as [QT] §12.6 encodes that scalar (`bool` one octet `00`/`01`; integers, `decimal`'s unscaled
   `int128`, floats, `date`, the 64-bit time types and `interval`'s three fields little-endian; `uuid` in RFC 9562
   order).
3. **offsets** (varbin): `(n + 1) × 4` octets of `u32`, with `offsets[0] = 0`, non-decreasing; slot `i` is
   `data[offsets[i] .. offsets[i+1])`. **data** is `offsets[n]` octets. `text` data MUST be valid UTF-8 per slot.
   The buffer is `(n + 1) × 4` octets even when `n = 0` (one zero offset).
4. **record**: no buffers of its own beyond validity. Child `j`'s slot `i` is field `j` of slot `i`.
5. **relation** offsets: `(n + 1) × 4` octets of `u32`, `offsets[0] = 0`, non-decreasing. Slot `i`'s elements are
   the child's slots `offsets[i] .. offsets[i+1]`. For `list<E>` they are in list order; for `bag<E>` their order
   is unspecified but each element appears exactly once.
6. **union**: `discriminants` is `n × w` octets (`w` from §6.2.5, little-endian), slot `i`'s variant ordinal
   ([QT] §2.4.2). `offsets` is `n × 4` octets of `u32`: slot `i`'s position in child `discriminants[i]`. The
   offsets are **canonical**: `offsets[i]` MUST equal the number of non-null slots `j < i` with
   `discriminants[j] = discriminants[i]`. A discriminant MUST be less than `k`.
7. **The top-level order**: for a `list<E>` result, rows are in batch order and, within a batch, slot order. For a
   `bag<E>` result the order is unspecified.
8. Every offset, every `n`, and every buffer length MUST be less than 2³¹ (`W2014` at the server if a single row
   would exceed it).

### 6.4 Batch

```
Batch := uvar tuple                                (parameter tuple this batch belongs to, §4.2)
         uvar column_count                         (MUST equal the number of nodes in the column tree)
         column_count × (uvar length, uvar null_count)          per node, in pre-order
         uvar buffer_count                         (MUST equal the number of buffers in the column tree)
         buffer_count × uvar byte_length                         per buffer, in order
         zero octets to the next multiple of 8 (measured from the start of the payload)
         buffer_count × ( the buffer's octets, then zero octets to the next multiple of 8 )
```

1. `length` is the node's slot count `n`; `null_count` the number of its slots whose validity bit is 0. The root's
   `length` is the batch's row count `N`, and `N` MUST be at least 1: a result with no rows sends no batch.
2. Every `length`, `null_count` and `byte_length` MUST agree with §6.3: a decoder MUST check that each buffer's
   `byte_length` equals the length §6.3 implies, that child lengths equal what the parent's offsets and
   discriminants imply, and that padding octets are zero. A server sending otherwise is a protocol violation; a
   client SHOULD close the connection with `W1008`.
3. The payload length is therefore a multiple of 8, and each buffer starts at a payload offset that is a multiple
   of 8. A payload received into an 8-aligned buffer (the frame header is 16 octets) can be read in place as typed
   arrays on a little-endian host.
   *Rationale: zero-copy access to fixed-width columns from C and from avin's typed views.*
4. A top-level row is never split across batches: all of its nested content is in the batch that holds it.
5. Batches of one tuple are sent before any batch of the next tuple, and `tuple` is non-decreasing across a
   request's batches. A tuple that returns no rows sends no batch.
6. `batch_rows`, when given, MUST NOT be exceeded. `batch_bytes`, when given, SHOULD NOT be exceeded except by a
   batch of one row. A server SHOULD default to batches of at most 1 MiB of payload.

### 6.5 Placeholders under nulls

A slot that is null, or that lies under a null record slot, still has a position in every buffer that position
reaches. Its contents MUST be the **placeholder** for its type, so that the encoding is canonical:

1. a nullable node's slot under a null ancestor is itself null (validity bit 0, counted in its `null_count`);
2. a fixed slot: all-zero octets;
3. a varbin slot: empty (`offsets[i+1] = offsets[i]`);
4. a relation slot: empty (`offsets[i+1] = offsets[i]`), so it occupies no child slot;
5. a record slot: each child slot holds its own placeholder;
6. a non-nullable union slot under a null record: discriminant 0, occupying one slot of child 0 that holds child
   0's placeholder;
7. a null union slot: discriminant 0, offset per §6.3.6, occupying **no** child slot.

### 6.6 Worked example: users → posts → comments, with a union column

The result of [QT] §16.1, with one column added to each user: `status: union<active: {}, banned: {reason: text}>`.
Its type, with `E` the user record:

```
list<{id: int64, name: text, post_count: int64,
      status: union<active: {}, banned: {reason: text}>,
      posts: list<{id: int64, title: text, comments: bag<{id: int64, body: text}>}>}>
```

The data, two users in one batch (parameter tuple 0):

```
{id: 1, name: "ann", post_count: 2, status: active{},
 posts: [{id: 10, title: "hi", comments: [{id: 100, body: "ok"}]},
         {id: 11, title: "yo", comments: []}]}
{id: 2, name: "bo",  post_count: 0, status: banned{reason: "spam"}, posts: []}
```

**The type** (`ResultHeader.result_type`, 119 octets, [QT] §12.3; every length here fits one `uvar` octet):

```
13 75                                  list, payload 117
  10 73 05                             record, payload 115, 5 fields
    02 69 64                 03 00     "id"          int64
    04 6E 61 6D 65           08 00     "name"        text
    0A 70 6F 73 74 5F 63 6F 75 6E 74
                             03 00     "post_count"  int64
    06 73 74 61 74 75 73               "status"
      11 1E 02                         union, payload 30, 2 variants
        06 61 63 74 69 76 65 10 01 00                      "active" {}
        06 62 61 6E 6E 65 64 10 0A 01 06 72 65 61 73 6F 6E 08 00
                                                           "banned" {reason: text}
    05 70 6F 73 74 73                  "posts"
      13 2A                            list, payload 42
        10 28 03                       record, payload 40, 3 fields
          02 69 64           03 00     "id"          int64
          05 74 69 74 6C 65  08 00     "title"       text
          08 63 6F 6D 6D 65 6E 74 73   "comments"
            12 0F                      bag, payload 15
              10 0D 02 02 69 64 03 00 04 62 6F 64 79 08 00
                                       record {id: int64, body: text}
```

**The column tree**, in pre-order, with each node's slot count in this batch:

| Node | Path | Kind | n | Buffers |
|---|---|---|---|---|
| 0 | (user) | record | 2 | — |
| 1 | `id` | fixed 8 | 2 | b0 values |
| 2 | `name` | varbin | 2 | b1 offsets, b2 data |
| 3 | `post_count` | fixed 8 | 2 | b3 values |
| 4 | `status` | union, w = 1 | 2 | b4 discriminants, b5 offsets |
| 5 | `status.active` | record `{}` | 1 | — |
| 6 | `status.banned` | record | 1 | — |
| 7 | `status.banned.reason` | varbin | 1 | b6 offsets, b7 data |
| 8 | `posts` | relation | 2 | b8 offsets |
| 9 | `posts[]` | record | 2 | — |
| 10 | `posts[].id` | fixed 8 | 2 | b9 values |
| 11 | `posts[].title` | varbin | 2 | b10 offsets, b11 data |
| 12 | `posts[].comments` | relation | 2 | b12 offsets |
| 13 | `posts[].comments[]` | record | 1 | — |
| 14 | `posts[].comments[].id` | fixed 8 | 1 | b13 values |
| 15 | `posts[].comments[].body` | varbin | 1 | b14 offsets, b15 data |

Nothing is nullable, so there are no validity buffers and every `null_count` is 0.

**The batch**: frame header, then a 240-octet payload. The request id is 7.

```
frame header   F0 00 00 00  92  00  00 00  07 00 00 00 00 00 00 00
               length 240   Batch flags reserved  stream 7

payload
0x00  00                                   tuple 0
0x01  10                                   column_count 16
0x02  02 00  02 00  02 00  02 00  02 00    nodes 0–4:   length, null_count
0x0C  01 00  01 00  01 00                  nodes 5–7
0x12  02 00  02 00  02 00  02 00  02 00    nodes 8–12
0x1C  01 00  01 00  01 00                  nodes 13–15
0x22  10                                   buffer_count 16
0x23  10 0C 05 10 02 08 08 04 0C 10 0C 04 0C 08 08 02
                                           byte lengths of b0 … b15
0x33  00 00 00 00 00                       pad to 0x38
0x38  01 00 00 00 00 00 00 00 02 00 00 00 00 00 00 00      b0  id: 1, 2
0x48  00 00 00 00 03 00 00 00 05 00 00 00  00 00 00 00     b1  name offsets 0, 3, 5   + pad
0x58  61 6E 6E 62 6F  00 00 00                             b2  "annbo"                + pad
0x60  02 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00      b3  post_count: 2, 0
0x70  00 01  00 00 00 00 00 00                             b4  status: active, banned + pad
0x78  00 00 00 00 00 00 00 00                              b5  status offsets 0, 0
0x80  00 00 00 00 04 00 00 00                              b6  reason offsets 0, 4
0x88  73 70 61 6D  00 00 00 00                             b7  "spam"                 + pad
0x90  00 00 00 00 02 00 00 00 02 00 00 00  00 00 00 00     b8  posts offsets 0, 2, 2  + pad
0xA0  0A 00 00 00 00 00 00 00 0B 00 00 00 00 00 00 00      b9  post id: 10, 11
0xB0  00 00 00 00 02 00 00 00 04 00 00 00  00 00 00 00     b10 title offsets 0, 2, 4  + pad
0xC0  68 69 79 6F  00 00 00 00                             b11 "hiyo"                 + pad
0xC8  00 00 00 00 01 00 00 00 01 00 00 00  00 00 00 00     b12 comments offsets 0, 1, 1 + pad
0xD8  64 00 00 00 00 00 00 00                              b13 comment id: 100
0xE0  00 00 00 00 02 00 00 00                              b14 body offsets 0, 2
0xE8  6F 6B  00 00 00 00 00 00                             b15 "ok"                   + pad
0xF0  (end)
```

Reading it back: user 1's posts are `posts[0..2)` (b8), so posts 10 and 11; post 10's comments are
`comments[0..1)` (b12), comment 100; post 11's are `comments[1..1)`, empty; user 2's posts are `posts[2..2)`,
empty. User 1's status has discriminant 0 (`active`), offset 0 into node 5; user 2's has discriminant 1
(`banned`), offset 0 into node 6, whose `reason` is `data[0..4)` of b7, `"spam"`.

**The rest of the response**, for an `Execute` in an implicit read-only transaction with txid 1000 and snapshot
999:

```
ResultHeader  frame: 9C 00 00 00  91 00 00 00  07 00 00 00 00 00 00 00
              payload (156 octets): E8 07                 txid 1000
                                    E7 07                 snapshot 999
                                    <32 octets>           shape digest (SHA-256 of the shape form)
                                    01 <119 octets above> result_type present
Complete      frame: 03 00 00 00  93 00 00 00  07 00 00 00 00 00 00 00
              payload: 01 02  00                          counts [2], commit absent
```

### 6.7 Worked example: a nullable column

Result type `bag<{x: int32?}>` with rows `{x: 7}`, `{x: null}`, `{x: 9}`. Node 0 is the record (3 slots, null
count 0); node 1 is `x` (nullable, 3 slots, null count 1) with buffers b0 validity and b1 values. Payload, 40
octets:

```
0x00  00  02  03 00  03 01  02  01 0C        tuple 0; 2 nodes; (3,0) (3,1); 2 buffers; lengths 1, 12
0x09  00 00 00 00 00 00 00                   pad to 0x10
0x10  05  00 00 00 00 00 00 00               b0 validity 0b101 (slots 0 and 2 valid) + pad
0x18  07 00 00 00 00 00 00 00 09 00 00 00  00 00 00 00
                                             b1 values 7, placeholder 0, 9 + pad
0x28  (end)
```

The same rows with no null would send `(3,0)` for node 1 and a validity byte length of 0.

### 6.8 Relation to Apache Arrow

The encoding is **Arrow-shaped but not Arrow IPC**.

1. *Informative.* Each node maps to an Arrow array: fixed ↔ primitive, varbin ↔ `Utf8`/`Binary` (32-bit offsets),
   record ↔ `Struct`, relation ↔ `List` (32-bit offsets), union ↔ dense `Union`. Validity bitmaps, offset buffers
   and dense-union offsets have Arrow's bit order and meaning, so a client can expose a batch as Arrow arrays by
   pointing at the buffers, with these exceptions:
   - `bool` is one octet per value ([QT] §12.6), where Arrow packs bits: one conversion pass.
   - `interval` is (months, days, micros), Arrow's `MonthDayNano` is nanos: a multiply per value.
   - `int128` has no Arrow type; it maps to `Decimal128(38, 0)` with a range check, or to `FixedSizeBinary(16)`.
   - An Arrow union has no validity bitmap; a nullable TernDB union needs one added (Arrow models it as a null
     variant).
   - Arrow type ids are `int8` (at most 128 variants); TernDB uses `u8` and, above 256 variants, `u16`.
   - `date` ↔ `Date32`, `time` ↔ `Time64(us)`, `datetime` ↔ `Timestamp(us)` without zone, `timestamp` ↔
     `Timestamp(us, "UTC")`, `decimal(p, s)` ↔ `Decimal128(p, s)`, `uuid` ↔ `FixedSizeBinary(16)` (or the
     `arrow.uuid` extension) map without conversion.
2. *Rationale for not using Arrow IPC itself.* Arrow IPC carries its schema as Flatbuffers, so a client would need
   a Flatbuffers implementation (there is none for avin); its type system lacks TernDB's distinctions (nullability
   is in the type, `bag` versus `list`, union tags and record field order as part of identity), so the schema would
   need a side channel anyway; and its message framing duplicates this protocol's. Taking Arrow's buffer layouts
   and nothing else keeps conversion cheap and the decoder small.
3. Whether the C ABI also offers the Arrow C Data Interface is Open question 12.

## 7. Streaming, flow control and cancellation

### 7.1 Streaming

1. A server SHOULD send each batch as soon as it is produced and MUST NOT require the whole result to be
   materialised before sending the first batch, except where the plan itself requires it (a sort, an aggregate).
2. A client MAY consume batches as they arrive. For an explicit transaction the rows are as final as the
   transaction's own reads; for an implicit one they are provisional until `Complete` (§4.5).

### 7.2 Credit

1. `ExecOptions.credit` is the request's initial **credit**: the number of `Batch` payload octets (after
   reassembly, header included) the server may send before it waits. Absent, the credit is unlimited and the
   request is paced only by the transport.
2. A `Credit` message on the request's stream adds `octets` to its credit. Credit is per request; there is no
   connection-level credit.
   *Rationale: per-request credit stops one slow consumer of a large result from stalling the other streams on a
   multiplexed connection, which TCP's single window cannot do.*
3. The server MUST NOT start sending a `Batch` while the request's credit is zero or negative. It MAY start one
   whose size exceeds the remaining credit, which then goes negative by the difference.
   *Rationale: a row larger than the window can always be delivered, so no credit setting deadlocks.*
4. While a request waits for credit, the server MUST suspend producing its result (holding at most one batch
   beyond what it has sent) and MUST NOT time it out on `statement_timeout_ms` for the time spent waiting. Locks
   and the snapshot are held meanwhile.
5. `ResultHeader`, `Complete` and `Error` consume no credit. A `Credit` of 0 octets is legal and has no effect.
6. A client that has granted no more credit and wants nothing further SHOULD send `Cancel`, not leave the request
   suspended.

### 7.3 Cancellation

1. `Cancel` on a request's stream asks the server to stop it. If the request has not yet sent its terminal message,
   the server MUST stop it as soon as it can and end it with an `Error` with code `cancelled` (or with its normal
   terminal message if it finished first). The request then had no effect, unless it was a statement in an
   explicit transaction, in which case that transaction is aborted ([ISO] §5.2).
2. A `Cancel` naming a closed or never-opened stream, or a `Commit` already received (§5.4.4), MUST be ignored.
3. `statement_timeout_ms` (from `ExecOptions`, else the connection default) ends a request that runs longer with
   `cancelled`, `details.reason = "statement_timeout"`.
4. A server MUST process `Cancel` and `Credit` without waiting behind any other request on the connection.

### 7.4 Keepalive

A client SHOULD send `Ping` on an idle connection at least every 60 seconds if it expects to keep it, and a
server MAY close a connection with no traffic for a configured idle time after sending `Goodbye` with `W4002`.

## 8. Errors

### 8.1 The Error message

```
Error      := list<ErrorEntry> entries                         (non-empty; entries[0] is the primary error)
              bool retryable
              enum txn_state { unaffected, aborted, closed }
              opt<uvar> txid
              opt<uvar> tuple                                   (the parameter tuple that failed, §4.2)
ErrorEntry := str code
              str message                                       (human text; not stable)
              str path                                          (empty when there is none)
              list<(str, str)> details                          (sorted by key, keys unique)
              opt<(uvar start, uvar end)> span                  (source span from the envelope's Spans)
```

1. `code` is the stable code, written exactly as its defining spec writes it: a [QT] code (`Q2003`, `R0002`), an
   [ISO] code (`serialization_failure`, `unique_violation`) or a code of this document (`W2004`). Clients and tests
   match on `code` alone.
2. `path` is the [QT] §11.1 path into the tree (`/statement/source/rows/0/1`), or the parameter path of §4.2.3. For
   a runtime error the server SHOULD give the path of the node whose evaluation failed.
3. A validation error MUST carry, as `entries`, every error [QT] §11.2 requires: the first phase's errors sorted by
   path, the pre-order-first one first.
4. `span` is present when the envelope carried `spans` and one covers the node at `path` ([QT] §5.4).
5. `retryable` is true iff `entries[0].code` is retryable under [ISO] §5.2 (`serialization_failure`,
   `deadlock_detected`, `lock_timeout`, `schema_changed`, `replica_lag_timeout`, `replica_conflict`, `cancelled`,
   `resource_exhausted`) or is `W2004` or `W2008`. A retryable error MUST guarantee that the request had no effect,
   and for a transaction-aborting code that the transaction had no effect ([ISO] §5.2).
6. `txn_state` says what became of the request's transaction: `unaffected` (an implicit transaction rolled back,
   or a request outside any transaction), `aborted` (an explicit transaction that now accepts only `Rollback`,
   `RollbackToSavepoint` or `Commit`), `closed` (the handle is released: a failed `Commit`, a `Begin` that failed).
7. `txid` is present whenever a transaction id had been assigned; it is required for `commit_unknown`.
8. A connection-fatal error is sent on stream 0 and the connection is then closed by its sender. Every `W1…` and
   `W3…` code, `W1005` and a handshake-time `Q1005` or `W2011` is fatal; no other code is.

### 8.2 Codes of this document

| Code | Meaning | Fatal | Retryable |
|---|---|---|---|
| `W1001` | Malformed frame or message: bad reserved field or flag, unknown or wrong-direction type, payload length mismatch, non-canonical primitive | yes | no |
| `W1003` | Frame larger than `max_frame_size`, or message larger than `max_message_size` | yes | no |
| `W1004` | Message out of order for the handshake state | yes | no |
| `W1005` | No common wire protocol major version | yes | no |
| `W1006` | Request id zero, or not greater than every earlier one | yes | no |
| `W1007` | Fragmentation error: empty `MORE` frame, type change within a message | yes | no |
| `W1008` | (client-side) The server sent a batch inconsistent with §6 | yes | no |
| `W2001` | Unknown transaction handle | no | no |
| `W2002` | Transaction handle already open | no | no |
| `W2003` | Transaction is aborted; only `Rollback`, `RollbackToSavepoint`, `Commit` accepted | no | no |
| `W2004` | `unknown_plan`: shape digest not in the plan cache; resend the tree | no | yes |
| `W2005` | Unknown prepared handle | no | no |
| `W2006` | Prepared handle already registered | no | no |
| `W2007` | Prepared statement's result type changed after a catalog change; handle released | no | no |
| `W2008` | Too many in-flight requests on the connection | no | yes |
| `W2010` | `txid` older than `status_horizon` | no | no |
| `W2011` | Invalid or misplaced option (out-of-range value, `implicit` with `txn ≠ 0`) | handshake only | no |
| `W2012` | Unknown savepoint | no | no |
| `W2014` | A single row exceeds an encoding limit (§6.3.8) or `max_message_size` | no | no |
| `W2016` | Too many open transactions on the connection | no | no |
| `W2017` | Result type not encodable (a union with more than 65,536 variants) | no | no |
| `W3001` | Authentication failed | yes | no |
| `W3002` | No acceptable authentication method | yes | no |
| `W3003` | TLS required (plaintext connection from a non-loopback address) | yes | no |
| `W3004` | Unknown database | yes | no |
| `W4001` | Server shutting down (in `Goodbye`, and for a request after it) | no | yes |
| `W4002` | Idle connection closed (in `Goodbye`) | no | no |

Codes are never reused; a withdrawn code stays reserved. `W4001` is retryable on another connection.

### 8.3 Which code a failure gets

1. A failure inside the tree envelope or the parameter block is a [QT] code (`Q…`), never a `W` code.
2. A runtime error during execution is an `R…` code ([QT] §11.3) or an [ISO] §5.2 code.
3. An abort is reported with exactly one [ISO] §5.2 code as `entries[0].code` ([ISO] §5.1).
4. Everything about framing, sessions, handles and authentication is a `W` code.

## 9. The C ABI view of the same encoding

`libterndb` embeds the engine in-process, and also offers a network client of this protocol. In both modes it
presents results through the same handles, and those handles expose the **same octets** as §6: the `Type`
encoding of `ResultHeader.result_type` and each `Batch` payload. A client with its own §6 decoder (a pure-avin one,
for instance) can therefore decode a result obtained through the C ABI with no second decoder, and a client that
prefers not to decode can walk the result through accessor functions. The full API is `c-api.md`'s; this section
fixes what that API MUST offer for results.

### 9.1 Rules

1. Every object crosses the C boundary as an **opaque handle** (`terndb_result *`, `terndb_batch *`,
   `terndb_column *`, `terndb_type *`, `terndb_error *`). No sysl type, sysl string, slice or closure crosses it;
   strings and buffers cross as `(const uint8_t *, size_t)` pairs; integers as `<stdint.h>` types.
2. A result is a **cursor over batches**: the caller pulls batches one at a time, and a batch stays valid until the
   caller frees it, independently of the result and of later batches.
3. Column handles and buffer pointers obtained from a batch are borrowed from it and are valid until the batch is
   freed. They are not freed separately.
4. Every pointer to a batch payload or buffer MUST be 8-aligned (§6.4.3).
5. Accessors that read a value MUST return it in host byte order whatever the host; the raw buffer accessor returns
   the little-endian bytes of §6.
6. An embedded execution reports errors as a `terndb_error *` with the fields of §8.1, through the same accessors as
   a network one.

### 9.2 Sketch of the result functions

*Informative as to names and exact signatures, which `c-api.md` fixes; normative as to the capabilities.*

| Function | Purpose |
|---|---|
| `terndb_result_type(result) → const terndb_type *` | The result type. |
| `terndb_result_type_bytes(result, &ptr, &len)` | The [QT] §12.3 encoding of the type, as in `ResultHeader`. |
| `terndb_result_next(result, &batch, &error) → int` | Pull the next batch: 1 = a batch, 0 = end of result, −1 = error. |
| `terndb_result_counts(result, &ptr, &len)`, `terndb_result_commit(result, &seq) → bool` | `Complete`'s fields, after the end. |
| `terndb_result_txid(result)`, `terndb_result_snapshot(result)`, `terndb_result_shape(result, digest_out)` | `ResultHeader`'s fields. |
| `terndb_batch_bytes(batch, &ptr, &len)` | The `Batch` payload, octet for octet as on the wire. |
| `terndb_batch_tuple(batch)`, `terndb_batch_rows(batch)` | `tuple` and the root's `length`. |
| `terndb_batch_root(batch) → const terndb_column *` | Node 0. |
| `terndb_batch_free(batch)`, `terndb_result_free(result)` | Release. |
| `terndb_column_kind(col)`, `terndb_column_type(col)` | The node kind (fixed, varbin, record, union, relation) and its type. |
| `terndb_column_len(col)`, `terndb_column_null_count(col)` | `length` and `null_count`. |
| `terndb_column_is_null(col, i) → bool` | Validity of slot `i`. |
| `terndb_column_fixed(col, &ptr, &width)` | The `values` buffer, for direct typed access. |
| `terndb_column_get_i64(col, i)`, `…_i32`, `…_f64`, `…_bool`, … | One fixed value in host order. |
| `terndb_column_bytes(col, i, &ptr, &len)` | One varbin value. |
| `terndb_column_field_count(col)`, `terndb_column_field(col, j)`, `terndb_column_field_by_name(col, name, len)` | A record's children. |
| `terndb_column_children(col) → const terndb_column *`, `terndb_column_range(col, i, &start, &end)` | A relation's element column and slot `i`'s element range in it. |
| `terndb_column_union_tag(col, i)`, `terndb_column_union_offset(col, i)`, `terndb_column_variant(col, j)` | A union slot's discriminant, its position in the variant column, and variant `j`'s column. |
| `terndb_type_kind(t)`, `terndb_type_nullable(t)`, `terndb_type_field_count(t)`, `terndb_type_field_name(t, j, &ptr, &len)`, `terndb_type_field_type(t, j)`, `terndb_type_element(t)`, `terndb_type_decimal(t, &p, &s)` | Walking a type. |

With these, the §6.6 result is walked from C as: root → field `posts` → `terndb_column_range(posts, 0, …)` gives
`[0, 2)` → `terndb_column_children(posts)` → field `comments` → and so on, with no knowledge of buffer offsets.

## 10. Conformance

### 10.1 Properties

Each property is checked by the golden vectors of §10.2 (decoder and encoder tests, both directions), by a
protocol test harness that drives a real server, or both. **D** = decoder test, **S** = server test.

1. (S) A connection whose first plaintext octet is neither `0x16` nor `0x54`, or whose preface differs, is closed
   with no response.
2. (D, S) A frame with a non-zero reserved field, flag bit 1–7, unknown type or wrong-direction type is `W1001` and
   closes the connection.
3. (S) A frame larger than the announced `max_frame_size` is `W1003`.
4. (D, S) A message's payload with trailing octets, missing octets, or a non-canonical primitive is `W1001`.
5. (S) Version negotiation picks the highest common major and `min` of the minors, for wire and tree separately;
   no common major is `W1005` / `Q1005`.
6. (S) No request is processed before `Ready`; one sent earlier is `W1004`.
7. (S) A wrong password, an unknown user, and a method not permitted for the user all produce `W3001`, with
   identical timing within the server's configured noise bound.
8. (S) SCRAM-SHA-256-PLUS fails when the channel binding data is altered; the client refuses a server-final with a
   wrong signature.
9. (S) Request ids that are zero or not increasing are `W1006`; `Cancel`/`Credit` naming closed ids are ignored.
10. (S) Every request receives exactly one terminal message, and nothing follows it on the stream.
11. (S) Requests on one transaction run in arrival order; a pipelined `Begin, Execute, Execute, Commit` sent in one
    flight succeeds as if sent one at a time.
12. (S) After an error in a transaction, every later request except `Rollback`, `RollbackToSavepoint`, `Commit` is
    `W2003`; `Commit` then reports the original code and closes the handle.
13. (S) `Rollback` cancels a running request on its transaction and fails queued ones with `cancelled`.
14. (S) `Prepare` with `handle = 0` registers nothing; `ExecutePrepared` with that number is `W2005`.
15. (S) `ExecutePrepared` by digest either runs the statement with the same results as `Execute` of its tree, or is
    `W2004` with no effect.
16. (S) A DDL that changes a prepared statement's result type makes its next `ExecutePrepared` `W2007`.
17. (S) `ResultHeader.shape` equals the digest `EXPLAIN` reports, and two trees of the same shape report the same
    digest.
18. (S) `ResultHeader` precedes the commit of an implicit transaction: a connection killed between them leaves a
    txid that `TransactionStatus` resolves to `committed` or `aborted`.
19. (S) A `Batch` is never sent while credit is zero or negative; a single row larger than the credit is still
    delivered after one `Credit`.
20. (S) `Cancel` of a running `Execute` ends it with `cancelled` and no effect; `Cancel` of a received `Commit` is
    ignored.
21. (D) Every batch satisfies §6.3–§6.5: buffer lengths, child lengths, canonical union offsets, empty validity
    when `null_count = 0`, zero padding, zero placeholders, `N ≥ 1`.
22. (D) A decoder rejects a batch that violates any rule of property 21 (one negative vector per rule).
23. (S) A `list` result's rows arrive in list order across batches; a nested `list`'s elements in list order.
24. (S) A parameter batch of `t` tuples returns `Complete.counts` of length `t`, batches tagged in non-decreasing
    `tuple`, and on any tuple's failure an `Error` with `tuple` set and no effect of any tuple.
25. (S) Encoding a result twice gives identical octets (apart from bag order and batch boundaries, which a test
    fixes with `list` results and `batch_rows`).
26. (S) An error's `entries` follow [QT] §11.2's order and the `path` names the failing node; a parameter error's
    path is `/params/<tuple>/<index>`.
27. (S) `retryable` is set exactly for the codes of §8.1.5, and a retryable error leaves no effect.
28. (S) `after = n` on a replica waits until the horizon reaches `n` or fails with `replica_lag_timeout`; on a
    primary with `n` from that primary's own acknowledgements it never waits.
29. (S) A connection that ends with open transactions rolls them all back.
30. (D) The C ABI's `terndb_batch_bytes` and `terndb_result_type_bytes` return exactly the octets the wire carries
    for the same query, and every accessor of §9.2 agrees with a §6 decoder over those octets.

### 10.2 Golden vectors

The vectors live in `docs/spec/vectors/wire/`, one file per vector, written in the same commit as the
implementation that first produces them. A file is UTF-8 text: header lines `# name:`, `# spec:` (the section it
checks), `# direction:` (`c2s` or `s2c`), `# expect:` (`accept` with a description of the decoded value, or
`reject <code>`), then the octets as hex, whitespace ignored. A client test suite MUST decode every `accept`
vector to the stated value, re-encode it to the same octets, and refuse every `reject` vector with the stated code.

| # | Vector | Checks |
|---|---|---|
| V1 | `preface` | §1.2 |
| V2 | `frame-header` (Ping on stream 0) and `pong` | §1.3, §3.4 |
| V3 | `frame-reserved-nonzero`, `frame-flag-bit1`, `frame-unknown-type`, `frame-wrong-direction` (reject `W1001`) | §1.3 |
| V4 | `fragmented-execute` (three frames, `MORE` set on two) and `fragment-type-change` (reject `W1007`) | §1.4 |
| V5 | `client-hello-scram` and `client-hello-token`, with `SessionDefaults` all present and all absent | §3.4 |
| V6 | `server-hello`, `ready` | §3.4 |
| V7 | `hello-versions-not-descending` (reject `W1001`), `uvar-non-minimal` (reject `W1001`) | §2.2, §1.6 |
| V8 | A complete SCRAM-SHA-256 exchange with fixed nonce and salt (RFC 7677's test vector, framed) | §2.4 |
| V9 | `execute-autocommit` with the [QT] §16.1 tree and parameters `$0 = 50`, `$1 = 3` | §3.5, §4.2 |
| V10 | `execute-param-batch` with three tuples | §4.2 |
| V11 | `params-trailing-octets` (reject `Q1001`, path `/params/0`) | §4.2 |
| V12 | `prepare`, `prepared`, `execute-prepared-handle`, `execute-prepared-digest` | §4.4 |
| V13 | `begin` with every `TxnOptions` field, `txn-begun`, `commit`, `txn-committed` with and without `commit` | §5 |
| V14 | `transaction-status`, `txn-status` for each state | §5.6 |
| V15 | `result-header` and `batch` and `complete` of §6.6, exactly as printed there | §6.6 |
| V16 | `batch-nullable` of §6.7, and its no-null variant | §6.7 |
| V17 | `batch-null-record-placeholders`: `bag<{r: {a: int64, s: text, l: list<{v: int32}>, u: union<a: {}, b: {x: int16}>}?}>` with a null `r` | §6.5 |
| V18 | `batch-null-union`: `bag<{u: union<a: {x: int32}, b: {}>?}>` with null, `a`, null, `a`, `b` | §6.3.6, §6.5.7 |
| V19 | `batch-every-scalar`: one non-null and one null value of each scalar type of [QT] §2.1, incl. `int128` min, `decimal(38, 10)`, NaN, −0.0, `interval` with negative fields | §6.3.2 |
| V20 | `batch-empty-nested`: a row whose nested relations are all empty, and `{}` records | §6.3 |
| V21 | Negative batches, one per rule: wrong `byte_length`, non-zero padding, non-canonical union offset, discriminant ≥ k, validity present with `null_count = 0`, decreasing offsets, invalid UTF-8 in `text`, `N = 0` (reject `W1008`) | §6.4.2 |
| V22 | `error-validation` with two entries, a span and details; `error-commit-unknown` with txid | §8.1 |
| V23 | `cancel`, `credit` | §7 |
| V24 | `goodbye` | §3.4 |
| V25 | `union-257-variants`: a batch whose union uses 2-octet discriminants | §6.2.5 |

The C ABI conformance suite (`c-api.md`) MUST load V15–V21 through the embedded engine's own result for the same
query and data, and compare `terndb_batch_bytes` to the vector.

## 11. Open questions

These need the user's decision; the spec above takes the stated provisional position.

1. **Port.** The default TCP port is provisionally 7365 (unassigned by IANA at the time of writing). Which number,
   and should it be registered?
2. **`bool` width in results.** One octet per value, because [QT] §12.6 fixes it and one value decoder then serves
   parameters and results. Arrow packs bits. Should result columns pack `bool` (changing [QT] §12.6 for results
   only)?
3. **Columnar parameter input.** Parameters are row-major (§4.2). A relation-valued parameter of a million rows
   would be cheaper to build and decode in the §6 layout. Should a parameter of relation type be sendable as
   batches (a `ParamBatch` message), for bulk load?
4. **Client certificates.** Is mutual TLS (certificate → user mapping) needed in 1.0, or is `token` enough for
   service accounts?
5. **Token format.** Opaque tokens validated by a server-side table, or signed tokens (JWT/PASETO) validated
   against a configured issuer key, or both?
6. **Plaintext TCP.** Allowed on loopback only (as specified), or never, with local unencrypted access only over
   the Unix socket?
7. **Plan-cache scope for digests.** Server-wide (as specified). Should it be per database, per user (a digest
   could reveal that someone else ran a query of that shape via timing), or should `ExecutePrepared` by digest be
   restricted to digests this user has prepared?
8. **Compression.** No compression in 1.0. Should batches be compressible (per-buffer LZ4 or Zstandard, negotiated
   in the handshake)?
9. **Notifications.** Types are reserved for server-pushed notifications (LISTEN/NOTIFY, change feeds). Are they
   wanted, and in which version?
10. **Ordered implicit transactions.** Pipelined implicit transactions are independent (§4.6.3). Should there be an
    option that makes a connection's implicit transactions start in arrival order, giving read-your-writes within a
    pipeline without an explicit transaction?
11. **SQL text over the native protocol.** Version 1.0 accepts only trees; DDL needs SQL ([QT] §1.5). Should there
    be an `ExecuteSql` request (text in, same result format out) for DDL and tools, before `ddl.md` exists?
12. **Arrow C Data Interface.** Should `libterndb` export batches as `ArrowArray`/`ArrowSchema` (with the
    conversions of §6.8.1), so DuckDB, Polars and pandas consume them directly?
13. **Union discriminant width.** Variable (1 or 2 octets, by arity) as specified, or a fixed 2 octets for
    simplicity, or a fixed 1 octet with a 256-variant limit written into [QT] §2.4?
14. **Error-code style.** [QT] uses `Qnnnn`/`Rnnnn`, [ISO] uses names (`serialization_failure`), this document
    `Wnnnn`. The wire carries each as written. Should the three be unified before 1.0 freezes?
