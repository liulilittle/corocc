# arm32 little-endian soft-float EABI (arm-linux-gnueabi), tests under
# qemu-arm. Backend: the same vendored arm_aapcs_elf_gas.S as the armhf and
# armeb sites; for this soft-float ABI (__SOFTFP__ defined, __ARM_ARCH 5
# default) the VFP save/restore block in the backend is compiled out by its
# own guard, so the context frame matches the soft-float C ABI.
# Note: on Ubuntu noble the gcc-13-arm-linux-gnueabi packages install only
# the versioned driver (arm-linux-gnueabi-gcc-13); the versioned driver name
# is used here (same convention as the mips64/loongarch64/sparc64 rows).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(COROCC_ABI arm)                          # authoritative backend selector
set(CMAKE_C_COMPILER arm-linux-gnueabi-gcc-13)
set(CMAKE_CXX_COMPILER arm-linux-gnueabi-g++-13)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabi)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-arm;-L;/usr/arm-linux-gnueabi)
