# Android NDK (r26+) cross toolchain.
#
# Point ANDROID_NDK at the extracted NDK root, e.g.
#   export ANDROID_NDK=/mnt/d/android/sdk/ndk/26.1.10909125
# and pick an ABI with ANDROID_ABI (arm64-v8a, armeabi-v7a, x86_64, x86).
#
# All four ABIs have a vendored context-switch backend; the mapping below is
# explicit because the NDK's CMAKE_SYSTEM_PROCESSOR values ("armv7-a",
# "i686", ...) are not part of the generic heuristics in
# cmake/arch_select.cmake (which still receives COROCC_ABI as the
# authoritative selector).
if(NOT DEFINED ENV{ANDROID_NDK})
    message(Fatal "ANDROID_NDK must point at an extracted Android NDK root")
endif()
set(ANDROID_NDK "$ENV{ANDROID_NDK}")
if(NOT DEFINED ANDROID_ABI)
    set(ANDROID_ABI "arm64-v8a")
endif()

if (ANDROID_ABI STREQUAL "arm64-v8a")
    set(COROCC_ABI aarch64)          # arm64_aapcs_elf_gas.S
elseif (ANDROID_ABI STREQUAL "armeabi-v7a")
    set(COROCC_ABI arm)              # arm_aapcs_elf_gas.S (VFP block active)
elseif (ANDROID_ABI STREQUAL "x86_64")
    set(COROCC_ABI x86_64_sysv)      # x86_64_sysv_elf_gas.S
elseif (ANDROID_ABI STREQUAL "x86")
    set(COROCC_ABI i386_sysv)        # i386_sysv_elf_gas.S
else()
    message(FATAL_ERROR "corocc: unsupported ANDROID_ABI '${ANDROID_ABI}'")
endif()

set(CMAKE_SYSTEM_NAME Android)
set(CMAKE_SYSTEM_VERSION 24)
set(CMAKE_ANDROID_NDK "${ANDROID_NDK}")
set(CMAKE_ANDROID_ARCH_ABI "${ANDROID_ABI}")
set(CMAKE_ANDROID_STL_TYPE c++_static)
