# mips N32 big-endian (mips64-linux-gnuabi64 toolchain driven with -mabi=n32;
# the multilib support and the libc6-dev-mipsn32-cross sysroot provide the
# N32 libraries), tests under qemu-mipsn32.
# The N32 context-switch backend (src/arch/mips32_n32_elf_gas.S) is
# implemented (W6, hand-written; big/little endian via __MIPSEL__).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR mipsn32)
set(COROCC_ABI mips_n32)                     # authoritative backend selector
set(CMAKE_C_COMPILER mips64-linux-gnuabi64-gcc-13)
set(CMAKE_CXX_COMPILER mips64-linux-gnuabi64-g++-13)
set(CMAKE_C_FLAGS_INIT "-mabi=n32")
set(CMAKE_CXX_FLAGS_INIT "-mabi=n32")
set(CMAKE_ASM_FLAGS_INIT "-mabi=n32")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_FIND_ROOT_PATH /usr/mips64-linux-gnuabin32)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-mipsn32)
