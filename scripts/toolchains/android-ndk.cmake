# Android NDK (r26+) cross toolchain.
#
# Point ANDROID_NDK at the extracted NDK root, e.g.
#   export ANDROID_NDK=/mnt/d/android/sdk/ndk/26.1.10909125
# and pick an ABI with ANDROID_ABI (arm64-v8a, armeabi-v7a, x86_64, x86).
#
# This file only selects the toolchain; the corresponding ABI needs a
# vendored context-switch backend before it can actually build (see
# scripts/README.md for the per-ISA status table).
if(NOT DEFINED ENV{ANDROID_NDK})
    message(Fatal "ANDROID_NDK must point at an extracted Android NDK root")
endif()
set(ANDROID_NDK "$ENV{ANDROID_NDK}")
if(NOT DEFINED ANDROID_ABI)
    set(ANDROID_ABI "arm64-v8a")
endif()

set(CMAKE_SYSTEM_NAME Android)
set(CMAKE_SYSTEM_VERSION 24)
set(CMAKE_ANDROID_NDK "${ANDROID_NDK}")
set(CMAKE_ANDROID_ARCH_ABI "${ANDROID_ABI}")
set(CMAKE_ANDROID_STL_TYPE c++_static)
