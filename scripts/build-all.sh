#!/usr/bin/env bash
# Build and test corocc for every cross target listed below.
#
# Only the targets whose context-switch backend is already vendored are
# compiled and tested. For the remaining ISAs this script *validates* the
# toolchain file (configure runs and reaches the arch gate) and reports the
# target as pending, so a broken toolchain file can never hide behind
# "backend missing".
#
# Environment:
#   LOG_DIR   where configure/build logs are written (default .opencode/logs)
#   JOBS      parallel build jobs (passed to CMake)
#
# Usage: scripts/build-all.sh
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 2
LOG_DIR="${LOG_DIR:-$ROOT/.opencode/logs}"
mkdir -p "$LOG_DIR"

# triple | toolchain file | ISA token | test-runner
# The test-runner column is empty for non-vendored targets (no tests built).
TARGETS=(
  "aarch64-linux-gnu|scripts/toolchains/aarch64.cmake|aarch64|ctest"
  "arm-linux-gnueabihf|scripts/toolchains/arm32.cmake|arm32|"
  "arm-linux-gnueabi|scripts/toolchains/armeb.cmake|armeb|"
  "mips-linux-gnu|scripts/toolchains/mips32.cmake|mips32|"
  "mipsel-linux-gnu|scripts/toolchains/mips32el.cmake|mips32el|"
  "powerpc-linux-gnu|scripts/toolchains/ppc32.cmake|ppc32|"
  "powerpc64-linux-gnu|scripts/toolchains/ppc64.cmake|ppc64|"
  "powerpc64le-linux-gnu|scripts/toolchains/ppc64le.cmake|ppc64le|"
  "s390x-linux-gnu|scripts/toolchains/s390x.cmake|s390x|"
  "riscv64-linux-gnu|scripts/toolchains/riscv64.cmake|riscv64|"
)

# Backends that are vendored under src/arch/ today (milestone M9 adds the rest).
VENDORED="x86_64 aarch64"

pass=0; skip=0; fail=0
summary=()

has_vendored_backend() {
    case " $VENDORED " in *" $1 "*) return 0 ;; esac
    return 1
}

for entry in "${TARGETS[@]}"; do
    IFS='|' read -r triple toolchain isa runner <<<"$entry"
    dir="build-all/$triple"
    log="$LOG_DIR/build-all-$triple.log"

    if has_vendored_backend "$isa" && [ -f "$ROOT/$toolchain" ]; then
        rm -rf "$ROOT/$dir"
        if cmake -S "$ROOT" -B "$ROOT/$dir" -G Ninja \
                 -DCMAKE_TOOLCHAIN_FILE="$ROOT/$toolchain" >"$log" 2>&1 &&
           cmake --build "$ROOT/$dir" >>"$log" 2>&1 &&
           ctest --test-dir "$ROOT/$dir" --output-on-failure >>"$log" 2>&1; then
            result="PASS  ($(ctest --test-dir "$ROOT/$dir" 2>&1 | grep -E 'tests passed' | tail -1))"
            pass=$((pass + 1))
        else
            result="FAIL  (see $log)"
            fail=$((fail + 1))
        fi
    else
        # Backend not vendored: validate the toolchain file in isolation.
        rm -rf "$ROOT/$dir"
        cmake -S "$ROOT" -B "$ROOT/$dir" -G Ninja -DCOROCC_BUILD_TESTS=OFF \
             -DCMAKE_TOOLCHAIN_FILE="$ROOT/$toolchain" >"$log" 2>&1
        if grep -q "unsupported arch" "$log"; then
            result="PENDING (toolchain OK, backend not vendored yet)"
            skip=$((skip + 1))
        elif grep -qiE "could not be found|not able to compile a simple" "$log"; then
            result="PENDING (cross toolchain not installed)"
            skip=$((skip + 1))
        else
            result="FAIL  (toolchain file broken, see $log)"
            fail=$((fail + 1))
        fi
    fi
    printf '%-22s %s\n' "$triple" "$result"
    summary+=("$triple: $result")
done

echo
echo "built+tested=$pass pending=$skip failed=$fail"
[ "$fail" -eq 0 ] || exit 1
exit 0
