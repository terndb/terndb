# FINDINGS: what a database engine (TernDB) needs from sysl 0.11.0

Probed on aarch64-macos, every run through marshal, on 2026-11-14. The tally is 2 expressible, 6 expressible but unpleasant, 2 gaps, plus 2 compiler bugs and 1 suspected. Probe sources are in the `pNN-*` directories beside this file, and `run-all.zsh` reruns them.

| # | Need | Verdict | Evidence | What it would take |
|---|---|---|---|---|
| 1 | Atomics with ordering; a 64-bit atomic in a shared page | expressible | p01-atomics: `Atomic[u64]`, `[u32]` and `[u8]` fields in an `@align(16384)` struct held as `&sync Page`; `add(AcqRel)`, CAS, `or`, `load`, `store`, `swap`, `atomic_fence` all correct | nothing |
| 2 | Seqlock / optimistic read | unpleasant | p02-seqlock works (`torn 0`). Without a promise the writer is refused ("writes a field reached through a '&sync SeqPage' … write '@unchecked(Share)'"), and that promise covers the whole type. sync.md does not define a racy plain read. There is no bulk relaxed copy | a decision on racy-read semantics; per-field `@unchecked(Share)` or a `SeqLock`/`Racy[T]` |
| 3 | Page memory and pointer swizzling | expressible (friction) | p03-page: `@packed` header via `ptr_cast`, a slot directory, `str_alias` key views, a tagged swip. `sizeof` and `.len` are `usize` while offsets are `int`, so every offset needs a conversion | a call on mixing `int` and `usize` |
| 4 | One thread per core, pinning, loops, queue | unpleasant (pinning) | p04-threads: `cpu_count`, `scope` workers, `Poller`, `Channel` all fine. No affinity call exists; the Linux `pthread_setaffinity_np` extern was not compile-checked | `sysl.threads.pin(core)` |
| 5 | I/O | unpleasant; **io_uring is a gap** | p05-io: `read_at`, `write_at` and `sync` work. fdatasync, F_FULLFSYNC, F_NOCACHE/O_DIRECT and a writable shared mmap with msync are missing from the library (they work through externs). No io_uring binding exists anywhere | `sync_data`, `full_sync`, uncached open and a writable map in `sysl.fs`; a `uring` package |
| 6 | SIMD | **gap** | p06-simd: compare, `.all()`, select and sum work. Mask to bits is refused ("cannot convert <16>bool to ushort"). No gather, no compress; lane indices must be constant | `mask.bits()`, `gather`, `compress` |
| 7 | Run-time code generation | unpleasant, not a gap | p07-jit: about 30 externs to LLVM's C API, then ORC LLJIT; `(a+b)*a` compiled and called. Target initialisation is per architecture (a static inline in C) | a `sysl-lang/llvm` package |
| 8 | Nested values with no box per element | unpleasant + **B1** | p08-nested: elements stored inline, but `for x in items` emits `arc.copy` and `arc.dispose` per element. The `ref` alternative is refused (B1) | a borrowing `for`; the B1 fix |
| 9 | C ABI with handles and a callback | unpleasant | p09-export: works (`seen 3 acc 6`). No `into_raw`/`from_raw` for `&T`, so handles live in a registry `Buf[&Db]`. `*u8` is spelled `uint8_t *`, not `void *`. The module must be a directory | handing C a counted `&T`; a `void *` spelling |
| 10 | CRC32C, fast hash | **gap** + **B2** | p10-hash: only CRC-32 (IEEE). No CRC intrinsic admitted. Only FNV-1a byte at a time, nothing seeded or fast | `sysl.checksum.crc32c` with a hardware path; a fast seeded hash |

## Bugs
- **B1:** a `ref` into a `Buf[V]` is refused across a call that only reads, once the callee loops with `for x in items`. memory.md § "The one rule" says calls whose bodies only read stay open. Reduction: `p08-nested/ref-for/main.sysl`. Error: "'x' is a 'ref' standing on storage held in a 'Buf[V]', and 'w' may write a 'Buf[V]'…". With `items.len()` in place of the loop it compiles (`p08-nested/ref-flat`).
- **B2:** on aarch64, an inline-assembly `u32` operand is printed as an `x` register, and there is no way to ask for `w`. `"crc32cx {out}, {crc}, {word}"` fails inside clang ("invalid operand for instruction crc32cx x8, x0, x1"); `{out:w}` is "names an operand this assembly does not declare".
- **B3 (suspected):** the header `build-c` writes for a root-package library is guarded `SYSL___H` (an empty module part), so two such headers in one C file would hide the second.

