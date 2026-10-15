# mips32 big-endian O32 (mips-linux-gnu), tests under qemu-mips.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR mips)
set(COROCC_ABI mips_o32)                     # authoritative backend selector
set(CMAKE_C_COMPILER mips-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER mips-linux-gnu-g++)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_FIND_ROOT_PATH /usr/mips-linux-gnu)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-mips;-L;/usr/mips-linux-gnu)
