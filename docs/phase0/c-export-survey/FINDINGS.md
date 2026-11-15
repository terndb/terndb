# FINDINGS: what `libterndb`'s C ABI needs from sysl 0.11.0

These probes test the needs listed in `docs/spec/c-api.md` §14. The sysl session ran them on 2026-11-15, through
marshal, on aarch64-macos. Item numbers follow §14.

| # | Need | Verdict | Notes |
|---|---|---|---|
| 1 | Hand C a counted `&T` and take it back (`into_raw`/`from_raw`) | gap | Waiting on a sysl decision. Today, handles live in a registry |
| 2 | A `void *` spelling in headers | gap | Waiting on a sysl decision |
| 3 | `const T *` in headers | gap | There is no read-only pointer type (p08-const) |
| 4 | The `size_t`, `bool` and `float`/`double` spellings | gap | `usize` is printed as `uint64_t`, not `size_t`, and a `c type`'s C name is lost (p01-types) |
| 5a | The header's file name | works | `build-c --header terndb.h` |
| 5b | The include guard | works (bug B3 fixed in sysl 0.11.2) | A `.` or bare-file build now gets a guard named after its source and a hash (`SYSL_A_26A2D3A3_H`, `SYSL_LIB_A85427C4_H`), so two such headers can be included together and the program links and runs (p03-guard) |
| 5c | `#define`s or enum constants from `val`s or enums | gap | p02-vals |
| 5d | `extern "C"` | works | Always emitted |
| 6 | A shared library with an exact export list and versions | gap, and a **bug** | There is no `--shared` mode; the user is deciding. `-o libterndb.dylib` silently writes an ar archive, and will be refused. By hand: `clang -dynamiclib -Wl,-all_load` plus `-exported_symbols_list`, since otherwise `_sysl_bounds_fail` and `_sysl_stdout_write` are exported too |
| 7 | A panic never unwinds into C | works, with a caveat | A trap is SIGTRAP. But `panic` calls `exit(1)` and prints to the host's stdout; sysl has been asked for stderr, abort and an embedder hook (p04-trap) |
| 8 | A cheap "engine thread?" check | works | An `@thread_local var` costs the same as C `_Thread_local` (p05-tls) |
| 9 | A process-wide allocator hook | works | The manifest's `allocator { alloc, free }` block routes every allocation (206/206 seen), and settable function pointers give a runtime hook. There is no realloc and no context pointer (p07-alloc) |
| 10 | A wake descriptor usable with `Poller` | works | `Poller.waker().wake()` from a C thread wakes `wait` (EVFILT_USER on macOS, eventfd on Linux). There is no `Poller.fd()` for nesting one poller in another (p06-wake) |

Gaps 1–4, 5c and 6 are with the user as sysl decisions. The C ABI implementation waits for them, under Rule 9: no
workarounds.
