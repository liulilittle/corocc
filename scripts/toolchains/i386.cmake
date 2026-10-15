# i386 (x86 32-bit, SysV ABI) built with the host gcc -m32, tests under
# qemu-i386. Requires the libc6-dev-i386 + lib32gcc-13-dev host packages.
#
# CMAKE_CXX_COMPILER is deliberately left at its default: no 32-bit C++
# runtime is installed here, and every target built for this ABI is C (the
# Boost coexistence test is C++ but native-only). The C and ASM compile steps
# get -m32 explicitly.
#
# -isystem /usr/include/x86_64-linux-gnu: gcc -m32 on amd64 does not add the
# x86 multiarch include dir, so <asm/errno.h> (pulled in through <errno.h> ->
# <linux/errno.h>) would be missing; the x86 asm/ kernel headers are shared
# between -m32 and -m64 (asm/unistd.h dispatches on __i386__).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)
set(COROCC_ABI i386_sysv)                    # authoritative backend selector
set(CMAKE_C_COMPILER gcc)
set(CMAKE_CXX_COMPILER g++)
set(CMAKE_C_FLAGS_INIT "-m32 -isystem /usr/include/x86_64-linux-gnu")
set(CMAKE_CXX_FLAGS_INIT "-m32 -isystem /usr/include/x86_64-linux-gnu")
set(CMAKE_ASM_FLAGS_INIT "-m32")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-i386)
