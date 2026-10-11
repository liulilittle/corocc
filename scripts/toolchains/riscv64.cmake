# riscv64 (riscv64-linux-gnu), tests under qemu-riscv64.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv64)
set(CMAKE_C_COMPILER riscv64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER riscv64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/riscv64-linux-gnu)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-riscv64;-L;/usr/riscv64-linux-gnu)
