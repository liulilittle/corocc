# ABI — the binary interface contract

> [中文](ABI.zh-CN.md) | English

This document is the application binary interface (ABI) contract of corocc:
what a compiled consumer may rely on, how the layout of every complete
public type is pinned, and which source changes are compatible with it. The
per-function contracts live in the API references
([coro.md](api/coro.md), [coro_l0.md](api/coro_l0.md),
[coro_rt.md](api/coro_rt.md), [coro_sched.md](api/coro_sched.md),
[coro_scope.md](api/coro_scope.md), [coro_cpp.md](api/coro_cpp.md),
[coro_rt_driver.md](api/coro_rt_driver.md)); this
page is the cross-cutting binary contract plus the measured layout tables.

## What the ABI covers

| Surface | Pinned by this document |
| --- | --- |
| Public functions | Name, signature, calling convention, return type (`extern "C"`). |
| Public enumerations | `coro_status`, `coro_state`, `coro_driver_flags`: numeric values frozen. |
| Complete public structures | `coro_stack`, `coro_ctx`, `coro_attr_t`, `coro_allocator_t`, `coro_rt_mutex`, `coro_rt_cond`, `coro_rt_sem`, `coro_rt_rwlock`, `coro_rt_channel`, `coro_rt_scope_t`, `coro_rt_driver_ops`: `sizeof`, alignment, field offsets. |
| Opaque handles | `coro_t`, `coro_rt_t`, `coro_sched_t`, `coro_rt_driver_t`: pointer width only — the layout behind them is private and may change freely. |
| Symbol visibility | The `COROCC_API` macro (`coro_status.h`): the single platform conditional in the public headers, symbol decoration only, never layout. |
| Constants | `CORO_DEFAULT_STACK_SIZE`, `CORO_LOCAL_SLOTS`, `CORO_TOKEN_NONE`, `CORO_TOKEN_DEAD`, `CORO_F_NONE`. |

Everything not listed is implementation detail. In particular, the public
headers contain **no layout-affecting conditional compilation**: the only
`_WIN32` conditional in `include/corocc/` is the `COROCC_API` macro in
`coro_status.h`.

The runtime driver layer (`coro_rt_driver.h`) is a **pure addition**: it
introduces the complete `coro_rt_driver_ops` structure (whose leading
`struct_size` field tags the vtable for forward-compatible extension), the
`coro_driver_flags` enumeration, the opaque `coro_rt_driver_t` handle and new
functions, while `coro_rt.h`'s surface and the built-in backend are unchanged.
With no driver attached a runtime takes the built-in path bit-for-bit.

## Stability promise

Within one ABI major line (currently **1.x**):

- **Frozen:** every existing public symbol (function name and signature),
  every existing enumeration value, and every complete structure's `sizeof`,
  alignment and field offsets.
- **Frozen semantics:** the documented meaning of each status code and each
  entry point; the field *order* of the complete structures.
- **Allowed additions:** new functions, new opaque handle types, new
  complete types, and new enumeration members **appended after the existing
  ones** (never inserted). `coro_status` grows only at the tail, so a
  numeric comparison against an old value never changes meaning.
- **Forbidden in 1.x:** reordering, adding, removing or resizing fields of
  an existing complete structure; changing an existing enumeration value;
  changing a function signature, return type or calling convention;
  removing or renaming a symbol.

A change that cannot respect these rules is a **2.0** change: the ABI major
version is the package major version, and a 2.x release is a new, parallel
library — old and new binaries are not mixed within one ABI component.

Cross-component mixing: like any C library, all translation units in one
binary must agree on the headers they were compiled against. A binary that
links two different header versions of the same major line is a defect by
construction; within 1.x, source compatibility means a rebuild is always
sufficient.

## Pointer models and layout

All complete public structures are built exclusively from `void*` pointers,
`size_t`, `uint64_t` and 32-bit enumerations. That yields exactly these
distinct layouts:

- **LP64** (Linux and macOS, most Unix 64-bit): pointers, `size_t` and
  `long` are 8 bytes. macOS x86_64 and Apple Silicon arm64 are LP64, so the
  64-bit layout table below applies to them unchanged (like LLP64, and for
  the same reason: no public field uses `long`).
