# Appends architecture-specific assembly sources to `corocc`.
if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_sources(corocc PRIVATE src/arch/x86_64_sysv_elf_gas.S)
else()
    message(FATAL_ERROR "corocc: unsupported arch ${CMAKE_SYSTEM_PROCESSOR}")
endif()
enable_language(ASM)