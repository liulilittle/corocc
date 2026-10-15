# ppc64 big-endian ELFv1 (powerpc64-linux-gnu), tests under qemu-ppc64.
# The vendored backend (ppc64_sysv_elf_gas.S) carries both the ELFv1 and the
# ELFv2 (_CALL_ELF == 2) code paths.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR powerpc64)
set(COROCC_ABI ppc64)                        # authoritative backend selector
set(CMAKE_C_COMPILER powerpc64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER powerpc64-linux-gnu-g++)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_FIND_ROOT_PATH /usr/powerpc64-linux-gnu)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-ppc64;-L;/usr/powerpc64-linux-gnu)
