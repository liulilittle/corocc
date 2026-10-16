# sparc64 big-endian (sparc64-linux-gnu), tests under qemu-sparc64. Backend:
# src/arch/sparc64_sysv_elf_gas.S (vendored from Boost.Context; see
# THIRD-PARTY.md).
# Note: on Ubuntu noble the gcc-13-sparc64-linux-gnu package installs only the
# versioned driver (sparc64-linux-gnu-gcc-13); there is no unversioned symlink,
# so the versioned driver name is used here.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR sparc64)
set(COROCC_ABI sparc64)                      # authoritative backend selector
set(CMAKE_C_COMPILER sparc64-linux-gnu-gcc-13)
set(CMAKE_CXX_COMPILER sparc64-linux-gnu-g++-13)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_FIND_ROOT_PATH /usr/sparc64-linux-gnu)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-sparc64;-L;/usr/sparc64-linux-gnu)
