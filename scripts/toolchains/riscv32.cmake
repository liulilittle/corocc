# riscv32 (ILP32D), tests under qemu-riscv32.
#
# Toolchain built from source (gcc 13.3 configured --target=riscv32-linux-gnu
# --with-arch=rv32gc --with-abi=ilp32d, plus a glibc 2.39 sysroot; the compiler
# defaults to rv32gc/ilp32d, so no -march/-mabi flags are needed). The
# prebuilt-asset flow is documented in docs/platforms.md.
# The riscv32 context-switch backend (src/arch/riscv32_sysv_elf_gas.S, adapted
# from the vendored riscv64 backend).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv32)
set(COROCC_ABI riscv32)                      # authoritative backend selector
if (NOT EXISTS "/opt/corocc-toolchain/riscv32-linux-gnu/sysroot/usr/include/stdio.h")
    message(FATAL_ERROR
        "corocc: riscv32 toolchain is not built yet; fetch the prebuilt asset "
        "with scripts/ci/fetch-toolchain-assets.sh riscv32-linux-gnu "
        "(see docs/platforms.md)")
endif()
set(CMAKE_C_COMPILER /opt/corocc-toolchain/riscv32-linux-gnu/bin/riscv32-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER /opt/corocc-toolchain/riscv32-linux-gnu/bin/riscv32-linux-gnu-g++)
set(CMAKE_SYSROOT /opt/corocc-toolchain/riscv32-linux-gnu/sysroot)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_FIND_ROOT_PATH /opt/corocc-toolchain/riscv32-linux-gnu/sysroot)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-riscv32)
