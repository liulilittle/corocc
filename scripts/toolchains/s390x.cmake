# s390x big-endian (s390x-linux-gnu), tests under qemu-s390x.
#
# -fno-stack-protector: GCC's s390x TLS access materializes the thread
# pointer (access registers a0/a1) into a callee-saved GPR and may reuse it
# across calls within one function -- including the stack-protector canary
# check (clc <saved>,40(%rN)).  A coroutine context resumed by a different
# thread would then dereference the TLS slot of the thread that suspended it
# (and crash once that thread is gone).  Disabling the canary removes that
# compiler-generated TLS-pointer cache from the switch path; the full
# analysis is in .opencode/logs/isa/P11-recon.md and the s390x backend
# header note.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR s390x)
set(COROCC_ABI s390x)                        # authoritative backend selector
set(CMAKE_C_COMPILER s390x-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER s390x-linux-gnu-g++)
set(CMAKE_C_FLAGS_INIT "-fno-stack-protector")
set(CMAKE_CXX_FLAGS_INIT "-fno-stack-protector")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_FIND_ROOT_PATH /usr/s390x-linux-gnu)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-s390x;-L;/usr/s390x-linux-gnu)
