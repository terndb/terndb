# The C ABI's sysl requirements on sysl 0.11.8

`docs/spec/c-api.md` §14 lists what `libterndb` needs from sysl; it was written against the 0.11.0 survey (row 9 of
`../FINDINGS.md`). Each item below was re-checked on 0.11.8 by building and running the probe named beside it.

**Build recipe.** A probe is `<root>/<module>/<module>.sysl` (a module is a directory). Build with
`sysl build-c <root> -o <root>/lib.a --header <root>/x.h` (or `sysl emit-header <root>`), then
`clang driver/main.c <root>/lib.a -I<root> -lm`.

| § 14 item | Verdict | Probe |
|---|---|---|
| 1. Counted `&T` to C and back | missing | `i01a_export_ref`, `i01b_into_raw` |
| 2. `void *` in the header | missing | `i02_void` |
| 3. `const` in the header | missing | `i03_const` |
| 4. `size_t`, `bool`, `float`/`double`, fixed arrays | partly | `i04_arrays` |
| 5. Header control | partly | `i02_05_header`, `i05_define` |
| 6. Shared library with an exact export list | partly | `i07_08_rt` |
| 7. No unwinding into C | met (not yet stated or tested in sysl) | `i07_08_rt` |
| 8. Thread identity | met | `i07_08_rt` |
| 9. Process-wide allocator hook | partly | `i09_alloc` |
| 10. Wake descriptor | met | `i10_wake` |
| 11. The module is a directory | confirmed | — |

## Evidence

1. **Counted `&T`.** An exported `&T` parameter or result is refused: *"the exported 'open' returns &Db, which C has
   no way to spell … Hand out a raw pointer and keep the counted one on this side"*. `d.into_raw()` → *"type 'Db'
   has no method 'into_raw'"*; `from_raw(p)` → *"undefined function 'from_raw'"*. Wanted:
   `open() -> *Db { val d: &Db = Db(5); d.into_raw() }` and `close(p: *Db) { val d: &Db = from_raw(p) }`.
2. **`void *`.** `*void` → *"unknown type 'void'"*; `*u8` prints as `uint8_t *`. A C driver passing §12's
   `void *(*)(size_t)` allocator fails under clang (`-Wincompatible-function-pointer-types`); a data pointer only warns.
3. **`const`.** `*const u8` → *"identifier expected"*; `[]const u8` is refused for export (a slice is two C
   parameters). An exported `val` is emitted as `extern uint32_t const x;`, so the emitter can write `const`; there
   is no source form for a read-only pointee.
4. **Scalars.** `bool`, `f32` and `f64` print as `bool`, `float` and `double`. `usize` prints as `uint64_t`, never
   `size_t`; on macOS arm64 those are `unsigned long long` and `unsigned long`, so a C callback typed
   `bool (*)(size_t)` against the header's `bool (*)(uint64_t)` is a hard clang error. A by-value `[32]u8` parameter
   is refused (*"C decays an array parameter to a pointer … Wrap it in a struct"*); `*[32]u8` prints as
   `uint8_t (*digest)[32]`, so a caller passing `digest` gets an incompatible-pointer warning.
5. **Header control.** Met: the file name (`--header <path>`), `extern "C"` guards, an opaque
   `typedef struct terndb_db terndb_db;` for every handle type a signature names, and callback typedefs. Missing:
   the include guard is always `SYSL_<MODULE>_<hex>_H` (e.g. `SYSL_I02_05_HEADER_F25FA0BA_H`; survey suspect B3
   still open), and there is no `#define`: `@export const` is refused (*"A 'const' is folded into every use and has
   no storage to name"*) and an exported `val` is a symbol, not a macro.
6. **Shared library.** sysl has no shared-library output, version script or install-name option; `build-c` writes
   a `.a`. `clang -dynamiclib -Wl,-all_load lib.a -install_name @rpath/…` works, but `nm -gU` then shows the runtime
   hooks `sysl_bounds_fail` and `sysl_thread_self` beside the `@export` symbols (and `sysl_stdout_write` in an
   allocator build). Adding `-Wl,-exported_symbols_list` leaves exactly the `@export` symbols, and a C driver
   linked against that dylib runs. The ELF version script was not exercised (no Linux machine).
7. **No unwinding.** C calling a panicking export directly, and C → sysl → C callback → sysl panic, both printed
   *"index 5 out of bounds for length 2 (rt/rt.sysl:22:7)"* and exited 133 (SIGTRAP), from the archive and through
   the dylib. sysl's `errors.md` says a trap aborts without unwinding, but says nothing of exported functions or C
   frames and has no test, so §14's "state it and test it" is still open.
8. **Thread identity.** A `@thread_local var` (not `static var`) and `sysl.threads.current().id` work from a C
   `pthread` sysl did not start: main thread `is_engine=1`, the other `is_engine=0`, with distinct ids.
9. **Allocator hook.** The manifest's `allocator { alloc = "ha_alloc" free = "ha_free" }` can name `@export` shims
   that forward to host function pointers installed at run time; `ha_work(1000)` (a `Buf` of 1,000 pushes and a
   string) showed 11 allocations and 11 frees, 16,692 bytes, through the host. Limits: `free` takes no size; there is
   no alignment argument (over-aligned types use `sysl_alloc_aligned`/`sysl_free_aligned`); allocations made before
   the hook is installed come from libc, so a block freed afterwards crosses heaps; the shims are global exports
   that need hiding (item 6).
10. **Wake descriptor.** It lives in `sysl.io.poll`, not `sysl.threads`: `Poller.waker().wake()` from a `scope`
    thread ends a blocked `wait` (*"woke, events: 0"* after 0.37 s for a 0.3 s sleeper). A `sysl.process.pipe` read
    end registered with the poller is level-triggered. Warts: the pipe's write-end `File` cannot be captured by a
    closure shared across threads (its count is not atomic; the compiler asks for `&sync FileState`), so the probe
    used `into_raw_fd()` and `extern "write"`; `Waker` exposes no descriptor; there is no eventfd wrapper.
11. **Directory module.** A root-level `x.sysl` declaring `module x` is refused: *"a module is a directory, so the
    two must agree"*.

## Sent to the sysl session

No bugs; these gaps: `into_raw`/`from_raw` for `&T`; a `void *` header spelling; a read-only pointer that prints
`const T *`; `usize` as `size_t` and a pointer-to-array parameter as `uint8_t d[32]`; a settable include guard and
`#define` from a `val`/`const`; a shared-library output with an exact export list, install name, versions and
hidden runtime hooks (or archive plus linker flags documented as the supported road); "a trap in an exported
function aborts, never unwinds into C" stated in the reference with a test; a size on the allocator's `free` and an
alignment argument.
