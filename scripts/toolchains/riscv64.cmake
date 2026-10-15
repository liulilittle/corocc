# riscv64 little-endian (riscv64-linux-gnu), tests under qemu-riscv64.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv64)
set(COROCC_ABI riscv64)                      # authoritative backend selector
set(CMAKE_C_COMPILER riscv64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER riscv64-linux-gnu-g++)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_FIND_ROOT_PATH /usr/riscv64-linux-gnu)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-riscv64;-L;/usr/riscv64-linux-gnu)
