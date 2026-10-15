# riscv32 (ILP32D), tests under qemu-riscv32.
#
# Toolchain built from source by the W4/W5 pipeline (gcc 13.3 configured
# --target=riscv32-linux-gnu --with-arch=rv32gc --with-abi=ilp32d, plus a
# glibc 2.39 sysroot; the compiler defaults to rv32gc/ilp32d, so no -march/
# -mabi flags are needed). See .opencode/logs/isa/exec/w4s-build/.
# The riscv32 context-switch backend (src/arch/riscv32_sysv_elf_gas.S) is
# implemented (W6; adapted from the vendored riscv64 backend).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv32)
set(COROCC_ABI riscv32)                      # authoritative backend selector
if (NOT EXISTS "/opt/corocc-sysroot/riscv32-linux-gnu/usr/include/stdio.h")
    message(FATAL_ERROR
        "corocc: riscv32 toolchain is not built yet; run "
        ".opencode/logs/isa/exec/w4s-build/02_build_all.sh riscv32-linux-gnu (W5)")
endif()
set(CMAKE_C_COMPILER /opt/corocc-toolchain/riscv32-linux-gnu/bin/riscv32-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER /opt/corocc-toolchain/riscv32-linux-gnu/bin/riscv32-linux-gnu-g++)
set(CMAKE_SYSROOT /opt/corocc-sysroot/riscv32-linux-gnu)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_FIND_ROOT_PATH /opt/corocc-sysroot/riscv32-linux-gnu)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-riscv32)
