# Third-Party Software

This project (`corocc`) incorporates the following third-party components.
The origin, version, and license of each are recorded below.

## Boost.Context

- **Vendored content:** the context-switch assembly sources under `src/arch/`
  (`x86_64_sysv_elf_gas.S`, and later `arm64_aapcs_elf_gas.S`), copied from
  Boost.Context and kept verbatim except for the renaming of the public
  symbols (`make_fcontext` -> `corocc_asm_make`,
  `jump_fcontext` -> `corocc_asm_jump`) so that `corocc` can coexist in a
  single binary with Boost. The original BSL-1.0 copyright headers are
  preserved in the files.
- **Upstream project:** Boost.Context (part of Boost).
- **Version:** 1.83.0.
- **License:** Boost Software License 1.0 (BSL-1.0).
  Full text: <https://www.boost.org/LICENSE_1_0.txt>
- **Compatibility:** BSL-1.0 is a permissive, OSI-approved license that is
  compatible with this project's MIT license.

The `corocc` project itself is licensed under the MIT License; see `LICENSE`
at the repository root.