- **LLP64** (64-bit Windows, x64 and ARM64): pointers and `size_t` are
  8 bytes, `long` is
  4 bytes — no public field uses `long`, so **LLP64 is layout-identical to
  LP64**. The Windows ARM64 target is LLP64 as well, so it follows the same
  64-bit table (the CI windows-arm64 job runs the full suite on the ARM64
  host).
- **ILP32, 8-byte `uint64_t` alignment** (ARM EABI, MIPS o32, Win32
  MSVC): pointers and `size_t` are 4 bytes; `uint64_t` is 8 bytes aligned
  to 8. Measured on the armhf, mips o32 and Windows x86 targets; the
  remaining 32-bit ARM/MIPS/riscv targets follow the same alignment rule.
  MSVC's x86 default (`/Zp8`) keeps `__int64` 8-byte aligned, so Win32
  joins this table rather than the 4-byte one.
- **ILP32, 4-byte `uint64_t` alignment** (i386 SysV, Android x86):
  pointers and `size_t` are 4 bytes; `uint64_t` is 8 bytes aligned to 4.
  Applies to the i386 target and the Android x86 ABI. The four pointer-only
  structures (`coro_stack`, `coro_ctx`, `coro_attr_t`, `coro_allocator_t`)
  are identical to the other ILP32 variant; the six structures containing
  `uint64_t` are smaller.

The tables in the next sections are the *measured* values from the ABI
probe on every locally available platform, all consistent:

| Site | Pointer model | Result |
| --- | --- | --- |
| Windows x64, MSVC (`cl`, VS2022 17.x, `/W4 /WX`) | LLP64 | matches the LP64 table |
| Windows x86 (Win32), MSVC (`cl`, VS2022 17.x, `/W4 /WX`) | ILP32, 8-byte `uint64_t` | matches the first ILP32 table |
| Linux x86-64, GCC 13 | LP64 | baseline |
| Linux aarch64, GCC 13 / `qemu-aarch64` | LP64 | identical |
| Linux riscv64, GCC 13 / `qemu-riscv64` | LP64 | identical |
| Linux ppc64le, GCC 13 / `qemu-ppc64le` | LP64 | identical |
| Linux s390x (big-endian), GCC 13 / `qemu-s390x` | LP64 | identical |
| Linux armhf (armv7 ABI), GCC 13 / `qemu-arm` | ILP32, 8-byte `uint64_t` | matches the first ILP32 table |
| Linux mips (o32, big-endian), GCC / `qemu-mips` | ILP32, 8-byte `uint64_t` | identical to armhf |
| Linux i386 (x86 32-bit SysV), GCC 13 | ILP32, 4-byte `uint64_t` | matches the i386 table |
| Android NDK clang 18 (r27d), `armeabi-v7a` | ILP32, 8-byte `uint64_t` | matches the first ILP32 table (record-layout dump) |
| Android NDK clang 18 (r27d), `x86` | ILP32, 4-byte `uint64_t` | matches the i386 table (record-layout dump) |

Byte order does not change any offset: the layout rules above are
endian-independent; a binary is still only exchangeable with the **same**
ABI (architecture, endianness, width, ABI variant and C library). The
big-endian sites above exist to prove that claim, not to make binaries
interchangeable.

### 64-bit layout (LP64 and LLP64, identical)

