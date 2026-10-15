# mips N32 little-endian (mips64el-linux-gnuabi64 toolchain driven with
# -mabi=n32), tests under qemu-mipsn32el. See mipsn32.cmake for details.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR mipsn32el)
set(COROCC_ABI mipsel_n32)                   # authoritative backend selector
set(CMAKE_C_COMPILER mips64el-linux-gnuabi64-gcc-13)
set(CMAKE_CXX_COMPILER mips64el-linux-gnuabi64-g++-13)
set(CMAKE_C_FLAGS_INIT "-mabi=n32")
set(CMAKE_CXX_FLAGS_INIT "-mabi=n32")
set(CMAKE_ASM_FLAGS_INIT "-mabi=n32")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_FIND_ROOT_PATH /usr/mips64el-linux-gnuabin32)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-mipsn32el)
