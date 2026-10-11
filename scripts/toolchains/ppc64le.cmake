# ppc64 little-endian ELFv2 (powerpc64le-linux-gnu), tests under qemu-ppc64le.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR powerpc64le)
set(CMAKE_C_COMPILER powerpc64le-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER powerpc64le-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/powerpc64le-linux-gnu)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-ppc64le;-L;/usr/powerpc64le-linux-gnu)
