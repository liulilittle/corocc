# Canonical aarch64 toolchain file lives at toolchain/aarch64-linux-gnu.cmake
# (that is the path referenced by README.md and by the Git history). This
# forwarder exists so that every cross target can be discovered under
# scripts/toolchains/ by scripts/build-all.sh and scripts/prebuild-all.sh.
include("${CMAKE_CURRENT_LIST_DIR}/../../toolchain/aarch64-linux-gnu.cmake")
