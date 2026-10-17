# arm32 big-endian (armeb-linux-gnueabi ABI), tests under qemu-armeb.
#
# Toolchain built from source (gcc 13.3 for armeb-linux-gnueabi configured
# --with-endian=big, plus a glibc 2.39 sysroot). The local assembler wrapper
# defaults the shared arm binutils to -EB; the compiler carries its own BE
# crtbegin*/libgcc/libatomic/libstdc++. The prebuilt-asset flow is documented
# in docs/platforms.md.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armeb)
set(COROCC_ABI armeb)                        # authoritative backend selector
if (NOT EXISTS "/opt/corocc-toolchain/armeb-linux-gnueabi/sysroot/usr/include/stdio.h")
    message(FATAL_ERROR
        "corocc: armeb toolchain is not built yet; fetch the prebuilt asset "
        "with scripts/ci/fetch-toolchain-assets.sh armeb-linux-gnueabi "
        "(see docs/platforms.md)")
endif()
set(CMAKE_C_COMPILER /opt/corocc-toolchain/armeb-linux-gnueabi/bin/armeb-linux-gnueabi-gcc)
set(CMAKE_CXX_COMPILER /opt/corocc-toolchain/armeb-linux-gnueabi/bin/armeb-linux-gnueabi-g++)
set(CMAKE_SYSROOT /opt/corocc-toolchain/armeb-linux-gnueabi/sysroot)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_FIND_ROOT_PATH /opt/corocc-toolchain/armeb-linux-gnueabi/sysroot)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-armeb)
