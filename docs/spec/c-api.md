# TernDB C ABI (`libterndb`)

**Status:** normative, draft. **ABI version:** 1.0. **Tree schema version carried:** 1.0. **Wire protocol version
of the network mode:** 1.0.

This document defines `libterndb`'s C application binary interface: the one header `terndb.h`, what each function
does, who owns what crosses it, which threads may call what, and how the ABI evolves. It is the contract every
embedding language builds on: C and C++ directly, **avin as a first-class embedded client**, and any language with
a C foreign-function interface. Nothing in it depends on sysl; the sysl the implementation needs is listed in §14.

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as in RFC 2119. Every MUST is meant to be
checkable by the C test driver of §15. Text marked *Rationale* explains a choice and is not itself a rule. Text
marked *Informative* has no force.

References: `query-tree.md` is **[QT]**, `wire-protocol.md` **[WIRE]**, `isolation.md` **[ISO]**,
`on-disk-format.md` **[DISK]**, `ddl.md` **[DDL]**; every error code is registered in `errors.md` **[ERR]**.

## 0. Scope

1. `libterndb` has two modes behind one API. **Embedded**: the engine runs inside the caller's process, on a
   database directory (or in memory) the process opens. **Network**: the library is a client of [WIRE], connected
   to a `terndb` server. A connection handle (`terndb_conn`) is the same type in both, and every function that
   takes one behaves identically in both, except where a rule here says otherwise.
2. The ABI is the [WIRE] request model with the framing removed. Queries go in as [QT] §12.2 envelope bytes,
   parameters as [WIRE] §4.2 parameter blocks, and results come out as the [WIRE] §6 encoding, which a caller may
   either take as raw bytes or walk through accessors ([WIRE] §9). One decoder therefore serves the wire, the C ABI
   and stored test vectors.
3. There is no SQL entry point. A SQL or GraphQL front end produces trees; it is not part of this ABI.
4. DDL is submitted like any statement: an envelope whose statement is a [DDL] §3 kind, through `terndb_execute`
   ([DDL] §3.1). It declares no parameters and binds one empty tuple, so its parameter block is the two octets of
   [WIRE] §4.2 rule 3 (`01 00`: `tuple_count = 1` and an empty `columnar` list), or a NULL pointer with length
   0, which the library MUST treat as exactly that block.

## 1. Conventions across the boundary

1. **Opaque handles only.** Every object is an incomplete `struct` type reached through a pointer
   (`terndb_db *`, …). The ABI declares **no struct, union or bitfield with a visible layout**: options are objects
   with setter functions (§5), and the allocator is given as function pointers (§3). *Rationale: a struct layout is
   the part of a C ABI that breaks silently across versions and that every foreign-function interface has to
   transcribe by hand; avin, Python's ctypes and Java's FFM then need only pointer, integer and float types.*
2. **Plain C types.** Integers are `<stdint.h>` types; sizes, lengths and indices are `size_t`; flags are `bool`
   from `<stdbool.h>`; floats are `float` and `double`. Enumerated values are `int32_t` with `#define` constants,
   never a C `enum` type, whose width the C standard does not fix.
3. **Text** crosses as `(const char *, size_t)`: UTF-8, **not** NUL-terminated, length in octets. **Bytes** cross
   as `(const uint8_t *, size_t)`. A length-0 argument MAY have a NULL pointer. The only NUL-terminated strings
   are the static ones the library returns (`terndb_version`, error codes and names), which live for the life of
   the process.
4. **Fallible functions return `int`**: `0` success, `-1` failure, and for a function documented as a pull or a
   test, `1` for "yes / one more". The last parameter of every fallible function is `terndb_error **err` (§10).
5. **Out-parameters** are written only on success, except where a rule says otherwise. On failure they are left
   untouched.
6. **Inputs are copied.** Every buffer, string and options object a caller passes is copied, or fully consumed,
   before the call returns, for synchronous and asynchronous calls alike. The caller MAY free or reuse it as soon
   as the call returns.
7. **Precondition violations** — a NULL where a handle is required, a freed handle, a handle of the wrong kind,
   an index out of range, a typed getter on a column of another representation — are reported with a `C1…` code
   where the function has an error channel. Where it has none (a getter returning a value, a `_free`), a **debug
   build** MUST print the code and the function name to standard error and abort; a release build's behaviour is
   undefined. §2.4 says which checks are made in which build.
8. **Network byte order is never seen.** Value getters return host order; raw buffers are the little-endian octets
   of [WIRE] §6 ([WIRE] §9.1 rule 5).

## 2. Handles, ownership and threads

### 2.1 The handles

| Handle | Created by | Freed by | Owner of |
|---|---|---|---|
| `terndb_config` | `terndb_config_new` | `terndb_config_free` | key/value settings for open, clone, promote, session, connect |
| `terndb_db` | `terndb_open`, `terndb_clone`, `terndb_promote` | `terndb_close` | an open embedded database |
| `terndb_conn` | `terndb_session`, `terndb_connect` | `terndb_conn_close` | a session: defaults, transactions, statements |
| `terndb_txn_options`, `terndb_exec_options` | `…_new` | `…_free` | the [WIRE] `TxnOptions` and `ExecOptions` |
| `terndb_txn` | `terndb_begin` | `terndb_txn_free` | an explicit transaction |
| `terndb_stmt` | `terndb_prepare` | `terndb_stmt_free` | a prepared statement |
| `terndb_result` | `terndb_execute…` | `terndb_result_free` | one request's cursor over batches |
| `terndb_batch` | `terndb_result_next`, `terndb_batch_decode` | `terndb_batch_free` | one batch's octets |
| `terndb_type` | `terndb_type_decode` (owned); accessors (borrowed) | `terndb_type_free` (owned only) | a type tree |
| `terndb_column` | accessors only (borrowed) | never | a node of a batch's column tree |
| `terndb_error` | any fallible call | `terndb_error_free` | one [WIRE] §8.1 error |
| `terndb_queue` | `terndb_queue_new` | `terndb_queue_free` | a completion queue (§6) |
| `terndb_op` | every `…_async` call | `terndb_op_free` | one pending operation |
| `terndb_metrics` | `terndb_metrics_snapshot` | `terndb_metrics_free` | a metrics snapshot |
| `terndb_log_record` | the log sink's argument (borrowed) | never | one log record |

### 2.2 Ownership rules

1. A handle returned through a non-`const` pointer is **owned** by the caller, who MUST free it exactly once with
   its `_free` (or `_close`) function. A handle returned as `const T *` is **borrowed** and MUST NOT be freed.
2. A borrowed handle, and every pointer into memory it reaches, is valid until its **anchor** is freed: the anchor
   is named in each accessor's rule. The anchors are the batch (for columns, column buffers, and types reached
   from a batch), the result (for types and fields reached from the result), the statement (for its type and
   shape), the error (for its strings), the metrics snapshot, and, for a log record, the return of the sink call.
3. **A batch outlives everything else.** It holds its own octets and its own reference to its type tree, so it
   stays readable after its result, its connection and its database are freed or closed. *Rationale: a batch
   handed to another thread or another language runtime must not dangle because the cursor that produced it went
   away; this is [WIRE] §9.1 rule 2 extended to the database.*
4. Freeing a handle while other handles that depend on it are alive is permitted, with the effect each `_free`
   rule states (closing a connection rolls its transactions back, freeing a result cancels it, closing a database
   ends its sessions). The dependants stay valid to free; any other call on them fails with `C1006` (`closed`).
   *Rationale: hosts with garbage collectors and finalizers (avin among them) free in an order they do not
   control.*
5. Every `_free` and `_close` function accepts NULL and does nothing.

### 2.3 Threads

1. The embedded engine is **thread-per-core**: `terndb_open` starts the database's **core threads** (config
   `threads`, default the number of online cores) and one **service thread**. The library MUST NOT run caller code
   on a core thread except the allocator functions of §3. Caller threads never become engine threads.
2. Thread safety per handle:

| Handle | Rule |
|---|---|
| `terndb_db`, `terndb_queue` | Fully thread-safe: any number of threads at once. |
| `terndb_conn`, and the `terndb_txn` and `terndb_stmt` it owns | **One thread at a time.** A call on a transaction or statement is a call on its connection. The connection MAY move between threads between calls. Overlapping calls are detected in every build and fail with `C1004` (`concurrent_use`). |
| `terndb_result`, `terndb_op` | One thread at a time, independently of the connection: a result may be read on one thread while its connection issues requests on another. |
| `terndb_batch`, its columns and types; `terndb_type` (owned); `terndb_error`; `terndb_metrics` | **Immutable**: any number of threads may read at once. Freeing races with nothing only if no other thread is still reading, which is the caller's to ensure. |
| `terndb_config`, the options objects | One thread at a time while being set; once set, any number of calls MAY read the same one concurrently. |

3. **A caller may block.** Every synchronous function blocks only its calling thread; the engine keeps serving
   every other connection meanwhile. A blocked caller holds no engine core.
4. A synchronous function, `terndb_op_wait` or `terndb_queue_wait` called **from an engine thread** (inside the
   log sink or an allocator function) MUST NOT block: a synchronous function fails with `C1003` (`wrong_thread`),
   and the two waits behave as §12.1 rule 22 says. *Rationale: a core thread waiting on itself deadlocks the engine.*
5. The allocator functions and the log sink MUST NOT call any `terndb_*` function except the log record accessors
   (from the sink).

### 2.4 What each build checks

1. **Every build** checks: NULL handles and NULL required pointers (`C1002`), concurrent use of a connection
   (`C1004`), calls on a closed handle (`C1006`), engine-thread waits (`C1003`), enum values and option values
   (`C1002`), indices passed to fallible functions.
2. **Debug builds** (`terndb_debug_checks()` returns true) additionally check every handle on every call: a
   handle MUST carry its kind and a generation, freed handles MUST NOT be reused for the life of the process (their
   memory is quarantined), and a freed handle or one of the wrong kind is `C1001` (`invalid_handle`). Value getters
   check their index and the column's representation, and abort on a violation.
3. A release build MUST behave identically to a debug build on every call that violates no precondition.

## 3. Library initialisation and versioning

1. `terndb_init` MUST be the first call into the library, before any other except `terndb_abi_version`,
   `terndb_version`, `terndb_code_name` and `terndb_debug_checks`. Any other call before it fails with `C1008`
   (`not_initialized`), or aborts where it has no error channel.