| Type | `sizeof` | `alignof` | Field offsets |
| --- | --- | --- | --- |
| `coro_stack` | 40 | 8 | `base` 0, `total` 8, `lo` 16, `usable` 24, `guard_bytes` 32 |
| `coro_ctx` | 48 | 8 | `fctx` 0, `stack` 8, `entry` 16, `arg` 24, `back` 32, `asan_fake` 40 |
| `coro_attr_t` | 32 | 8 | `stack_size` 0, `name` 8, `flags` 16, `allocator` 24 |
| `coro_allocator_t` | 32 | 8 | `alloc_stack` 0, `free_stack` 8, `alloc_handle` 16, `free_handle` 24 |
| `coro_rt_mutex_t` | 16 | 8 | `locked` 0, `waitq` 8 |
| `coro_rt_cond_t` | 8 | 8 | `waitq` 0 |
| `coro_rt_sem_t` | 16 | 8 | `count` 0, `waitq` 8 |
| `coro_rt_rwlock_t` | 24 | 8 | `state` 0, `rd_waitq` 8, `wr_waitq` 16 |
| `coro_rt_channel_t` | 64 | 8 | `slots` 0, `cap` 8, `len` 16, `head` 24, `limit` 32, `closed` 40, `sendq` 48, `recvq` 56 |
| `coro_rt_scope_t` | 88 | 8 | `state` 0, `rt` 8, `children` 16, `nchildren` 24, `cap` 32, `live` 40, `status` 48, `waitq` 56, `first_done` 64, `first_value` 72, `timer_fired` 80 |

### 32-bit layouts (ILP32)

The pointer-only structures are identical in both ILP32 variants:

| Type | `sizeof` | `alignof` | Field offsets |
| --- | --- | --- | --- |
| `coro_stack` | 20 | 4 | `base` 0, `total` 4, `lo` 8, `usable` 12, `guard_bytes` 16 |
| `coro_ctx` | 24 | 4 | `fctx` 0, `stack` 4, `entry` 8, `arg` 12, `back` 16, `asan_fake` 20 |
| `coro_attr_t` | 16 | 4 | `stack_size` 0, `name` 4, `flags` 8, `allocator` 12 |
| `coro_allocator_t` | 16 | 4 | `alloc_stack` 0, `free_stack` 4, `alloc_handle` 8, `free_handle` 12 |

Structures containing `uint64_t`, **8-byte `uint64_t` alignment**
(measured: armhf, mips o32):

| Type | `sizeof` | `alignof` | Field offsets |
| --- | --- | --- | --- |
| `coro_rt_mutex_t` | 16 | 8 | `locked` 0, `waitq` 8 |
| `coro_rt_cond_t` | 4 | 4 | `waitq` 0 |
| `coro_rt_sem_t` | 16 | 8 | `count` 0, `waitq` 8 |
| `coro_rt_rwlock_t` | 16 | 8 | `state` 0, `rd_waitq` 8, `wr_waitq` 12 |
| `coro_rt_channel_t` | 40 | 8 | `slots` 0, `cap` 4, `len` 8, `head` 12, `limit` 16, `closed` 24, `sendq` 32, `recvq` 36 |
| `coro_rt_scope_t` | 72 | 8 | `state` 0, `rt` 8, `children` 12, `nchildren` 16, `cap` 20, `live` 24, `status` 32, `waitq` 40, `first_done` 48, `first_value` 56, `timer_fired` 64 |

Structures containing `uint64_t`, **4-byte `uint64_t` alignment**
(measured: i386 SysV):

| Type | `sizeof` | `alignof` | Field offsets |
| --- | --- | --- | --- |
| `coro_rt_mutex_t` | 12 | 4 | `locked` 0, `waitq` 8 |
| `coro_rt_cond_t` | 4 | 4 | `waitq` 0 |
| `coro_rt_sem_t` | 12 | 4 | `count` 0, `waitq` 8 |
| `coro_rt_rwlock_t` | 16 | 4 | `state` 0, `rd_waitq` 8, `wr_waitq` 12 |
| `coro_rt_channel_t` | 36 | 4 | `slots` 0, `cap` 4, `len` 8, `head` 12, `limit` 16, `closed` 20, `sendq` 28, `recvq` 32 |
| `coro_rt_scope_t` | 64 | 4 | `state` 0, `rt` 8, `children` 12, `nchildren` 16, `cap` 20, `live` 24, `status` 28, `waitq` 36, `first_done` 40, `first_value` 48, `timer_fired` 56 |

## Enumerations

`coro_status` (4 bytes on every supported ABI; values frozen):