## Decisions that need the user
1. What a racy plain read means: a defined but possibly torn value, or undefined.
2. Per-field `@unchecked(Share)`, or a library `SeqLock`/`Racy[T]`.
3. A borrowing `for` over a `Buf`.
4. Handing C a counted `&T` (`into_raw`/`from_raw`).
5. A `void *` spelling in exported headers.
6. `mask.bits()`, `gather` and `compress`: language or `sysl.vectors`.
7. `int` vs `usize` in layout arithmetic.
8. New packages (`uring`, `llvm`) and library additions (`sysl.fs` sync, uncached and map; `threads.pin`; `crc32c`; a fast hash).

## Status at sysl 0.13.5 (2026-11-24)

Rechecked against the library docs and re-probed on aarch64-macos, every run through marshal. New probes: `p11-simd013`, `p12-hash013`, `p13-io013`, `p14-text013`, `p15-misc013` (`rerun-old.zsh` reruns the B1 and seqlock probes), `p16-capi013` (`run.zsh` builds an archive and a shared library and runs a C driver on each). The rows above keep the 0.11.0 wording; this table is the current state.

| # | Need | At 0.13.5 | Evidence |
|---|---|---|---|
| 1 | Atomics | closed (was already expressible) | unchanged |
| 2 | Seqlock / optimistic read | closed | `sysl.sync.SeqLock[T]` (read retries, reader writes nothing) and `load_relaxed`/`store_relaxed` (a `Plain` value copied word by word with relaxed atomics) are in the library (`library/sync.md`); p15-misc013 `seqlock 1 2`. A plain write through `&sync` is still refused (p02-seqlock/refused), so a page-sized racy copy goes through `load_relaxed`, and `T` must be `Plain` |
| 3 | Page memory, `int` vs `usize` | unchanged: friction remains | not re-probed; no doc change found |
| 4 | Thread pinning | still open | no affinity call in `sysl.threads` or `sysl.cpu` (api/ and library/ docs searched); `cpu_count`, `scope`, `Poller`, `Channel` as before |
| 5 | I/O: durability, uncached, writable map | closed | `File.sync_data`, `File.full_sync` (F_FULLFSYNC on macOS), `open_uncached` (O_DIRECT on Linux, which wants 4096-aligned buffer, offset and length), `File.map_mut` with `MappedMut.flush`/`flush_async` (p13-io013: all succeeded, a store through the map read back through `open_uncached`) |
| 5 | I/O: io_uring | **still open** | no `uring` package or repo under `~/dev/sysl-lang/`, no mention in the sysl docs. `sysl.io.poll` (kqueue/epoll) is the readiness API that exists |
| 6 | SIMD mask, lookup, gather, compress | closed | `mask.bits()` (a `<16>bool` is a `u16`; lane 3 and 15 give 32776), `table.lookup(idx)` (run-time lane picks, out of range gives 0, `tbl`/`pshufb`), `xs.gather(idx)` (traps like `xs[idx[k]]`), `v.compress(m)` (packs kept lanes low, zero fill). No hardware compress or gather instruction is used by default: each is a lane move per lane. p11-simd013. Still absent: a masked load or store (`reference/vectors.md` § "What a vector does not have") |
| 7 | JIT | unchanged: not a gap | not re-probed |
| 8 | Borrowing `for` | B1 fixed | `p08-nested/ref-for` compiles and prints `4`. The `arc.copy` per element of `for x in items` was not re-measured |
| 9 | C ABI | closed for every item of `spec/c-api.md` §14 probed | p16-capi013: `Buf`-free handles through `into_raw()` / `from_raw` (`sysl.raw`); `*unit` prints `void *`, `*const T` prints `const T *`, `usize` prints `size_t`, `*[N]T` parameter prints `T name[N]`; `@export` on a `const` emits a `#define`; `--header-guard TERNDB_H` names the guard (B3 gone); `build-c --shared` makes a `.dylib`/`.so` whose `nm -gU` lists exactly the `@export` symbols (3 of 3 here), and a C driver ran against both archive and library. Unwinding, allocator size/alignment and the wake descriptor were not re-probed; `reference/errors.md` states a trap aborts with no unwinding |
| 10 | CRC32C, fast hash | closed | `sysl.checksum.crc32c`, incremental `new_crc32c()`/`update`/`finish` (check value 0xe3069283; the instruction on AArch64 `crc` and SSE 4.2, a slice-by-8 table elsewhere, same answer); `hash64(bytes, seed = 0)` is rapidhash V3, stable across targets and releases, so it may be persisted. p12-hash013. B2 (inline assembly operand width) no longer matters for CRC |
| new | UTF-8 validation | closed | `sysl.text.is_utf8(bytes)` and `validate_utf8(bytes) -> Option[int]` (index of the first bad byte), no string built. p14-text013 |

Open at 0.13.5: io_uring (no package), thread pinning, a masked vector load/store, and the `int`/`usize` conversion friction. Everything else the engine design waits on is available.
