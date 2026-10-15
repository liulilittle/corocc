# aarch64 little-endian (aarch64-linux-gnu), tests under qemu-aarch64.
# Canonical aarch64 toolchain file (referenced by README.md); the forwarder
# scripts/toolchains/aarch64.cmake includes this file.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(COROCC_ABI aarch64)                      # authoritative backend selector
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")   # matrix requirement: static links
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-aarch64;-L;/usr/aarch64-linux-gnu)
