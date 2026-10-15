#!/usr/bin/env bash
# Prebuild every corocc variant that can be built on this host and stage the
# artifacts for drop-in consumption:
#
#   dist/<triple>/lib/libcorocc.a      static library
#   dist/<triple>/include/corocc/*.h   public headers
#   dist/<triple>/README.txt           how the artifact was produced
#
# Targets whose backend is not vendored yet are reported as pending and
# skipped; the release/reference archs are always built.
#
# Environment:
#   LOG_DIR   log directory        (default ci-logs)
#   DIST_DIR  staging directory    (default dist)
#   WITH_CLANG also prebuild a native clang variant (default: yes)
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 2
LOG_DIR="${LOG_DIR:-$ROOT/ci-logs}"; mkdir -p "$LOG_DIR"
DIST_DIR="${DIST_DIR:-$ROOT/dist}"
WITH_CLANG="${WITH_CLANG:-yes}"

stage() { # triple, build_dir
    local triple="$1" bdir="$2"
    local out="$DIST_DIR/$triple"
    mkdir -p "$out/lib" "$out/include/corocc"
    cp "$bdir/libcorocc.a" "$out/lib/" || return 1
    # P1-2: *.h* also ships the header-only C++11 wrapper (coro.hpp).
    cp include/corocc/*.h* "$out/include/corocc/" || return 1
    {
        echo "corocc $triple"
        echo "built by: $(basename "$0")"
        echo "source:   $(cd "$ROOT" && git rev-parse --short HEAD 2>/dev/null || echo unknown)"
        echo "usage:    cc your.c -I$out/include -L$out/lib -lcorocc"
    } >"$out/README.txt"
    echo "staged $triple -> $out"
}

fail=0

# 1) native x86_64 (GCC)
echo "== native (gcc) =="
if scripts/build.sh >"$LOG_DIR/prebuild-native.log" 2>&1; then
    stage "x86_64-unknown-linux-gnu" "build" || fail=1
else
    echo "native build failed, see $LOG_DIR/prebuild-native.log"; fail=1
fi

# 2) native clang
if [ "$WITH_CLANG" = "yes" ] && command -v clang >/dev/null 2>&1; then
    echo "== native (clang) =="
    if CC=clang CXX=clang++ BUILD_DIR=build-clang scripts/build.sh \
            >"$LOG_DIR/prebuild-clang.log" 2>&1; then
        stage "x86_64-unknown-linux-gnu-clang" "build-clang" || fail=1
    else
        echo "clang build failed, see $LOG_DIR/prebuild-clang.log"; fail=1
    fi
fi

# 3) aarch64 cross (qemu-tested)
echo "== aarch64 cross =="
if scripts/build-all.sh >"$LOG_DIR/prebuild-cross.log" 2>&1; then
    if [ -f build-all/aarch64-linux-gnu/libcorocc.a ]; then
        stage "aarch64-unknown-linux-gnu" "build-all/aarch64-linux-gnu" || fail=1
    else
        echo "aarch64 library missing"; fail=1
    fi
else
    echo "cross build reported failures, see $LOG_DIR/prebuild-cross.log"; fail=1
fi

echo
echo "staged triples:"
(cd "$DIST_DIR" 2>/dev/null && ls -1 || true)
exit $fail
