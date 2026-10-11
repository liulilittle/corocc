# mips32 little-endian O32 (mipsel-linux-gnu), tests under qemu-mipsel.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR mips)
set(CMAKE_C_COMPILER mipsel-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER mipsel-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/mipsel-linux-gnu)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-mipsel;-L;/usr/mipsel-linux-gnu)