| Value | Numeric | Returned for (summary; full table in [coro_l0.md](api/coro_l0.md)) |
| --- | --- | --- |
| `CORO_OK` | 0 | Success. |
| `CORO_EINVAL` | 1 | Invalid argument: NULL, uninitialized, bad size, out-of-range slot key. |
| `CORO_ENOMEM` | 2 | Allocation failure (handle, stack mapping, ring growth, worker start). |
| `CORO_EBUSY` | 3 | Busy target: running/destroy racing, full channel, undrained object, live children. |
| `CORO_ESTATE` | 4 | Wrong state: unlock without hold, spawn on a canceling/drained scope, double start. |
| `CORO_ECANCELED` | 5 | Cancellation observed at a safe point; `wait_for` deadline. |
| `CORO_EDESTROYED` | 6 | Target already finished/collected; wakeup queued for a gone thread. |
| `CORO_ESTALE` | 7 | Stale credential; or "must be called inside a runtime coroutine". |
| `CORO_EAGAIN` | 8 | Transient condition; non-blocking answer of `coro_join` on a live target. |
| `CORO_ECLOSED` | 9 | Channel closed (buffered items drain first). |

`coro_state` (4 bytes; values frozen): `CORO_NEW` 0, `CORO_READY` 1,
`CORO_RUNNING` 2, `CORO_SUSPENDED` 3, `CORO_DEAD` 4.

`coro_ctx_init` is the one entry point that does **not** return a
`coro_status`: it returns `int` (0 on success, negative `CORO_EINVAL` on a
bad argument). This is part of the frozen signature.

## Structure passing

Only `coro_attr_default()` returns a structure **by value** (`coro_attr_t`,
32 bytes on 64-bit / 16 on 32-bit); every other boundary uses pointers or
scalars. The by-value return follows the platform's standard structure
return rules (memory return for a structure of this size), so no special
handling is needed when mixing compilers on one platform.

## Symbol visibility and calling convention

- All public functions are declared in `extern "C"` blocks (when compiled as
  C++) and use the platform's default C calling convention; there are no
  variadic public functions, no `__stdcall`/`__fastcall` decorations, and no
  name mangling on the public surface.
- `COROCC_API` is the only platform conditional in the public headers:
  - Windows: `__declspec(dllexport)` when the library itself is built with
    `COROCC_BUILDING` defined, `__declspec(dllimport)` for consumers. A
    **static-library build defines `COROCC_BUILDING=1` for itself and its
    consumers** (the CMake target does this) so that no `__imp_` indirection
    remains.
  - ELF/Mach-O: `__attribute__((visibility("default")))`, which keeps the
    public symbols exported while the library is compiled with
    `-fvisibility=hidden`.
- The vendored context-switch assembly exports renamed symbols
  (`corocc_asm_make`, `corocc_asm_jump`) and is not part of the public
  surface.

## 32-bit link-interface note (atomic)

On 32-bit builds where the toolchain provides no inline 64-bit
compare-and-swap, the library relies on `libatomic` for a handful of
operations. The prebuilt CMake package therefore appends `atomic` to the
`corocc::corocc` link interface **only when the build-width probe found no
inline 64-bit CAS** (mirroring `cmake/arch_select.cmake`). Consumers of the
prebuilt archives need no manual change; this is documented because it is
part of the link ABI of those archives.

## Versioning

- The ABI major version tracks the package major version (`1.x` today).
- Frozen within 1.x: layouts, enumeration values, signatures, semantics
  (see Stability promise).
- The prebuilt CMake package reports its version through
  `corocc-config-version.cmake`; `find_package(corocc 1.0 CONFIG)` accepts
  any compatible 1.x.
- There is no promised binary compatibility *across* major versions.

## Android NDK and cross-toolchain notes

- **Android NDK (r26+).** Compile-and-link support through
  `scripts/toolchains/android-ndk.cmake`, which selects the toolchain and
  the target ABI (`ANDROID_ABI`: `arm64-v8a`, `armeabi-v7a`, `x86_64`,
  `x86`) with Android API level 24 and the static C++ STL. The CI compiles
  and links all four ABIs; on-device runs are outside the emulator matrix.
  Layouts, measured with the NDK clang record-layout dump: `arm64-v8a` and
  `x86_64` are LP64 (the 64-bit table); `armeabi-v7a` is ILP32 with 8-byte
  `uint64_t` (the first ILP32 table); `x86` is ILP32 with 4-byte
  `uint64_t` (the i386 table) -- Android x86 joins the 4-byte table, unlike
  Win32 MSVC x86 which is 8-byte aligned.
