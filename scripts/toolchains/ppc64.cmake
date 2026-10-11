# ppc64 big-endian ELFv1 (powerpc64-linux-gnu), tests under qemu-ppc64.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR powerpc64)
set(CMAKE_C_COMPILER powerpc64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER powerpc64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/powerpc64-linux-gnu)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-ppc64;-L;/usr/powerpc64-linux-gnu)
