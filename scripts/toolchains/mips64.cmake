# mips64 big-endian N64 (mips64-linux-gnuabi64), tests under qemu-mips64.
# Note: on Ubuntu noble the unversioned mips64-linux-gnuabi64-gcc symlink
# dangles at gcc-12 (not installed); the installed noble cross compiler is
# gcc-13, so the versioned driver name is used here. Whether the C++ suites
# are built is a build-segment gate in tests/CMakeLists (cross sites register
# the C-only segment), not a toolchain limit; the g++ driver below is declared
# because configure runs a C++ compiler check anyway.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR mips64)
set(COROCC_ABI mips_n64)                     # authoritative backend selector
set(CMAKE_C_COMPILER mips64-linux-gnuabi64-gcc-13)
set(CMAKE_CXX_COMPILER mips64-linux-gnuabi64-g++-13)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_FIND_ROOT_PATH /usr/mips64-linux-gnuabi64)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-mips64;-L;/usr/mips64-linux-gnuabi64)