2. `terndb_init(abi_version, alloc, free, ctx, err)` checks the ABI the caller was compiled against: the caller
   passes `TERNDB_ABI_VERSION`. It fails with `C1009` (`abi_mismatch`) if the major differs from the library's or
   the minor exceeds the library's. *Rationale: a newer header promises functions an older library lacks; an older
   header on a newer library is always safe within a major (§13).*
3. `alloc` and `free` are the library's allocator for **all** memory it allocates, engine pages included. Both NULL
   selects the system allocator. One NULL and one not is `C1002`. `alloc(ctx, size, align)` returns memory aligned
   to `align` (a power of two, at most 16,384) or NULL; `free(ctx, ptr, size, align)` receives the same `size` and
   `align`. Both MUST be thread-safe: they are called from caller threads, core threads and the service thread.
   An `alloc` returning NULL fails the operation that needed it with `C2001` (`out_of_memory`), or aborts the
   transaction it was serving with `T1008` (`resource_exhausted`, `details.reason = "allocator"`).
4. A second `terndb_init` with the same arguments succeeds and does nothing; with different arguments it fails with
   `C1005` (`invalid_state`). There is no de-initialisation: the library's process-wide state lives until exit.
5. `terndb_abi_version()` returns the library's ABI as `major << 16 | minor`. `terndb_version()` returns the
   release version (`"1.4.2"`), static and NUL-terminated; the two are independent.

## 4. Databases (embedded mode)

### 4.1 Configuration

`terndb_config` is a set of `key = value` text pairs. `terndb_config_set` validates the key and the value's form at
once: an unknown key or a malformed value is `C1002` with `details.key`. Whether a key applies to the call it is
passed to is checked by that call: a key that does not apply there is `C1002`, never ignored. *Rationale: a
misspelt durability setting that silently selects `none` loses data; string keys let a minor release add settings
without new symbols.*

| Key | Values | Default | Used by |
|---|---|---|---|
| `mode` | `disk`, `memory` | `disk` | open, clone, promote |
| `durability` | `none`, `snapshot`, `log` | `log` | open (memory mode only; [DISK] §8.1) |
| `create` | `never`, `if_missing`, `always` | `if_missing` | open |
| `read_only` | `true`, `false` | `false` | open |
| `threads` | integer ≥ 1 | online cores | open |
| `memory_limit` | octets, integer | 0 (no limit) | open |
| `snapshot_interval_ms` | integer ≥ 1 | 60000 | open (memory mode, `snapshot` or `log`) |
| `max_snapshot_age_ms` | integer ≥ 0 | 3600000 | open ([ISO] §5.5) |
| `isolation`, `synchronous`, `lock_timeout_ms`, `statement_timeout_ms` | as [WIRE] `SessionDefaults` | server defaults | session, connect |
| `user`, `application` | text | `terndb`, empty | session, connect |
| `password`, `token`, `auth_methods` | text | — | connect ([WIRE] §2.4) |
| `tls_ca_file`, `tls_server_name` | text | system roots, URL host | connect |
| `max_in_flight`, `max_open_txns` | integer ≥ 1 | 256, 64 | session |

### 4.2 Open

1. `terndb_open(dir, dir_len, cfg, &db, err)` opens the database in directory `dir` ([DISK] §2) with `cfg` (NULL
   for defaults). In disk mode it takes the exclusive `flock` on `CONTROL` ([DISK] §2 rule 4) and runs recovery
   ([DISK] §6.9) before returning; another process holding the lock is `D4001` (`database_locked`).
2. `create`: `never` on a missing directory is `D4002` (`database_not_found`); `always` on a directory that holds a
   `CONTROL` is `D4003` (`database_exists`); `if_missing` creates a database only when there is no `CONTROL`. A new
   database is created with one superuser role named `terndb` with `login` ([DDL] §2.10), whose name an embedded
   session takes by default.
3. **Memory mode** with `durability = none` MAY be given an empty `dir` (length 0): an anonymous database that no
   directory backs. Every other combination needs a directory. Memory mode loads the directory's generation and
   recovers ([DISK] §8.3).
4. **Promotion by open.** A directory written in memory mode under `snapshot` or `log` durability MUST open in disk
   mode with no conversion ([DISK] §8.3), and a disk-mode directory MUST open in memory mode. `durability` given
   with `mode = disk` is `C1002`.
5. A format newer than the library is `D3001`; an older one it can only read opens read-only and every write fails
   with `D3002` ([DISK] §7.2). `read_only = true` opens with every transaction read-only (`T4001` on a write).
6. Any other failure to read or create the files is `D4004` (`io_error`), with `details.errno` and `details.file`.
7. Thread safety: any thread. Opening the same directory twice in one process is `D4001`, as from two processes.
8. `terndb_open_async` does the same and completes through an operation (§6); `terndb_op_take_db` yields the
   handle.

### 4.3 Close

1. `terndb_close(db, err)` ends the database: it marks every session of it closed, which rolls back their open
   transactions and cancels their running requests with `T1007` (`cancelled`, `details.reason = "shutdown"`),
   waits for them to end, then, in memory mode with `snapshot` durability, writes a final snapshot; in `log` mode
   and disk mode it takes a checkpoint ([DISK] §6.8). It stops the engine threads, releases the lock and frees the
   handle, even when it returns `-1`.
2. It returns `-1` only if the final snapshot or checkpoint failed (`D4004`); the database is then exactly as
   recovery would leave it after a crash at that moment, which [DISK] §9 makes safe.
3. Every handle derived from the database remains valid to free, and every other call on one fails with `C1006`.
   Batches remain readable (§2.2 rule 3).
4. `terndb_close` MUST NOT be called from an engine thread (`C1003`) and MUST NOT be called twice (debug `C1001`).
   It blocks until the engine has stopped; `terndb_close_async` does not.

### 4.4 Snapshot, clone and promote

1. `terndb_snapshot(db, dir, dir_len, err)` writes a snapshot generation ([DISK] §8.2). With `dir` empty it writes
   one to the database's own directory, which is what `snapshot` durability does periodically; with a directory
   given (memory mode, any durability), it writes a complete database there (`SNAPSHOT TO <dir>`), a directory
   that then opens in either mode. It returns once the generation is sealed and synced. A disk-mode database is
   `C1007` (`not_supported`); its backups belong to [DISK] §6.11. A directory that already holds a `CONTROL` is
   `D4003`.
2. `terndb_clone(src, dir, dir_len, cfg, &clone, err)` creates a new database that starts as an exact copy of `src`
   ([DISK] §8.4) and diverges from it. A memory-mode `src` clones in O(1) into a memory-mode database; `dir` MUST
   be empty (the clone is anonymous, `durability = none`) unless `cfg` sets `durability`, in which case `dir` is
   its directory. A disk-mode `src` clones by reflink into `dir`, which MUST be given and MUST NOT exist; a
   filesystem without reflink is `D3003`. Transactions active in `src` at the barrier are rolled back in the clone,
   as recovery would. `cfg` MAY set `threads` and `memory_limit`; a clone of either mode is open on return.
3. `terndb_promote(src, dir, dir_len, cfg, &db, err)` gives an on-disk database with `src`'s committed state:
   for a memory-mode `src` it writes a snapshot to `dir` (rule 1) and opens `dir` in disk mode; `src` stays open
   and unchanged. A disk-mode `src` is `C1007`. *Rationale: promotion is defined by the format as "open in the
   other mode" ([DISK] §8.3); the function is that composition, so that an anonymous test database can become a
   real one without the caller choosing an intermediate directory protocol.*
4. Each has an `_async` form. All three are thread-safe and may run while sessions on `src` are working: the
   barrier they take is the brief one of [DISK] §8.2 step 1.

## 5. Sessions, transactions and options

### 5.1 Sessions and connections

1. `terndb_session(db, cfg, &conn, err)` opens an embedded **session**: the in-process analogue of a [WIRE]
   connection, with its own `SessionDefaults`, transactions, statements and limits. `user` names its role
   (`Q6015` applies to what it runs, [DDL] §2.10); an unknown role, or one without `login`, is `W3001`. No
   authentication is performed: holding the open database is the credential.
2. `terndb_connect(url, url_len, cfg, &conn, err)` opens a **network** connection: `terndb://host[:port]/database`
   (TLS 1.3, [WIRE] §1.1 rule 2) or `terndb+unix:///path/to/socket?database=name`. It performs the handshake and
   authentication ([WIRE] §2) before returning; their errors are the [WIRE] codes. `terndb_connect_async` does not
   block.
3. Sessions are cheap: an idle one SHOULD cost at most 16 KiB, as a server connection does ([WIRE] §1.1 rule 5),
   so a host may keep one per logical task or fiber instead of pooling.
4. `terndb_conn_close(conn)` rolls back every open transaction, cancels every running request (`T1007`), frees the
   handle and never fails. Its transactions, statements and results stay valid to free (§2.2 rule 4).
5. A connection-fatal [WIRE] error (a `W1…` or `W3…` code, a `Goodbye`) closes a network connection: the error is
   reported to the operation it ended, and every later call on the connection fails with `C1006`.
6. `terndb_conn_is_embedded(conn)` is true for a session. `terndb_conn_catalog_version(conn)` is the catalog
   version at the last completed request (`Ready.catalog_version` at first).

### 5.2 Options objects

1. `terndb_txn_options` carries [WIRE] `TxnOptions`, field for field; `terndb_exec_options` carries `ExecOptions`.
   A fresh object has every field **absent**; a setter makes its field present. `_clear(opts, field)` makes it
   absent again. Absent fields take the session's defaults, then the server's ([WIRE] §5.2 rule 2).
2. An out-of-range value or an unknown isolation constant is `C1002` from the setter.
3. `terndb_exec_options_set_prefetch_bytes` is [WIRE] §7.2's credit expressed for a pull API: the engine (or, in
   network mode, the library, by granting `Credit`) produces at most that many octets of batches the caller has not
   yet pulled, then suspends the request exactly as [WIRE] §7.2 rule 4 says. Absent, it is 4 MiB. *Rationale: a
   caller that pulls slowly must bound the memory a large result takes; an unlimited window is still available
   with `SIZE_MAX`.*

