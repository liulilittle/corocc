# Third-Party Software

This project (`corocc`) incorporates the following third-party components.
The origin, version, and license of each are recorded below.

## Boost.Context

- **Vendored content:** the context-switch assembly sources under `src/arch/`,
  copied from Boost.Context and kept verbatim except for the renaming of the
  public symbols (`make_fcontext` -> `corocc_asm_make`,
  `jump_fcontext` -> `corocc_asm_jump`) so that `corocc` can coexist in a
  single binary with Boost. The original BSL-1.0 copyright headers are
  preserved in the files:
  - `x86_64_sysv_elf_gas.S` (x86_64 SysV ELF)
  - `x86_64_ms.asm` (x86_64 PE/MASM, Windows MSVC)
  - `arm64_aapcs_elf_gas.S` (aarch64 AAPCS; little- and big-endian)
  - `i386_sysv_elf_gas.S` (i386 SysV ELF)
  - `arm_aapcs_elf_gas.S` (arm32 AAPCS; little- and big-endian)
  - `mips32_o32_elf_gas.S` (MIPS O32; little- and big-endian)
  - `mips64_n64_elf_gas.S` (MIPS N64; little- and big-endian)
  - `ppc32_sysv_elf_gas.S` (ppc32 SysV ELF, big-endian)
  - `ppc64_sysv_elf_gas.S` (ppc64 ELFv1 big-endian and ELFv2 little-endian)
  - `s390x_sysv_elf_gas.S` (s390x SysV ELF, big-endian)
  - `riscv64_sysv_elf_gas.S` (riscv64 SysV ELF)
  - `riscv32_sysv_elf_gas.S` (riscv32 SysV ELF, ILP32D) - a derivative
    adaptation of `riscv64_sysv_elf_gas.S` from the same Boost 1.83.0 release
    (LP64 register frame reworked to an ILP32D 0xa0-byte frame); the BSL-1.0
    attribution is kept in the file header.
- **Upstream project:** Boost.Context (part of Boost).
- **Version:** 1.83.0 (tag `boost-1.83.0`, commit
  `65b179db288d13da21447e4f430400338ce08ca2`; per-file upstream blob SHAs and
  local sha256 sums are recorded in `.opencode/logs/isa/exec/W1-ledger.txt`).
- **License:** Boost Software License 1.0 (BSL-1.0).
  Full text: <https://www.boost.org/LICENSE_1_0.txt>
- **Compatibility:** BSL-1.0 is a permissive, OSI-approved license that is
  compatible with this project's MIT license.

## Hand-written context-switch backends

The following backend is hand-written for `corocc` (it contains no
third-party code): `mips32_n32_elf_gas.S` (MIPS N32; big- and little-endian
selected via `__MIPSEL__`). It is a **derivative** re-implementation built on
the register-frame structure of the vendored `mips32_o32` / `mips64_n64`
backends, with the N32 specifics calibrated against the toolchain ABIs
(8-byte general-register slots, the even f20-f30 FP set, and the
single-register `coro_transfer` packing).

The `corocc` project itself is licensed under the MIT License; see `LICENSE`
at the repository root.
