# Appends architecture-specific context-switch assembly sources to `corocc`.
#
# ABI selection:
#   Toolchain files set COROCC_ABI explicitly (authoritative). This keeps the
#   endianness and O32 / N32 / N64 / ELFv1-vs-ELFv2 choices unambiguous even
#   where CMAKE_SYSTEM_PROCESSOR collides (e.g. "mips" is used for both
#   endiannesses of the MIPS O32 toolchains).
#
#   COROCC_ABI     backend                            endianness
#   ------------   --------------------------------   -----------------------
#   x86_64_sysv    x86_64_sysv_elf_gas.S              -
#   x86_64_ms      x86_64_ms.asm (MSVC MASM, ml64)     -
#   i386_ms        i386_ms.asm (MSVC MASM, ml)         -
#   i386_sysv      i386_sysv_elf_gas.S                -
#   aarch64        arm64_aapcs_elf_gas.S              little
#   aarch64_be     arm64_aapcs_elf_gas.S              big
#   arm            arm_aapcs_elf_gas.S                little
#   armeb          arm_aapcs_elf_gas.S                big
#   mips_o32       mips32_o32_elf_gas.S               big
#   mipsel_o32     mips32_o32_elf_gas.S               little
#   mips_n64       mips64_n64_elf_gas.S               big
#   mipsel_n64     mips64_n64_elf_gas.S               little
#   mips_n32       mips32_n32_elf_gas.S               big    (hand-written)
#   mipsel_n32     mips32_n32_elf_gas.S               little (hand-written)
#   ppc32          ppc32_sysv_elf_gas.S               big
#   ppc64          ppc64_sysv_elf_gas.S (ELFv1)       big
#   ppc64le        ppc64_sysv_elf_gas.S (ELFv2)       little
#   s390x          s390x_sysv_elf_gas.S               big
#   riscv64        riscv64_sysv_elf_gas.S             little
#   riscv32        riscv32_sysv_elf_gas.S             little (adapted from riscv64)
#   loongarch64    loongarch64_sysv_elf_gas.S         little
#   sparc64        sparc64_sysv_elf_gas.S             big
#   x86_64_macho   x86_64_sysv_macho_gas.S            (macOS)
#   arm64_macho    arm64_aapcs_macho_gas.S            (macOS)
#
# When COROCC_ABI is unset (plain native builds), the historical heuristics
# are kept: MSVC pointer width selects the PE/MASM backend (ml64 at 8 bytes,
# ml at 4 bytes), x86_64 -> SysV ELF, aarch64 -> AAPCS; on Apple hosts the
# same x86_64/aarch64 probes select the Mach-O backends.

set(COROCC_ABI_x86_64_sysv "src/arch/x86_64_sysv_elf_gas.S")
set(COROCC_ABI_i386_sysv   "src/arch/i386_sysv_elf_gas.S")
set(COROCC_ABI_aarch64     "src/arch/arm64_aapcs_elf_gas.S")
set(COROCC_ABI_aarch64_be  "src/arch/arm64_aapcs_elf_gas.S")
set(COROCC_ABI_arm         "src/arch/arm_aapcs_elf_gas.S")
set(COROCC_ABI_armeb       "src/arch/arm_aapcs_elf_gas.S")
set(COROCC_ABI_mips_o32    "src/arch/mips32_o32_elf_gas.S")
set(COROCC_ABI_mipsel_o32  "src/arch/mips32_o32_elf_gas.S")
set(COROCC_ABI_mips_n64    "src/arch/mips64_n64_elf_gas.S")
set(COROCC_ABI_mipsel_n64  "src/arch/mips64_n64_elf_gas.S")
set(COROCC_ABI_mips_n32    "src/arch/mips32_n32_elf_gas.S")
set(COROCC_ABI_mipsel_n32  "src/arch/mips32_n32_elf_gas.S")
set(COROCC_ABI_ppc32       "src/arch/ppc32_sysv_elf_gas.S")
set(COROCC_ABI_ppc64       "src/arch/ppc64_sysv_elf_gas.S")
set(COROCC_ABI_ppc64le     "src/arch/ppc64_sysv_elf_gas.S")
set(COROCC_ABI_s390x       "src/arch/s390x_sysv_elf_gas.S")
set(COROCC_ABI_riscv64     "src/arch/riscv64_sysv_elf_gas.S")
set(COROCC_ABI_riscv32     "src/arch/riscv32_sysv_elf_gas.S")
set(COROCC_ABI_loongarch64 "src/arch/loongarch64_sysv_elf_gas.S")
set(COROCC_ABI_sparc64     "src/arch/sparc64_sysv_elf_gas.S")
set(COROCC_ABI_x86_64_macho "src/arch/x86_64_sysv_macho_gas.S")
set(COROCC_ABI_arm64_macho  "src/arch/arm64_aapcs_macho_gas.S")