- **macOS (Mach-O).** Native builds on macOS x86_64 and arm64 use the
  vendored Mach-O backends (`x86_64_sysv_macho_gas.S`,
  `arm64_aapcs_macho_gas.S`); both are LP64 and follow the 64-bit layout
  table. The public headers keep the same `extern "C"` surface and
  `visibility("default")` spelling as ELF; the only Darwin-specific
  platform adjustments are internal (realtime condition-variable deadlines
  and `MAP_ANON`). Mach-O symbols carry the usual C underscore prefix in the
  artefacts (`_corocc_asm_make`), which is the platform's standard
  translation and does not change the documented C names.
- **Cross toolchains.** Cross builds use the toolchain files under
  `scripts/toolchains/` (one per ABI; see
  [platforms.md](platforms.md) for the ISA list), are linked statically for
  the emulator runs, and are exercised under `qemu-user` per ISA. The
  binaries produced for different targets are **not** interchangeable: each
  triple is its own ABI package (`libcorocc.a` per triple, see the prebuilt
  layout in [building.md](building.md)).
- **s390x-specific compiler flag.** The s390x toolchain compiles with
  `-fno-stack-protector` because gcc's s390x TLS access materializes the
  thread pointer into a callee-saved register that a cross-thread context
  switch would restore stale; the canary check would read it. This is a
  toolchain-level detail of the s390x ABI package, not a source-level
  change.
- **Static linking discipline.** Prebuilt cross archives are static
  (`libcorocc.a`); the runtime's thread support comes from the platform
  (`Threads::Threads` in the CMake package).

## How to verify the layout yourself

The tables above are reproducible from the public headers alone: compile a
one-file check with any supported compiler and run it. On a 64-bit platform
the following program must print `PASS` (the same assertions are compiled
into the library's own verification runs on every platform listed):

```c
#include <corocc/coro.h>
#include <corocc/coro_ctx.h>
#include <corocc/coro_rt.h>
#include <corocc/coro_scope.h>
#include <corocc/coro_stack.h>
#include <corocc/coro_status.h>
#include <stddef.h>
#include <stdio.h>

#define SA(name, cond) typedef char sa_##name[(cond) ? 1 : -1]
SA(ctx_sz, sizeof(coro_ctx) == 48)
SA(ctx_fctx, offsetof(coro_ctx, fctx) == 0)
SA(ctx_asan, offsetof(coro_ctx, asan_fake) == 40)
SA(stack_sz, sizeof(coro_stack) == 40)
SA(attr_sz, sizeof(coro_attr_t) == 32)
SA(chan_sz, sizeof(coro_rt_channel_t) == 64)
SA(scope_sz, sizeof(coro_rt_scope_t) == 88)
SA(status_ok0, CORO_OK == 0)
SA(status_closed9, CORO_ECLOSED == 9)

int main(void) {
    printf("sizeof(coro_ctx)=%u sizeof(coro_stack)=%u sizeof(coro_attr_t)=%u\n",
           (unsigned)sizeof(coro_ctx), (unsigned)sizeof(coro_stack),
           (unsigned)sizeof(coro_attr_t));
    printf("PASS\n");
    return 0;
}
```

```sh
cc -std=c99 -Wall -Wextra -Werror -Iinclude -o abi_check abi_check.c
./abi_check
```

For a 32-bit target, use the ILP32 table for your ABI variant (for example
`sizeof(coro_ctx) == 24` in both variants; `sizeof(coro_rt_channel_t)` is
40 with 8-byte `uint64_t` alignment and 36 with 4-byte alignment);
everything else in the program is unchanged. Big-endian targets run the
same source; the offsets and sizes do not depend on byte order.
