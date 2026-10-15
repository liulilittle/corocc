# aarch64 big-endian (aarch64_be), tests under qemu-aarch64_be.
#
# Toolchain built from source (gcc 13.3 for aarch64_be-linux-gnu + glibc 2.39
# sysroot; the prebuilt-asset flow is documented in docs/platforms.md). The
# compiler is big-endian by target default and carries its own BE
# crtbegin*/libgcc/libatomic, which solves the packaged-LE-runtime startfile
# problem without -B/-nostdlib workarounds.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64_be)
set(COROCC_ABI aarch64_be)                   # authoritative backend selector
if (NOT EXISTS "/opt/corocc-sysroot/aarch64_be-linux-gnu/usr/include/stdio.h")
    message(FATAL_ERROR
        "corocc: aarch64_be toolchain is not built yet; fetch the prebuilt "
        "asset with scripts/ci/fetch-toolchain-assets.sh aarch64_be-linux-gnu "
        "(see docs/platforms.md)")
endif()
set(CMAKE_C_COMPILER /opt/corocc-toolchain/aarch64_be-linux-gnu/bin/aarch64_be-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER /opt/corocc-toolchain/aarch64_be-linux-gnu/bin/aarch64_be-linux-gnu-g++)
set(CMAKE_SYSROOT /opt/corocc-sysroot/aarch64_be-linux-gnu)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_FIND_ROOT_PATH /opt/corocc-sysroot/aarch64_be-linux-gnu)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-aarch64_be)