function(corocc_use_ms_pe_backend asm_file)
    # Windows (MSVC): the PE/MASM context-switch backend (Boost.Context
    # ms_pe, vendored under src/arch/). x64 uses ml64 via x86_64_ms.asm;
    # the 32-bit target uses ml via i386_ms.asm. MinGW and other Windows
    # toolchains are outside the supported scope (MSVC only).
    enable_language(ASM_MASM)
    # The directory-level /W4 /WX are C/C++ warning flags; ml64/ml reject an
    # invalid /W value with A4008 and /WX then turns that into a hard error.
    # MASM has its own warning model, so the ASM_MASM compile rule drops the
    # generic COMPILE_OPTIONS list; assembler flags can still be injected via
    # CMAKE_ASM_MASM_FLAGS.
    #
    # 32-bit only: /safeseh. The x86 MSVC linker validates SEH by default
    # (SAFESEH), and an ml object without the marker is rejected with LNK2026
    # ("module is unsafe for SAFESEH image"). /safeseh assembles the module
    # under the "no exception handlers declared" contract, which is exactly
    # this backend's position: it manipulates the SEH chain through the TIB
    # at run time but defines no handler of its own. The flag lives in
    # CMAKE_ASM_MASM_FLAGS: the Visual Studio generator does not read the
    # CMAKE_ASM_MASM_COMPILE_OBJECT rule below, but it translates /safeseh
    # from these flags into the MASM item's UseSafeExceptionHandlers (verified
    # against the generated vcxproj), while the rule below expands
    # ${CMAKE_ASM_MASM_FLAGS} for the Ninja/Makefile generators.
    if (CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(CMAKE_ASM_MASM_FLAGS "${CMAKE_ASM_MASM_FLAGS} /safeseh" PARENT_SCOPE)
    endif()
    set(CMAKE_ASM_MASM_COMPILE_OBJECT
        "<CMAKE_ASM_MASM_COMPILER> <DEFINES> ${CMAKE_ASM_MASM_FLAGS} /c /Fo<OBJECT> <SOURCE>"
        PARENT_SCOPE)
    target_sources(corocc PRIVATE ${asm_file})
endfunction()

if (DEFINED COROCC_ABI)
    string(TOLOWER "${COROCC_ABI}" corocc_abi_lc)
    if (corocc_abi_lc STREQUAL "x86_64_ms")
        if (NOT MSVC)
            message(FATAL_ERROR "corocc: COROCC_ABI=x86_64_ms requires MSVC (MASM backend)")
        endif()
        corocc_use_ms_pe_backend(src/arch/x86_64_ms.asm)
    elseif (corocc_abi_lc STREQUAL "i386_ms")
        if (NOT MSVC)
            message(FATAL_ERROR "corocc: COROCC_ABI=i386_ms requires MSVC (MASM backend)")
        endif()
        corocc_use_ms_pe_backend(src/arch/i386_ms.asm)
    else()
        set(corocc_asm "${COROCC_ABI_${corocc_abi_lc}}")
        if (corocc_asm STREQUAL "")
            message(FATAL_ERROR "corocc: unknown COROCC_ABI '${COROCC_ABI}'")
        endif()
        if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${corocc_asm}")
            message(FATAL_ERROR
                "corocc: COROCC_ABI=${corocc_abi_lc} needs ${corocc_asm}, "
                "which is not vendored/implemented yet")
        endif()
        target_sources(corocc PRIVATE ${corocc_asm})
    endif()
elseif (MSVC AND (CMAKE_SIZEOF_VOID_P EQUAL 8))
    # Pointer width decides the backend: an x64 toolset builds the ml64
    # backend even when CMAKE_SYSTEM_PROCESSOR names the host, and a Win32
    # toolset (-A Win32) builds the 32-bit ml backend.
    corocc_use_ms_pe_backend(src/arch/x86_64_ms.asm)
elseif (MSVC AND (CMAKE_SIZEOF_VOID_P EQUAL 4))
    corocc_use_ms_pe_backend(src/arch/i386_ms.asm)
elseif (APPLE AND (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64"))
    target_sources(corocc PRIVATE src/arch/x86_64_sysv_macho_gas.S)
elseif (APPLE AND (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64"))
    target_sources(corocc PRIVATE src/arch/arm64_aapcs_macho_gas.S)
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_sources(corocc PRIVATE src/arch/x86_64_sysv_elf_gas.S)
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    target_sources(corocc PRIVATE src/arch/arm64_aapcs_elf_gas.S)
else()
    message(FATAL_ERROR "corocc: unsupported arch ${CMAKE_SYSTEM_PROCESSOR}")
endif()

# 64-bit lock-free atomics probe: 32-bit ABIs without inline 64-bit CAS
# (armv5 armel / mips O32 / ppc32) resolve the __atomic_*_8 builtins through
# libatomic. The probe links a small program exactly like the library is
# linked, so cross toolchains with or without libatomic are both handled.
if (NOT MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    include(CheckCSourceCompiles)
    check_c_source_compiles("
        #include <stdint.h>
        int main(void) {
            volatile uint64_t v = 0;
            uint64_t expected = 0;
            __atomic_compare_exchange_n(&v, &expected, (uint64_t)1, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
            return (int)(v == 1 ? 0 : 1);
        }" COROCC_HAVE_INLINE_U64_ATOMICS)
    if (NOT COROCC_HAVE_INLINE_U64_ATOMICS)
        target_link_libraries(corocc PUBLIC atomic)
    endif()
endif()

enable_language(ASM)
