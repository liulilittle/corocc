# loongarch64 little-endian (loongarch64-linux-gnu), tests under
# qemu-loongarch64. Backend: src/arch/loongarch64_sysv_elf_gas.S (vendored
# from Boost.Context; see THIRD-PARTY.md).
# Note: on Ubuntu noble the gcc-13-loongarch64-linux-gnu package installs only
# the versioned driver (loongarch64-linux-gnu-gcc-13); there is no unversioned
# symlink, so the versioned driver name is used here.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR loongarch64)
set(COROCC_ABI loongarch64)                  # authoritative backend selector
set(CMAKE_C_COMPILER loongarch64-linux-gnu-gcc-13)
set(CMAKE_CXX_COMPILER loongarch64-linux-gnu-g++-13)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_FIND_ROOT_PATH /usr/loongarch64-linux-gnu)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-loongarch64;-L;/usr/loongarch64-linux-gnu)