### 5.3 Transactions

1. `terndb_begin(conn, opts, &txn, err)` begins an explicit transaction ([WIRE] §5.2) and returns its handle with
   `terndb_txn_id`, `terndb_txn_snapshot` and `terndb_txn_isolation` (the effective level) set.
2. `terndb_begin_async(conn, opts, &txn, q, tag)` writes `*txn` **at once**, before the transaction has begun, so
   the caller can pipeline `terndb_execute_async` and `terndb_commit_async` on it in the same breath ([WIRE] §5.1
   rule 1's rationale). Until the begin completes, `terndb_txn_state` is `TERNDB_TXN_PENDING` and the id getters
   return 0. If the begin fails, every request queued on the transaction fails with the begin's error.
3. Requests on one transaction run one at a time, in the order they were submitted ([WIRE] §4.6 rule 2); requests
   on different transactions and implicit ones are independent.
4. `terndb_commit(txn, &seq, err)` commits ([WIRE] §5.4). It returns `1` with `*seq` the commit sequence number when
   the transaction wrote, `0` when its write set was empty (no number, [ISO] §5.5), and `-1` on failure. A commit
   of an aborted transaction fails with the aborting error's code ([WIRE] §5.3 rule 3). `commit_unknown`
   (`T2001`) is resolved with `terndb_transaction_status` ([WIRE] §5.6); in embedded mode it arises only when the
   WAL device fails.
5. `terndb_rollback(txn, err)` pre-empts as [WIRE] §5.4 rule 3 says. `terndb_savepoint`,
   `terndb_rollback_to_savepoint` and `terndb_release_savepoint` are [WIRE] §5.5.
6. After commit or rollback the transaction is `TERNDB_TXN_CLOSED`; any further request on it is `W2001`. An error
   inside it sets `TERNDB_TXN_ABORTED` and later requests other than rollback, rollback-to-savepoint and commit are
   `W2003`.
7. `terndb_txn_free(txn)` frees the handle; an open transaction is rolled back first. A transaction with a commit in
   flight is not rolled back: the commit completes and its outcome is reported to its operation.
8. `terndb_transaction_status(conn, txid, &state, &seq, err)` is [WIRE] `TransactionStatus`: `state` is
   `TERNDB_TXN_STATUS_COMMITTED` (with `*seq`), `_ABORTED` or `_IN_PROGRESS`; a txid older than `status_horizon` is
   `W2010`.

## 6. Synchronous and asynchronous execution

### 6.1 The choice: a completion queue with a pollable descriptor

Every operation that can wait — open, close, snapshot, clone, promote, connect, begin, commit, rollback,
savepoints, transaction status, prepare, execute, and pulling a batch — has a **synchronous** form that blocks the
calling thread and an **`_async`** form that returns a `terndb_op *` at once. Completion is reported through a
**completion queue** (`terndb_queue`) that exposes a file descriptor, readable while the queue holds completions.
There are no completion callbacks.

*Rationale.* A callback would run on an engine core thread, so caller code — possibly blocking, possibly a
garbage-collecting runtime — would stall the core that every other session depends on; or it would run on a
library thread the host's runtime has never seen, which avin's scheduler, Python's GIL and the JVM all need
special registration for. A descriptor is the integration point every event loop already has: avin's `Poller`,
`epoll`, `kqueue`, libuv's `uv_poll`, Tokio's `AsyncFd`. A host adds one descriptor and drains the queue when it is
readable, on its own thread, under its own rules. A host with no event loop uses the synchronous forms, or
`terndb_queue_wait`.

### 6.2 Rules

1. **Equivalence.** Each synchronous function MUST behave exactly as its `_async` form followed by `terndb_op_wait`
   with no timeout and the matching `take` call: the same effects, outputs and errors.
2. `…_async(…, q, tag)` submits the operation and returns its handle. `q` MAY be NULL, in which case no completion
   is queued and the caller uses `terndb_op_poll` or `terndb_op_wait`. An async function fails at once only when it
   cannot allocate the operation: it then returns NULL, and nothing was submitted. Every other failure, argument
   errors included, is delivered through the operation.
3. When the operation completes, `tag` is appended to `q` **exactly once**. Completions from one connection on one
   transaction are appended in submission order; others in any order.
4. `terndb_queue_fd(q)` returns a descriptor that is readable (level-triggered) while `q` holds at least one tag.
   It MUST NOT be read, written or closed by the caller; it is closed by `terndb_queue_free`. On a platform without
   descriptors it returns −1 and the caller uses `terndb_queue_wait`. *Informative:* an `eventfd` on Linux, a pipe
   or `EVFILT_USER` kqueue on macOS.
5. `terndb_queue_drain(q, tags, max)` moves up to `max` tags into `tags` and returns how many, without blocking;
   when the queue becomes empty the descriptor stops being readable. `terndb_queue_wait(q, tags, max, timeout_ms)`
   blocks until at least one tag is available or the timeout (−1 = none) passes, and returns the count (0 on
   timeout).
6. A queue is fully thread-safe and may serve many connections and databases. `terndb_queue_free` with operations
   still pending is permitted: the queue is released when its last operation completes or is freed, and their
   completions are discarded.
7. `terndb_op_poll(op)` returns 1 when complete and 0 otherwise; `terndb_op_wait(op, timeout_ms)` blocks likewise.
   `terndb_op_error(op, err)` returns 0 if the completed operation succeeded and −1 with its error if it failed;
   on a pending operation it is `C1005`.
8. The `terndb_op_take_*` functions move an operation's output to the caller, once: the second call returns NULL.
   Calling the wrong one for the operation's kind is `C1002`.
9. `terndb_op_cancel(op)` asks for cancellation as [WIRE] §7.3 does: a statement then ends with `T1007` unless it
   finished first; a commit is never cancelled ([WIRE] §5.4 rule 4).
10. `terndb_op_free(op)` on a pending operation cancels it (rule 9) and discards its completion and its output:
    the tag is not appended. The output is freed by the library when it arrives (an opened database is closed, a
    result freed).
11. A caller MUST NOT hold the only reference to a pending `begin`'s transaction and free the operation expecting
    the transaction to be gone: the transaction handle is independent and is freed with `terndb_txn_free`.

### 6.3 Blocking inside the engine

Neither form ever blocks a core thread on the caller. In embedded mode the engine performs I/O asynchronously
([WIRE] §1.1 rule 5's rationale holds in-process), and a synchronous call parks only its own thread on a
completion. *Rationale: the engine's design does not change because the client is in-process; the ABI is a
client that happens to share the address space.*

## 7. Statements and execution

1. `terndb_prepare(conn, envelope, len, &stmt, err)` validates a [QT] §12.2 envelope without running it ([WIRE]
   §4.4 rule 1) and returns a statement handle with its shape digest (32 octets), catalog version, result type
   (NULL when the statement returns no value), and declared parameter types. The handle belongs to the connection
   and may be executed in any of its transactions.
2. `terndb_execute(conn, txn, implicit, exec, envelope, len, params, params_len, &result, err)` runs a statement
   ([WIRE] §4.3). `txn` NULL runs it in an implicit transaction ([WIRE] §4.5) with `implicit`'s options; a non-NULL
   `implicit` with a non-NULL `txn` is `W2011`. A `txn` of another connection is `C1002`. `exec` MAY be NULL.
3. `terndb_execute_prepared(stmt, txn, implicit, exec, params, params_len, &result, err)` runs a prepared
   statement. A changed catalog re-validates it ([WIRE] §4.4 rule 4): a different result type is `W2007`, and the
   statement is then released, so that every later execution of it is `W2005`.
4. `terndb_execute_digest(conn, txn, implicit, exec, digest, params, params_len, &result, err)` runs from the role's
   plan cache by its 32-octet shape digest, or fails with `W2004` and runs nothing ([WIRE] §4.4 rule 5).
5. `params` is a [WIRE] §4.2 parameter block, decoded by the statement's declared types, with the same errors and
   paths. NULL with length 0 is the empty block of §0 rule 4.
6. The synchronous execute returns once validation and binding succeeded and the result header is known: the
   transaction id, snapshot, shape and result type ([WIRE] §4.3 rules 1–2). Batches are then pulled (§8). A
   validation or binding failure is reported by the execute itself. The async form completes at the same point;
   `terndb_op_take_result` yields the handle.
7. `terndb_stmt_free` releases the statement; results it produced are unaffected.

## 8. Results and batches

### 8.1 The cursor

1. `terndb_result_next(result, &batch, err)` returns 1 with the next batch, 0 at the end of the result, −1 with the
   error that ended it ([WIRE] §3.3 rule 1). After 0 or −1 every further call returns the same value (−1 with a
   copy of the same error). `terndb_result_next_async(result, q, tag)` yields the batch through
   `terndb_op_take_batch`, which is NULL at the end.
2. Batches come in [WIRE] order: tuples in order, rows never split ([WIRE] §6.4 rules 4–5).
3. In an implicit transaction rows are **provisional until the end**: a commit failure arrives as the −1 after
   batches already pulled ([WIRE] §4.5 rule 2).
4. After the end, `terndb_result_counts(result, &ptr, &len, err)` gives `Complete.counts` (one `uint64_t` per
   parameter tuple, borrowed from the result) and `terndb_result_commit(result, &seq, err)` returns 1 with the
   commit sequence number, or 0 when absent. Before the end both are `C1005`; after an error the counts are those
   of `Complete` had it been sent and are not available (`C1005`).
5. `terndb_result_txid`, `terndb_result_snapshot`, `terndb_result_shape` (32 octets, borrowed) and
   `terndb_result_type` (NULL when the statement returns no value) are the `ResultHeader`'s and are available as
   soon as the result exists. `terndb_result_type_bytes` gives the [QT] §12.3 encoding octet for octet as in
   `ResultHeader.result_type`; its length is 0 when there is no type.
6. `terndb_result_cancel(result)` cancels the request ([WIRE] §7.3) and does not free the handle;
   `terndb_result_free` on an unfinished result cancels it and frees it. Freeing a result does not free batches
   pulled from it.

### 8.2 Batches as bytes

1. `terndb_batch_bytes(batch, &ptr, &len)` gives the [WIRE] §6.4 `Batch` payload, octet for octet as the wire
   carries it (uncompressed), 8-aligned ([WIRE] §9.1 rule 4). An embedded batch and a network batch of the same
   result MUST be identical octets.
2. `terndb_batch_decode(result_type, bytes, len, &batch, err)` makes a batch handle from payload octets obtained
   elsewhere (a client's own wire reader, a stored golden vector): it copies them into 8-aligned memory and
   checks them as [WIRE] §6.4 rule 2 requires, failing with `W1008` (`bad_batch`) and the path of the offending
   node. `result_type` is a relation type; the batch's column tree is `Col(E)` of its element. `terndb_type_decode`
   makes an owned type from [QT] §12.3 octets, failing with the [QT] decode codes.
3. Every pointer to batch memory is valid until `terndb_batch_free(batch)`.

### 8.3 Walking a batch

1. `terndb_batch_root(batch)` is node 0 of the column tree ([WIRE] §6.2 rule 4); `terndb_batch_column(batch, k)`
   is node `k` in pre-order, and `terndb_batch_column_count` the node count. `terndb_batch_rows` is the root's
   `length`; `terndb_batch_tuple` the parameter tuple.
2. `terndb_column_kind` is one of `TERNDB_COL_FIXED`, `_VARBIN`, `_RECORD`, `_UNION`, `_RELATION`.
   `terndb_column_type` is the node's type, nullable when the node is; `terndb_column_len` and
   `terndb_column_null_count` are its `length` and `null_count`; `terndb_column_is_null(col, i)` reads validity.
3. **Raw buffers.** `terndb_column_buffer_count` and `terndb_column_buffer(col, j, &ptr, &len)` give the node's
   buffers in [WIRE] §6.2 table order, `validity` first when nullable (empty when `null_count` is 0). On a
   little-endian host a fixed `values` buffer may be read as a C array of the column's type: `int64_t *` for
   `int64` and the 64-bit time types, `int32_t *` for `int32` and `date`, `double *` for `float64`.
   `terndb_column_fixed(col, &width)` is the `values` buffer directly.
4. **Value getters** return one slot's value in host order. Each accepts the representations listed and no other:

| Getter | Accepts |
|---|---|
| `terndb_column_get_bool` | `bool` |
| `…_get_i8`, `_i16`, `_i32`, `_i64` | `int8`, `int16`, `int32` and `date`, `int64` and `time`, `datetime`, `timestamp` |
| `…_get_u8`, `_u16`, `_u32`, `_u64` | the unsigned integers of that width |
| `…_get_f32`, `_f64` | `float32`, `float64` |
| `…_get_i128(col, i, &lo, &hi)` | `int128`, and `decimal`'s unscaled value (scale from `terndb_type_decimal`) |
| `…_get_interval(col, i, &months, &days, &micros)` | `interval` |
| `…_get_uuid(col, i, out16)` | `uuid`, in RFC 9562 order |
| `…_bytes(col, i, &len)` | `text` (valid UTF-8), `bytes`, `json`; borrowed from the batch |

   A null slot reads as its placeholder ([WIRE] §6.5); a caller tests `terndb_column_is_null` first.
5. **Record**: `terndb_column_field_count`, `terndb_column_field(col, j)`, `terndb_column_field_by_name(col, name,
   len)` (NULL when absent). Child `j`'s slot `i` is field `j` of slot `i`.
6. **Relation**: `terndb_column_children(col)` is the element node; `terndb_column_range(col, i, &start, &end)`
   gives slot `i`'s element slots `[start, end)` in it.
7. **Union**: `terndb_column_variant_count`, `terndb_column_union_tag(col, i)` (the variant ordinal),
   `terndb_column_union_offset(col, i)` (the slot in that variant's node) and `terndb_column_variant(col, j)`.
8. **Types**: `terndb_type_kind` is the [QT] §12.3 kind of the type with any nullable wrapper removed (never 20,
   never 32–63, [QT] §12.3's closing rule); `terndb_type_nullable` says whether the wrapper was there.
   `terndb_type_member_count`, `terndb_type_member_name`, `terndb_type_member_type` walk a record's fields or a
   union's variants; `terndb_type_element` is a `bag` or `list` element; `terndb_type_decimal` gives `p` and `s`;
   `terndb_type_bytes` gives the type's [QT] §12.3 encoding. A type reached from a batch is anchored by the batch;
   one reached from a result or statement by that handle.
9. A library MUST NOT return a type kind that the tree version of the statement's envelope does not define.

*Informative.* The [WIRE] §6.6 result (users → posts → comments) walks as: `root = terndb_batch_root(b)`;
`posts = terndb_column_field_by_name(root, "posts", 5)`; `terndb_column_range(posts, 0, &s, &e)` gives `[0, 2)`;
`p = terndb_column_children(posts)`; `comments = terndb_column_field_by_name(p, "comments", 8)`; and so on, with no
knowledge of buffer offsets.

## 9. Memory ownership

1. Everything the library allocates comes from the allocator of §3 and is freed by the library, through the
   handle's `_free`. The caller MUST NOT pass library memory to its own `free`, and the library never frees caller
   memory.
2. No function returns memory the caller frees with anything but a `terndb_*_free`.
3. `memory_limit` bounds the engine's own memory (frames, undo, execution state); a request that would pass it
   fails with `T1008` (`resource_exhausted`), and an open whose database cannot fit in memory mode fails with
   `C2001`. Batches the caller holds count against it until freed. *Rationale: a caller holding a million batches
   is holding the engine's memory, and should see the limit rather than the operating system's OOM killer.*
4. Handle memory is small and fixed per kind; batch memory is one allocation per batch, of the payload's size
   rounded up to 8.

## 10. Errors

### 10.1 The choice: an owned error object through an out-parameter

Every fallible function takes `terndb_error **err` last. On failure, if `err` is not NULL, `*err` receives a new
error the caller owns and frees with `terndb_error_free`; on success `*err` is not written. `err` MAY be NULL to
discard the detail. There is no thread-local "last error".

*Rationale.* Asynchronous completions are consumed on a thread other than the one that submitted them, and a host
runtime with green threads or fibers (avin's tasks, Go's goroutines) migrates them between OS threads between two
calls, so a thread-local slot can be overwritten or read by the wrong task. An owned object also carries all of
[WIRE] §8.1 — several entries, details, spans — which a `(code, static string)` pair cannot.

### 10.2 Accessors

1. An error has the fields of [WIRE] §8.1, read through the same accessors whichever mode produced it ([WIRE]
   §9.1 rule 6): `terndb_error_entry_count` (≥ 1; entry 0 is the primary error), and per entry
   `terndb_error_code` (the five-character code, static, NUL-terminated: `"T1001"`), `terndb_error_name` (its
   `snake_case` name, static), `terndb_error_message`, `terndb_error_path`, `terndb_error_detail_count`,
   `terndb_error_detail` and `terndb_error_span`; and for the whole error `terndb_error_retryable`,
   `terndb_error_txn_state`, `terndb_error_txid` and `terndb_error_tuple`.
2. Strings are borrowed from the error and valid until it is freed. A caller matches on the code ([ERR] §1 rule 3).
3. Errors the C layer raises itself (`C…`, `D4…`) have one entry, an empty path, `txn_state = unaffected`, and are
   never retryable.
4. `terndb_code_name(code)` maps a code to its name from the registry the library was built with, or returns NULL
   for an unknown code.

### 10.3 Codes this document raises

Registered in [ERR] §7 (area `C`) and §6 (`D4…`); cited here by code: `C1001` `invalid_handle`, `C1002`
`invalid_argument`, `C1003` `wrong_thread`, `C1004` `concurrent_use`, `C1005` `invalid_state`, `C1006` `closed`,
`C1007` `not_supported`, `C1008` `not_initialized`, `C1009` `abi_mismatch`, `C2001` `out_of_memory`, `D4001`
`database_locked`, `D4002` `database_not_found`, `D4003` `database_exists`, `D4004` `io_error`. Every other code
comes from the spec that defines the failure, unchanged: a validation error is the [QT] code, an abort the [ISO]
code, a handle-state error on a transaction or statement the [WIRE] code.

## 11. Logging and metrics

### 11.1 The log sink

1. `terndb_set_log_sink(min_level, fn, ctx, err)` installs the process's one sink; `fn` NULL removes it, and the
   library then writes nothing anywhere. Levels: `TERNDB_LOG_ERROR` 1, `_WARN` 2, `_INFO` 3, `_DEBUG` 4,
   `_TRACE` 5; records above `min_level` are not built at all.
2. `fn(ctx, record)` is called **on the service thread**, one record at a time, in the order the records were
   emitted per core thread. Records are queued from the core threads without blocking them; if the sink falls
   more than 65,536 records behind, the oldest are dropped and the next delivered record has the field
   `dropped = <n>`. *Rationale: a slow sink must never slow a commit.*
3. Record accessors, valid only during the call: `terndb_log_record_level`, `_time` (µs since the Unix epoch),
   `_target` (the subsystem, `"wal"`, `"exec"`), `_message`, `_field_count` and `_field(record, k, …)` (key/value
   text pairs, `database` and `session` among them where they apply).
4. The sink MUST NOT block for long and MUST NOT call other `terndb_*` functions (§2.3 rule 5). Setting a sink may
   be done at any time after `terndb_init`, from any thread; the old sink is not called after the setter returns.

### 11.2 Metrics

1. Metrics are **pulled**: `terndb_metrics_snapshot(db, &m, err)` copies every metric of the database at one
   instant, from any thread, without stopping the engine. Each metric has a name, a kind (`TERNDB_METRIC_COUNTER`,
   `_GAUGE`, `_HISTOGRAM`), labels (key/value text pairs, such as `core = "3"`) and a value: a counter's or
   gauge's `int64_t` through `terndb_metrics_value`; a histogram's sum through it and its cumulative buckets
   through `terndb_metrics_bucket_count` and `terndb_metrics_bucket(m, i, k, &upper_bound, &count)`.
2. Names are stable once released (§13), lowercase and dotted (`terndb.commits`, `terndb.wal.fsync_us`). At least
   these MUST exist: `terndb.commits`, `terndb.aborts` (label `code`), `terndb.sessions`, `terndb.txns.open`,
   `terndb.wal.bytes`, `terndb.wal.fsync_us` (histogram), `terndb.buffer.hits`, `terndb.buffer.misses`,
   `terndb.undo.bytes`, `terndb.snapshot.oldest_age_ms`, `terndb.memory.bytes`.
3. *Rationale: a pull fits every host — a Prometheus scrape, an avin timer, a test assertion — and runs host code
   on no engine thread.*

## 12. The header

The header is `terndb.h`. The declarations below are the whole of ABI 1.0; §13 governs how it grows.

```c
#ifndef TERNDB_H
#define TERNDB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- versions ---- */
#define TERNDB_ABI_MAJOR   1
#define TERNDB_ABI_MINOR   0
#define TERNDB_ABI_VERSION (((uint32_t)TERNDB_ABI_MAJOR << 16) | (uint32_t)TERNDB_ABI_MINOR)

/* ---- handles ---- */
typedef struct terndb_config       terndb_config;
typedef struct terndb_db           terndb_db;
typedef struct terndb_conn         terndb_conn;
typedef struct terndb_txn_options  terndb_txn_options;
typedef struct terndb_exec_options terndb_exec_options;
typedef struct terndb_txn          terndb_txn;
typedef struct terndb_stmt         terndb_stmt;
typedef struct terndb_result       terndb_result;
typedef struct terndb_batch        terndb_batch;
typedef struct terndb_column       terndb_column;
typedef struct terndb_type         terndb_type;
typedef struct terndb_error        terndb_error;
typedef struct terndb_queue        terndb_queue;
typedef struct terndb_op           terndb_op;
typedef struct terndb_metrics      terndb_metrics;
typedef struct terndb_log_record   terndb_log_record;

/* ---- constants ---- */
#define TERNDB_ISOLATION_SERIALIZABLE   0
#define TERNDB_ISOLATION_SNAPSHOT       1
#define TERNDB_ISOLATION_READ_COMMITTED 2

#define TERNDB_SYNC_OFF          0        /* isolation.md §6, replication.md §6.1 */
#define TERNDB_SYNC_ON           1
#define TERNDB_SYNC_REMOTE       2
#define TERNDB_SYNC_REMOTE_APPLY 3

#define TERNDB_TXN_PENDING  0
#define TERNDB_TXN_OPEN     1
#define TERNDB_TXN_ABORTED  2
#define TERNDB_TXN_CLOSED   3

#define TERNDB_TXN_STATUS_COMMITTED   0
#define TERNDB_TXN_STATUS_ABORTED     1
#define TERNDB_TXN_STATUS_IN_PROGRESS 2

#define TERNDB_TXN_STATE_UNAFFECTED 0     /* terndb_error_txn_state, wire-protocol.md §8.1 */
#define TERNDB_TXN_STATE_ABORTED    1
#define TERNDB_TXN_STATE_CLOSED     2

#define TERNDB_COL_FIXED    1
#define TERNDB_COL_VARBIN   2
#define TERNDB_COL_RECORD   3
#define TERNDB_COL_UNION    4
#define TERNDB_COL_RELATION 5

#define TERNDB_TYPE_BOOL       0          /* query-tree.md §12.3 kinds */
#define TERNDB_TYPE_INT16      1
#define TERNDB_TYPE_INT32      2
#define TERNDB_TYPE_INT64      3
#define TERNDB_TYPE_INT128     4
#define TERNDB_TYPE_FLOAT32    5
#define TERNDB_TYPE_FLOAT64    6
#define TERNDB_TYPE_DECIMAL    7
#define TERNDB_TYPE_TEXT       8
#define TERNDB_TYPE_BYTES      9
#define TERNDB_TYPE_DATE      10
#define TERNDB_TYPE_TIME      11
#define TERNDB_TYPE_DATETIME  12
#define TERNDB_TYPE_TIMESTAMP 13
#define TERNDB_TYPE_INTERVAL  14
#define TERNDB_TYPE_UUID      15
#define TERNDB_TYPE_RECORD    16
#define TERNDB_TYPE_UNION     17
#define TERNDB_TYPE_BAG       18
#define TERNDB_TYPE_LIST      19
#define TERNDB_TYPE_INT8      21
#define TERNDB_TYPE_UINT8     22
#define TERNDB_TYPE_UINT16    23
#define TERNDB_TYPE_UINT32    24
#define TERNDB_TYPE_UINT64    25
#define TERNDB_TYPE_JSON      26

#define TERNDB_LOG_ERROR 1
#define TERNDB_LOG_WARN  2
#define TERNDB_LOG_INFO  3
#define TERNDB_LOG_DEBUG 4
#define TERNDB_LOG_TRACE 5

#define TERNDB_METRIC_COUNTER   1
#define TERNDB_METRIC_GAUGE     2
#define TERNDB_METRIC_HISTOGRAM 3

#define TERNDB_TXN_OPT_ISOLATION          1   /* terndb_txn_options_clear */
#define TERNDB_TXN_OPT_SYNCHRONOUS        2
#define TERNDB_TXN_OPT_READ_ONLY          3
#define TERNDB_TXN_OPT_AFTER              4
#define TERNDB_TXN_OPT_LOCK_TIMEOUT       5
#define TERNDB_TXN_OPT_REPLICA_WAIT       6
#define TERNDB_TXN_OPT_MAX_LAG            7
#define TERNDB_EXEC_OPT_PREFETCH_BYTES    1   /* terndb_exec_options_clear */
#define TERNDB_EXEC_OPT_BATCH_ROWS        2
#define TERNDB_EXEC_OPT_BATCH_BYTES       3
#define TERNDB_EXEC_OPT_STATEMENT_TIMEOUT 4

/* ---- callbacks ---- */
typedef void *(*terndb_alloc_fn)(void *ctx, size_t size, size_t align);
typedef void  (*terndb_free_fn)(void *ctx, void *ptr, size_t size, size_t align);
typedef void  (*terndb_log_fn)(void *ctx, const terndb_log_record *record);

/* ---- library (§3) ---- */
uint32_t    terndb_abi_version(void);
const char *terndb_version(void);
bool        terndb_debug_checks(void);
size_t      terndb_debug_live_handles(void);
int         terndb_init(uint32_t abi_version, terndb_alloc_fn alloc, terndb_free_fn free_fn, void *ctx,
                        terndb_error **err);
const char *terndb_code_name(const char *code);

/* ---- configuration (§4.1) ---- */
int  terndb_config_new(terndb_config **out, terndb_error **err);
int  terndb_config_set(terndb_config *cfg, const char *key, size_t key_len, const char *value, size_t value_len,
                       terndb_error **err);
void terndb_config_free(terndb_config *cfg);

/* ---- databases (§4) ---- */
int        terndb_open(const char *dir, size_t dir_len, const terndb_config *cfg, terndb_db **out,
                       terndb_error **err);
terndb_op *terndb_open_async(const char *dir, size_t dir_len, const terndb_config *cfg, terndb_queue *q,
                             uint64_t tag);
int        terndb_close(terndb_db *db, terndb_error **err);
terndb_op *terndb_close_async(terndb_db *db, terndb_queue *q, uint64_t tag);
int        terndb_snapshot(terndb_db *db, const char *dir, size_t dir_len, terndb_error **err);
terndb_op *terndb_snapshot_async(terndb_db *db, const char *dir, size_t dir_len, terndb_queue *q, uint64_t tag);
int        terndb_clone(terndb_db *src, const char *dir, size_t dir_len, const terndb_config *cfg,
                        terndb_db **out, terndb_error **err);
terndb_op *terndb_clone_async(terndb_db *src, const char *dir, size_t dir_len, const terndb_config *cfg,
                              terndb_queue *q, uint64_t tag);
int        terndb_promote(terndb_db *src, const char *dir, size_t dir_len, const terndb_config *cfg,
                          terndb_db **out, terndb_error **err);
terndb_op *terndb_promote_async(terndb_db *src, const char *dir, size_t dir_len, const terndb_config *cfg,
                                terndb_queue *q, uint64_t tag);
bool       terndb_db_is_memory(const terndb_db *db);

/* ---- sessions and connections (§5.1) ---- */
int        terndb_session(terndb_db *db, const terndb_config *cfg, terndb_conn **out, terndb_error **err);
int        terndb_connect(const char *url, size_t url_len, const terndb_config *cfg, terndb_conn **out,
                          terndb_error **err);
terndb_op *terndb_connect_async(const char *url, size_t url_len, const terndb_config *cfg, terndb_queue *q,
                                uint64_t tag);
void       terndb_conn_close(terndb_conn *conn);
bool       terndb_conn_is_embedded(const terndb_conn *conn);
uint64_t   terndb_conn_catalog_version(const terndb_conn *conn);

/* ---- options (§5.2) ---- */
int  terndb_txn_options_new(terndb_txn_options **out, terndb_error **err);
int  terndb_txn_options_set_isolation(terndb_txn_options *o, int32_t isolation, terndb_error **err);
int  terndb_txn_options_set_synchronous(terndb_txn_options *o, int32_t level, terndb_error **err);
int  terndb_txn_options_set_read_only(terndb_txn_options *o, bool on, terndb_error **err);
int  terndb_txn_options_set_after(terndb_txn_options *o, uint64_t commit_seq, terndb_error **err);
int  terndb_txn_options_set_lock_timeout_ms(terndb_txn_options *o, uint64_t ms, terndb_error **err);
int  terndb_txn_options_set_replica_wait_timeout_ms(terndb_txn_options *o, uint64_t ms, terndb_error **err);
int  terndb_txn_options_set_max_lag_ms(terndb_txn_options *o, uint64_t ms, terndb_error **err);
int  terndb_txn_options_clear(terndb_txn_options *o, int32_t field, terndb_error **err);
void terndb_txn_options_free(terndb_txn_options *o);

int  terndb_exec_options_new(terndb_exec_options **out, terndb_error **err);
int  terndb_exec_options_set_prefetch_bytes(terndb_exec_options *o, size_t octets, terndb_error **err);
int  terndb_exec_options_set_batch_rows(terndb_exec_options *o, size_t rows, terndb_error **err);
int  terndb_exec_options_set_batch_bytes(terndb_exec_options *o, size_t octets, terndb_error **err);
int  terndb_exec_options_set_statement_timeout_ms(terndb_exec_options *o, uint64_t ms, terndb_error **err);
int  terndb_exec_options_clear(terndb_exec_options *o, int32_t field, terndb_error **err);
void terndb_exec_options_free(terndb_exec_options *o);

/* ---- transactions (§5.3) ---- */
int        terndb_begin(terndb_conn *conn, const terndb_txn_options *opts, terndb_txn **out, terndb_error **err);
terndb_op *terndb_begin_async(terndb_conn *conn, const terndb_txn_options *opts, terndb_txn **out,
                              terndb_queue *q, uint64_t tag);
int        terndb_commit(terndb_txn *txn, uint64_t *commit_seq, terndb_error **err);
terndb_op *terndb_commit_async(terndb_txn *txn, terndb_queue *q, uint64_t tag);
int        terndb_rollback(terndb_txn *txn, terndb_error **err);
terndb_op *terndb_rollback_async(terndb_txn *txn, terndb_queue *q, uint64_t tag);
int        terndb_savepoint(terndb_txn *txn, const char *name, size_t name_len, terndb_error **err);
terndb_op *terndb_savepoint_async(terndb_txn *txn, const char *name, size_t name_len, terndb_queue *q,
                                  uint64_t tag);
int        terndb_rollback_to_savepoint(terndb_txn *txn, const char *name, size_t name_len, terndb_error **err);
terndb_op *terndb_rollback_to_savepoint_async(terndb_txn *txn, const char *name, size_t name_len,
                                              terndb_queue *q, uint64_t tag);
int        terndb_release_savepoint(terndb_txn *txn, const char *name, size_t name_len, terndb_error **err);
terndb_op *terndb_release_savepoint_async(terndb_txn *txn, const char *name, size_t name_len, terndb_queue *q,
                                          uint64_t tag);
int32_t    terndb_txn_state(const terndb_txn *txn);
uint64_t   terndb_txn_id(const terndb_txn *txn);
uint64_t   terndb_txn_snapshot(const terndb_txn *txn);
int32_t    terndb_txn_isolation(const terndb_txn *txn);
void       terndb_txn_free(terndb_txn *txn);
int        terndb_transaction_status(terndb_conn *conn, uint64_t txid, int32_t *state, uint64_t *commit_seq,
                                     terndb_error **err);
terndb_op *terndb_transaction_status_async(terndb_conn *conn, uint64_t txid, terndb_queue *q, uint64_t tag);

/* ---- statements and execution (§7) ---- */
int        terndb_prepare(terndb_conn *conn, const uint8_t *envelope, size_t len, terndb_stmt **out,
                          terndb_error **err);
terndb_op *terndb_prepare_async(terndb_conn *conn, const uint8_t *envelope, size_t len, terndb_queue *q,
                                uint64_t tag);
const uint8_t      *terndb_stmt_shape(const terndb_stmt *stmt);                     /* 32 octets */
uint64_t            terndb_stmt_catalog_version(const terndb_stmt *stmt);
const terndb_type  *terndb_stmt_result_type(const terndb_stmt *stmt);               /* NULL: no value */
size_t              terndb_stmt_param_count(const terndb_stmt *stmt);
const terndb_type  *terndb_stmt_param_type(const terndb_stmt *stmt, size_t i);
void                terndb_stmt_free(terndb_stmt *stmt);

int        terndb_execute(terndb_conn *conn, terndb_txn *txn, const terndb_txn_options *implicit,
                          const terndb_exec_options *exec, const uint8_t *envelope, size_t envelope_len,
                          const uint8_t *params, size_t params_len, terndb_result **out, terndb_error **err);
terndb_op *terndb_execute_async(terndb_conn *conn, terndb_txn *txn, const terndb_txn_options *implicit,
                                const terndb_exec_options *exec, const uint8_t *envelope, size_t envelope_len,
                                const uint8_t *params, size_t params_len, terndb_queue *q, uint64_t tag);
int        terndb_execute_prepared(terndb_stmt *stmt, terndb_txn *txn, const terndb_txn_options *implicit,
                                   const terndb_exec_options *exec, const uint8_t *params, size_t params_len,
                                   terndb_result **out, terndb_error **err);
terndb_op *terndb_execute_prepared_async(terndb_stmt *stmt, terndb_txn *txn, const terndb_txn_options *implicit,
                                         const terndb_exec_options *exec, const uint8_t *params,
                                         size_t params_len, terndb_queue *q, uint64_t tag);
int        terndb_execute_digest(terndb_conn *conn, terndb_txn *txn, const terndb_txn_options *implicit,
                                 const terndb_exec_options *exec, const uint8_t digest[32],
                                 const uint8_t *params, size_t params_len, terndb_result **out,
                                 terndb_error **err);
terndb_op *terndb_execute_digest_async(terndb_conn *conn, terndb_txn *txn, const terndb_txn_options *implicit,
                                       const terndb_exec_options *exec, const uint8_t digest[32],
                                       const uint8_t *params, size_t params_len, terndb_queue *q, uint64_t tag);

/* ---- results (§8.1) ---- */
int                terndb_result_next(terndb_result *r, terndb_batch **out, terndb_error **err);
terndb_op         *terndb_result_next_async(terndb_result *r, terndb_queue *q, uint64_t tag);
uint64_t           terndb_result_txid(const terndb_result *r);
uint64_t           terndb_result_snapshot(const terndb_result *r);
const uint8_t     *terndb_result_shape(const terndb_result *r);                     /* 32 octets */
const terndb_type *terndb_result_type(const terndb_result *r);                      /* NULL: no value */
void               terndb_result_type_bytes(const terndb_result *r, const uint8_t **ptr, size_t *len);
int                terndb_result_counts(const terndb_result *r, const uint64_t **ptr, size_t *len,
                                        terndb_error **err);
int                terndb_result_commit(const terndb_result *r, uint64_t *commit_seq, terndb_error **err);
void               terndb_result_cancel(terndb_result *r);
void               terndb_result_free(terndb_result *r);

/* ---- batches (§8.2, §8.3) ---- */
int                  terndb_batch_decode(const terndb_type *result_type, const uint8_t *bytes, size_t len,
                                         terndb_batch **out, terndb_error **err);
void                 terndb_batch_bytes(const terndb_batch *b, const uint8_t **ptr, size_t *len);
uint64_t             terndb_batch_tuple(const terndb_batch *b);
size_t               terndb_batch_rows(const terndb_batch *b);
const terndb_type   *terndb_batch_type(const terndb_batch *b);                      /* the result type */
const terndb_column *terndb_batch_root(const terndb_batch *b);
size_t               terndb_batch_column_count(const terndb_batch *b);
const terndb_column *terndb_batch_column(const terndb_batch *b, size_t preorder_index);
void                 terndb_batch_free(terndb_batch *b);

/* ---- columns (§8.3) ---- */
int32_t              terndb_column_kind(const terndb_column *c);
const terndb_type   *terndb_column_type(const terndb_column *c);
size_t               terndb_column_len(const terndb_column *c);
size_t               terndb_column_null_count(const terndb_column *c);
bool                 terndb_column_is_null(const terndb_column *c, size_t i);
size_t               terndb_column_buffer_count(const terndb_column *c);
void                 terndb_column_buffer(const terndb_column *c, size_t j, const uint8_t **ptr, size_t *len);
const uint8_t       *terndb_column_fixed(const terndb_column *c, size_t *width);
bool                 terndb_column_get_bool(const terndb_column *c, size_t i);
int8_t               terndb_column_get_i8(const terndb_column *c, size_t i);
int16_t              terndb_column_get_i16(const terndb_column *c, size_t i);
int32_t              terndb_column_get_i32(const terndb_column *c, size_t i);
int64_t              terndb_column_get_i64(const terndb_column *c, size_t i);
uint8_t              terndb_column_get_u8(const terndb_column *c, size_t i);
uint16_t             terndb_column_get_u16(const terndb_column *c, size_t i);
uint32_t             terndb_column_get_u32(const terndb_column *c, size_t i);
uint64_t             terndb_column_get_u64(const terndb_column *c, size_t i);
float                terndb_column_get_f32(const terndb_column *c, size_t i);
double               terndb_column_get_f64(const terndb_column *c, size_t i);
void                 terndb_column_get_i128(const terndb_column *c, size_t i, uint64_t *lo, int64_t *hi);
void                 terndb_column_get_interval(const terndb_column *c, size_t i, int32_t *months, int32_t *days,
                                                int64_t *micros);
void                 terndb_column_get_uuid(const terndb_column *c, size_t i, uint8_t out[16]);
const uint8_t       *terndb_column_bytes(const terndb_column *c, size_t i, size_t *len);
size_t               terndb_column_field_count(const terndb_column *c);
const terndb_column *terndb_column_field(const terndb_column *c, size_t j);
const terndb_column *terndb_column_field_by_name(const terndb_column *c, const char *name, size_t name_len);
const terndb_column *terndb_column_children(const terndb_column *c);
void                 terndb_column_range(const terndb_column *c, size_t i, size_t *start, size_t *end);
size_t               terndb_column_variant_count(const terndb_column *c);
uint32_t             terndb_column_union_tag(const terndb_column *c, size_t i);
size_t               terndb_column_union_offset(const terndb_column *c, size_t i);
const terndb_column *terndb_column_variant(const terndb_column *c, size_t j);

/* ---- types (§8.3) ---- */
int                terndb_type_decode(const uint8_t *bytes, size_t len, terndb_type **out, terndb_error **err);
int32_t            terndb_type_kind(const terndb_type *t);
bool               terndb_type_nullable(const terndb_type *t);
size_t             terndb_type_member_count(const terndb_type *t);
void               terndb_type_member_name(const terndb_type *t, size_t j, const char **ptr, size_t *len);
const terndb_type *terndb_type_member_type(const terndb_type *t, size_t j);
const terndb_type *terndb_type_element(const terndb_type *t);
void               terndb_type_decimal(const terndb_type *t, uint32_t *precision, uint32_t *scale);
void               terndb_type_bytes(const terndb_type *t, const uint8_t **ptr, size_t *len);
void               terndb_type_free(terndb_type *t);

/* ---- asynchronous operations (§6) ---- */
int            terndb_queue_new(terndb_queue **out, terndb_error **err);
int            terndb_queue_fd(const terndb_queue *q);
size_t         terndb_queue_drain(terndb_queue *q, uint64_t *tags, size_t max);
size_t         terndb_queue_wait(terndb_queue *q, uint64_t *tags, size_t max, int64_t timeout_ms);
void           terndb_queue_free(terndb_queue *q);
int            terndb_op_poll(const terndb_op *op);
int            terndb_op_wait(terndb_op *op, int64_t timeout_ms);
int            terndb_op_error(terndb_op *op, terndb_error **err);
terndb_db     *terndb_op_take_db(terndb_op *op);
terndb_conn   *terndb_op_take_conn(terndb_op *op);
terndb_stmt   *terndb_op_take_stmt(terndb_op *op);
terndb_result *terndb_op_take_result(terndb_op *op);
terndb_batch  *terndb_op_take_batch(terndb_op *op);
int            terndb_op_commit_seq(const terndb_op *op, uint64_t *commit_seq);
int            terndb_op_txn_status(const terndb_op *op, int32_t *state, uint64_t *commit_seq);
void           terndb_op_cancel(terndb_op *op);
void           terndb_op_free(terndb_op *op);

/* ---- errors (§10) ---- */
size_t      terndb_error_entry_count(const terndb_error *e);
const char *terndb_error_code(const terndb_error *e, size_t entry);
const char *terndb_error_name(const terndb_error *e, size_t entry);
void        terndb_error_message(const terndb_error *e, size_t entry, const char **ptr, size_t *len);
void        terndb_error_path(const terndb_error *e, size_t entry, const char **ptr, size_t *len);
size_t      terndb_error_detail_count(const terndb_error *e, size_t entry);
void        terndb_error_detail(const terndb_error *e, size_t entry, size_t k, const char **key, size_t *key_len,
                                const char **value, size_t *value_len);
bool        terndb_error_span(const terndb_error *e, size_t entry, uint64_t *start, uint64_t *end);
bool        terndb_error_retryable(const terndb_error *e);
int32_t     terndb_error_txn_state(const terndb_error *e);
bool        terndb_error_txid(const terndb_error *e, uint64_t *txid);
bool        terndb_error_tuple(const terndb_error *e, uint64_t *tuple);
void        terndb_error_free(terndb_error *e);

/* ---- logging and metrics (§11) ---- */
int      terndb_set_log_sink(int32_t min_level, terndb_log_fn fn, void *ctx, terndb_error **err);
int32_t  terndb_log_record_level(const terndb_log_record *r);
int64_t  terndb_log_record_time(const terndb_log_record *r);
void     terndb_log_record_target(const terndb_log_record *r, const char **ptr, size_t *len);
void     terndb_log_record_message(const terndb_log_record *r, const char **ptr, size_t *len);
size_t   terndb_log_record_field_count(const terndb_log_record *r);
void     terndb_log_record_field(const terndb_log_record *r, size_t k, const char **key, size_t *key_len,
                                 const char **value, size_t *value_len);

int      terndb_metrics_snapshot(terndb_db *db, terndb_metrics **out, terndb_error **err);
size_t   terndb_metrics_count(const terndb_metrics *m);
void     terndb_metrics_name(const terndb_metrics *m, size_t i, const char **ptr, size_t *len);
int32_t  terndb_metrics_kind(const terndb_metrics *m, size_t i);
size_t   terndb_metrics_label_count(const terndb_metrics *m, size_t i);
void     terndb_metrics_label(const terndb_metrics *m, size_t i, size_t k, const char **key, size_t *key_len,
                              const char **value, size_t *value_len);
int64_t  terndb_metrics_value(const terndb_metrics *m, size_t i);
size_t   terndb_metrics_bucket_count(const terndb_metrics *m, size_t i);
void     terndb_metrics_bucket(const terndb_metrics *m, size_t i, size_t k, uint64_t *upper_bound,
                               uint64_t *count);
void     terndb_metrics_free(terndb_metrics *m);

#ifdef __cplusplus
}
#endif

#endif /* TERNDB_H */
```

### 12.1 Per-function rules not stated above

Each rule names preconditions (P), ownership (O), thread safety (T) and errors (E) where they are not already fixed
by §1–§11. A function not listed has exactly the behaviour its section gives, with T from §2.3.

1. `terndb_debug_live_handles` — counts handles allocated and not yet freed, of every kind, in the process. O:
   none. T: any. *Purpose: the leak test of §15.*
2. `terndb_config_set` — P: `key` and `value` valid UTF-8. A key set twice keeps the last value. E: `C1002`.
3. `terndb_db_is_memory` — T: any. Never fails.
4. `terndb_session` — P: `db` open. O: the session is owned; it holds the database open only in the sense of §4.3:
   closing the database closes it. T: any thread (the database is thread-safe). E: `C1006`, `W3001`, `C1002` for a
   network-only key.
5. `terndb_connect` — P: `url` in one of the forms of §5.1 rule 2 (`C1002`). E: the handshake's `W…` codes. A DNS,
   socket or TLS failure before the handshake has no registered code yet (Open question 4); until it does, a
   library reports it as `C1002` with `details.reason = "connect"` and `details.errno`.
6. `terndb_conn_catalog_version` — T: the connection's rule. Never fails.
7. Options setters — O: none. E: `C1002` (an isolation outside the three constants; a timeout above
   2⁶³ − 1). `_clear` with an unknown field constant is `C1002`.
8. `terndb_begin` — P: `opts` MAY be NULL. O: `*out` owned. E: [WIRE] §5.2 (`T1005`, `T4001`), `W2016`.
9. `terndb_commit` — P: `commit_seq` MAY be NULL. E: [WIRE] §5.3–5.4, `W2001` when closed.
10. `terndb_savepoint` family — P: `name` valid UTF-8, at most 255 octets (`Q6019`). E: `W2012`, `W2003`.
11. `terndb_txn_state`, `_id`, `_snapshot`, `_isolation` — T: the connection's rule; they read state and never
    block. `_isolation` is −1 while pending.
12. `terndb_prepare` — E: [QT] codes with paths; `W2008` past `max_in_flight`.
13. `terndb_stmt_*` getters — O: borrowed from the statement. `terndb_stmt_param_type` P: `i` < count.
14. `terndb_execute*` — P: `params` decodes against the statement's declarations; `txn` (if given) belongs to the
    connection. O: `*out` owned; the inputs copied. E: [QT], [WIRE] §4, [ISO] codes; `W2008`; `C1004` if the
    connection is in use on another thread.
15. `terndb_result_type_bytes` — O: borrowed from the result. Never fails.
16. `terndb_batch_decode` — P: `result_type` a relation type (`C1002` otherwise). O: the batch is owned and holds a
    copy of `result_type`, so the type may be freed at once.
17. `terndb_batch_column` — P: `preorder_index` < count (debug abort). O: borrowed from the batch.
18. Column getters — P: `i` < `terndb_column_len`, the representation accepted (§8.3 rule 4). Debug builds abort
    on a violation; release builds are undefined.
19. `terndb_column_children`, `_range` — P: a relation column. `_variant`, `_union_*` — P: a union column, `j` <
    variant count. `_field*` — P: a record column.
20. `terndb_type_decode` — O: owned; free with `terndb_type_free`. A named type reference (kind 32) is refused with
    `Q1002`, since only structural types are results.
21. `terndb_type_free` — P: an owned type; a borrowed one is `C1001` in debug builds.
22. `terndb_queue_wait`, `terndb_op_wait` — T: not from an engine thread (§2.3 rule 4). Called from one, a debug
    build aborts naming `C1003`, and a release build returns at once: `terndb_op_wait` −1, `terndb_queue_wait` 0.
23. `terndb_op_commit_seq` — for a commit operation: returns 1 with the number, 0 when absent, −1 when the
    operation is pending, failed, or not a commit. `terndb_op_txn_status` likewise for a status operation.
24. `terndb_error_code` — P: `entry` < count. Never NULL.
25. `terndb_set_log_sink` — P: `min_level` in 0…5 (0 delivers nothing). E: `C1002`.
26. `terndb_metrics_snapshot` — P: `db` open (`C1006`). T: any.

## 13. Versioning and symbol stability

1. **ABI version** `major.minor`, carried by `TERNDB_ABI_VERSION`, `terndb_abi_version()` and `terndb_init` (§3).
   It is independent of the release version and of the tree and wire versions.
2. **Within a major**, a release MUST NOT remove or rename a function, change a parameter or return type, change
   the meaning of a function, a constant's value, a configuration key or an error code, or narrow what a function
   accepts. It MAY add functions, constants, configuration keys, enumerated values, metrics and log targets, and
   each such addition bumps the minor.
3. **Enumerated values may grow.** A caller MUST treat a value it does not know (a new column kind cannot appear
   within a tree major, but a log level, metric kind, or error code can) as "other", never as an error of the
   library.
4. **Adding a capability to an existing function** is done with a new function (`terndb_execute2` is never the
   name; the new function says what it adds, `terndb_execute_with_spans`) or a new configuration key or options
   setter. Options objects are the extension point for requests: a new field is a new setter, and the old
   functions keep their meaning.
5. **Exports.** The shared library MUST export exactly the functions in `terndb.h` and nothing else; every other
   symbol is hidden. On ELF the symbols carry the version node `TERNDB_1` (a minor's additions go in `TERNDB_1.<m>`
   inheriting the previous), and the soname is `libterndb.so.1`. On Mach-O the install name is
   `libterndb.1.dylib`, `compatibility_version` `1.0.0` and `current_version` `1.<minor>.0`. A static archive
   `libterndb.a` exports the same names.
6. **Deprecation**: a function to be removed at the next major is marked in the header with
   `TERNDB_DEPRECATED("use …")` (an attribute macro the header defines) for at least one minor before the major.
7. **A new major** is a new soname and a new header; a process MAY not load two majors at once.
8. The ABI version and the supported tree versions are independent: the library accepts envelopes of the tree
   majors [QT] §13.3 allows, and results use the kinds of the envelope's tree version (§8.3 rule 9).

## 14. sysl requirements

`libterndb` is written in sysl. The 0.11.0 survey (`docs/phase0/sysl-survey/FINDINGS.md` row 9,
`p09-export/`) showed that an exported C API with handles and a callback works, but not this one. The items below
go to the sysl session; the ABI above is not to be bent around them.

*Status at sysl 0.13.5:* items 1 to 6 and 9 are met, probed in `docs/phase0/sysl-survey/p16-capi013/` (`into_raw`/`from_raw`,
`void *`, `const T *`, `size_t`, array parameters, `#define` from an exported `const`, `--header-guard`, `build-c --shared`
with exactly the `@export` symbols). Item 7 is stated in sysl's `errors.md` (a trap aborts, never unwinds); items 8 and 10
were not re-probed. See `FINDINGS.md` § "Status at sysl 0.13.5". The text below is the requirement as written.

1. **Hand C a counted `&T` and take it back** (`into_raw` / `from_raw` or an exported `&T` parameter and return).
   0.11.0 has no way to keep a reference-counted object alive while only C holds it, so the probe kept every
   handle in a module registry `Buf[&Db]`, which leaks slots, needs a lock to be thread-safe, and cannot detect a
   freed handle. Every handle in §2.1 needs this.
2. **`void *` in exported headers.** `*u8` is printed as `uint8_t *`, so the allocator (`alloc` returns and `free`
   takes `void *`), the callback contexts and the log sink's `void *ctx` cannot be spelled as §12 spells them.
3. **`const` in exported headers.** Every accessor takes `const T *` and every input buffer is `const uint8_t *`
   or `const char *`; the header 0.11.0 writes has no way to say a pointer is read-only. *Not yet probed.*
4. **The C spellings of `usize`, `bool`, `f32`/`f64` and fixed arrays**: `size_t`, `bool`, `float`/`double`, and
   `const uint8_t digest[32]` / `uint8_t out[16]` parameters. *Not yet probed beyond `i32`, `i64` and pointers.*
5. **Header control**: the header's file name (`terndb.h`, not `libterndb.a.h`), its include guard (`TERNDB_H`;
   see survey suspect B3, a guard of `SYSL___H`), `#define` constants emitted from sysl `val`s, `extern "C"`
   guards, and an opaque `typedef struct x x;` for each exported handle type (the probe's `@export("dbx_db")
   opaque struct` does this; it must hold for every handle).
6. **A shared library with an exact export list**: a `build-lib` output that is a `.so`/`.dylib`, exports only the
   `@export` symbols with everything else hidden, takes an ELF version script and Mach-O install name and
   versions (§13.5), and can be checked with `nm -gU`.
7. **No unwinding into C.** A sysl panic or trap inside an exported function MUST abort the process with a message,
   never unwind through a C frame; the guarantee needs stating in the sysl reference and a test.
8. **Thread identity**: a cheap "is this an engine thread" check (a thread-local, or a thread id comparable from
   any thread) for §2.3 rule 4, and the debug handle checks of §2.4.
9. **A process-wide allocator hook**: a way for every sysl allocation the library makes — `Buf`, strings, closures
   — to go through `terndb_init`'s `alloc` and `free`, with size and alignment. *Not probed; likely absent.*
10. **A wake descriptor**: an `eventfd` / `EVFILT_USER` (or pipe) usable as `sysl.threads.Poller` input and
    signalled from another thread, for §6.2 rule 4. Probably expressible through externs today; wanted in the
    library.
11. The module must be a directory (0.11.0): the C ABI lives in `sh.sysl.terndb.capi/` as its own module. This is a
    layout fact, not a gap.

## 15. Conformance

### 15.1 Properties

**D** = checked only in a debug build; everything else in both builds.

1. Every function in §12 is exported, and no other symbol is (`nm -gU` against the header).
2. `terndb_init` with a newer minor or another major fails with `C1009`; any call but the four of §3 rule 1 before
   it fails with `C1008`.
3. With a counting allocator installed, every allocation is matched by a `free` with the same size and alignment,
   and the allocator sees no traffic after every handle has been freed and every database closed.
4. After a test frees everything it made, `terndb_debug_live_handles()` returns its value before the test.
5. (D) A call with a freed handle of every kind, and with a handle of the wrong kind, fails with `C1001` (or
   aborts, for functions without an error channel); a freed handle's address is never returned again.
6. Two threads calling into one connection at once: exactly one call proceeds and the other fails with `C1004`.
7. A sync call, `terndb_op_wait` or `terndb_queue_wait` from inside the log sink fails with `C1003` and does not
   block.
8. Every input buffer may be overwritten with garbage immediately after the call returns, sync or async, without
   changing the outcome.
9. A batch stays readable, octet for octet, after its result is freed, its connection closed and its database
   closed.
10. A pointer obtained from a batch (bytes, buffers, column strings, types) is valid until that batch is freed and
    unchanged meanwhile, under ASan.
11. `terndb_batch_bytes` of an embedded execution equals, octet for octet, the network mode's for the same query and
    data, and equals the golden vectors V15–V21 of [WIRE] §10.2.
12. `terndb_batch_decode` of each golden vector yields a batch whose accessors give the vector's values, and of each
    corrupted variant (wrong length, wrong padding, non-canonical union offsets) fails with `W1008`.
13. Walking every batch of a randomised nested result through the accessors yields the same values as an
    independent decoder of `terndb_batch_bytes` (the property of [WIRE] §10).
14. Each synchronous function and its async form produce the same outputs and errors for the same inputs (§6.2
    rule 1), on a table of every operation kind.
15. Each async operation appends its tag to its queue exactly once; the descriptor is readable exactly while the
    queue is non-empty; freeing a pending operation appends nothing.
16. Operations on one transaction complete in submission order; a pipelined begin, two executes and commit,
    submitted without waiting, succeed.
17. `terndb_op_cancel` of a running query yields `T1007`; of a commit it changes nothing.
18. With `prefetch_bytes = P`, the octets of produced-but-unpulled batches never exceed `P` plus one batch.
19. A provisional result of an implicit transaction whose commit fails ends in −1 after its batches, and the
    database shows none of its effects.
20. Every error is reported through `err` with the fields of [WIRE] §8.1, identical between embedded and network
    mode for the same failure (code, path, details, `txn_state`, `retryable`).
21. `err = NULL` is accepted by every fallible function; success never writes `*err`.
22. A closed database or connection makes every call on its dependants fail with `C1006`, and freeing them in any
    order is clean (property 4).
23. A memory-mode database with `snapshot` or `log` durability, closed, opens in disk mode with the same committed
    state, and back (§4.2 rule 4); `terndb_promote` gives the same.
24. A memory-mode clone is independent of its source in both directions, and costs time independent of the
    source's size (a 1 GiB source clones as fast as a 1 MiB one within a factor of 2).
25. A second open of a disk directory, in-process or from a child process, fails with `D4001`.
26. An unknown configuration key, a key not applicable to the call, and a malformed value each fail with `C1002`.
27. No caller code runs on a core thread: the log sink's thread id is the service thread's on every call, and no
    allocator call blocks a commit for longer than the allocator itself takes.
28. A release build and a debug build give identical results on the whole suite minus the **D** properties.

### 15.2 The C test driver

The driver is a C program in `tests/capi/`, built against the installed header and library only, run in both
builds and under ASan and TSan. Its tests:

| Test | Properties |
|---|---|
| `exports` — compare `nm -gU` with the header's declarations | 1 |
| `init_order`, `init_version` | 2 |
| `allocator_counting`, `allocator_null_fails_cleanly` (an allocator that fails on the n-th call, for every n up to the first query's total) | 3 |
| `leaks_every_kind` | 4 |
| `use_after_free_every_kind`, `wrong_kind_every_kind` (D) | 5 |
| `conn_concurrent_use` | 6 |
| `sink_reentry` | 7, 27 |
| `inputs_scribbled` | 8 |
| `batch_outlives_db` | 9 |
| `borrowed_pointers_asan` | 10 |
| `golden_vectors_embedded`, `golden_vectors_network` | 11 |
| `decode_vectors`, `decode_corrupt` | 12 |
| `random_nested_walk` (seeded, 10,000 results) | 13 |
| `sync_async_equivalence` | 14 |
| `queue_exactly_once`, `queue_fd_level`, `op_free_pending` | 15 |
| `pipeline_txn`, `pipeline_begin_fails` | 16 |
| `cancel_query`, `cancel_commit_ignored` | 17 |
| `prefetch_bound` | 18 |
| `implicit_commit_fails` | 19 |
| `errors_match_network`, `error_null_out` | 20, 21 |
| `close_order_permutations` (every order of freeing db, conn, txn, stmt, result, batch) | 22 |
| `promote_roundtrip`, `promote_function` | 23 |
| `clone_isolation`, `clone_constant_time` | 24 |
| `lock_in_process`, `lock_child_process` | 25 |
| `config_strict` | 26 |
| `ddl_through_execute` (§0 rule 4: create, insert, `AwaitJobs`) | — |
| `transaction_status_after_crash` (embedded: kill a child mid-commit, reopen, ask) | — |
| `event_loop_integration` (an `epoll`/`kqueue` loop driving 1,000 concurrent queries through one fd) | 15 |

## 16. Open questions

1. **A parameter builder.** Parameters are [WIRE] §4.2 bytes, which a C caller without an encoder must build by
   hand. Should the ABI add a builder (`terndb_params_new`, `_push_i64`, `_begin_record`, …), or leave encoding to
   language bindings and a small separate helper library?
2. **Windows.** `terndb_queue_fd` has no meaning there. Is Windows a target at all, and if so is the completion
   handle an I/O completion port, an event `HANDLE`, or both?
3. **Multi-process readers.** Every open takes the exclusive lock. Should a `read_only` open of a disk database
   from a second process be allowed (a shared lock, reading what the writer has checkpointed), as SQLite and
   LMDB allow?
4. **The code of a failed network connect.** A DNS, socket or TLS failure before the handshake has no registered
   code. A new `W` code (`W3005` `connect_failed`), or a `C` code? §12.1 rule 5 holds a placeholder.
5. **Per-database allocators.** The allocator is process-wide. A host embedding several databases with separate
   budgets would want one per database; is `memory_limit` enough?
6. **Debug checks in release builds.** Should handle generations be checked in release builds too (one load and
   compare per call), so that a use after free is always `C1001` rather than undefined?
7. **The bootstrap role.** §4.2 rule 2 creates a superuser `terndb` in a new database. That belongs in `ddl.md`
   §2.10; should it move there, and should a server's new database get the same role?
8. **Arrow C Data Interface export** (decision W12: later). When it comes, is it a function on a batch
   (`terndb_batch_to_arrow`) in a minor release, or a separate library?
