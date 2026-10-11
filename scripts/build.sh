#!/usr/bin/env bash
# Native configure + build + test wrapper for corocc.
#
# Honors the environment:
#   CC / CXX        compiler selection (e.g. CC=clang CXX=clang++)
#   BUILD_DIR       build directory (default: build)
#   CMAKE_ARGS      extra arguments forwarded to the CMake configure step
#   CMAKE_GENERATOR generator (default: Ninja)
#
# Usage:
#   scripts/build.sh [extra cmake args...]
#
# Examples:
#   scripts/build.sh
#   CC=clang CXX=clang++ scripts/build.sh
#   scripts/build.sh -DCMAKE_BUILD_TYPE=Release
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
GENERATOR="${CMAKE_GENERATOR:-Ninja}"

cd "$ROOT" || exit 2
echo "== corocc native build (dir=${BUILD_DIR}, generator=${GENERATOR}, CC=${CC:-default}) =="
cmake -S "$ROOT" -B "$BUILD_DIR" -G "$GENERATOR" ${CMAKE_ARGS:-} "$@" || exit 1
cmake --build "$BUILD_DIR" || exit 1
ctest --test-dir "$BUILD_DIR" --output-on-failure || exit 1

if [ -x "$BUILD_DIR/tests/bench_switch" ]; then
    echo "== benchmark =="
    "$BUILD_DIR/tests/bench_switch"
fi
echo "== native build OK =="
