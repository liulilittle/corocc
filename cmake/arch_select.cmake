# Appends architecture-specific assembly sources to `corocc`.
if (MSVC AND (CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64"))
    # Windows x64: the PE/MASM context-switch backend (Boost.Context ms_pe,
    # vendored in src/arch/x86_64_ms.asm). MinGW and non-x64 Windows targets
    # are outside the M9 scope (MSVC x64 only).
    enable_language(ASM_MASM)
    # The directory-level /W4 /WX are C/C++ warning flags; ml64 rejects an
    # invalid /W value with A4008 and /WX then turns that into a hard error.
    # MASM has its own warning model, so the ASM_MASM compile rule drops the
    # generic COMPILE_OPTIONS list; assembler flags can still be injected via
    # CMAKE_ASM_MASM_FLAGS.
    set(CMAKE_ASM_MASM_COMPILE_OBJECT
        "<CMAKE_ASM_MASM_COMPILER> <DEFINES> ${CMAKE_ASM_MASM_FLAGS} /c /Fo<OBJECT> <SOURCE>")
    target_sources(corocc PRIVATE src/arch/x86_64_ms.asm)
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_sources(corocc PRIVATE src/arch/x86_64_sysv_elf_gas.S)
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    target_sources(corocc PRIVATE src/arch/arm64_aapcs_elf_gas.S)
else()
    message(FATAL_ERROR "corocc: unsupported arch ${CMAKE_SYSTEM_PROCESSOR}")
endif()
enable_language(ASM)
