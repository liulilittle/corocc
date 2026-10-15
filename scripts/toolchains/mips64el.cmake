# mips64 little-endian N64 (mips64el-linux-gnuabi64), tests under qemu-mips64el.
# See mips64.cmake for the versioned-driver note.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR mips64el)
set(COROCC_ABI mipsel_n64)                   # authoritative backend selector
set(CMAKE_C_COMPILER mips64el-linux-gnuabi64-gcc-13)
set(CMAKE_CXX_COMPILER mips64el-linux-gnuabi64-g++-13)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_FIND_ROOT_PATH /usr/mips64el-linux-gnuabi64)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-mips64el;-L;/usr/mips64el-linux-gnuabi64)
