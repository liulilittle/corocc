# arm32 big-endian (arm-linux-gnueabi), tests under qemu-armeb.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armeb)
set(CMAKE_C_COMPILER arm-linux-gnueabi-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabi-g++)
set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabi)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-armeb;-L;/usr/arm-linux-gnueabi)
