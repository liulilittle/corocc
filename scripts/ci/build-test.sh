#!/usr/bin/env bash
# Single-ABI build + test driver: configure -> -static build -> qemu ctest ->
# population assertion. M10 CI design (v1.2 section 5 interface): the workflow
# YAML only installs dependencies and calls this script per matrix shard;
# running it locally is exactly the same code path.
#
# Usage: scripts/ci/build-test.sh <abi> [options]
#   --toolchain <file>          CMake toolchain file (cross); omit for native
#   --build-dir <dir>           default: build-ci-<abi>
#   --build-type <type>         default: Release
#   --emulator <qemu-...>       optional override of the toolchain emulator
#   --exclude <regex>           ctest -E filter (default for cross: perf|bench)
#   --expect-registered <n[|n]> accepted registered-count(s), '|'-separated
#   --expect-executed <n[|n]>   accepted executed-count(s), '|'-separated
#   --matrix-env <name>         preset floors (see below)
#   --no-clean                  keep the existing build directory
#
# Site floors (double column = registered/executed, M10 v1.2 section 5):
#   cross   34/34  (post-guard target; pre-guard trees run 44/43 with the
#                   unconditionally registered C++ suites - both accepted
#                   while the guard rolls out, annotated in the log)
#   native  44/43  (with the Boost coexist suites: 45/44)
#   nospin  45/44
#   asan    43/40  (with the Boost coexist suites: 44/41)
#   windows 42/42
#   tsan    1/1
#
# Environment: JOBS (default: nproc), LOG_DIR (default: .opencode/logs/ci)
set -u

usage() { sed -n '2,33p' "$0" >&2; exit 2; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ABI="${1:-}"; [ -n "$ABI" ] || usage; shift || true

TOOLCHAIN=""; BUILD_DIR=""; BUILD_TYPE="Release"; EMULATOR=""
EXCLUDE=""; EXCLUDE_SET=0; EXP_REG=""; EXP_EXE=""; MATRIX_ENV=""; CLEAN=1

while [ $# -gt 0 ]; do
    case "$1" in
        --toolchain)          TOOLCHAIN="$2"; shift 2 ;;
        --build-dir)          BUILD_DIR="$2"; shift 2 ;;
        --build-type)         BUILD_TYPE="$2"; shift 2 ;;
        --emulator)           EMULATOR="$2"; shift 2 ;;
        --exclude)            EXCLUDE="$2"; EXCLUDE_SET=1; shift 2 ;;
        --expect-registered)  EXP_REG="$2"; shift 2 ;;
        --expect-executed)    EXP_EXE="$2"; shift 2 ;;
        --matrix-env)         MATRIX_ENV="$2"; shift 2 ;;
        --no-clean)           CLEAN=0; shift ;;
        *) echo "build-test.sh: unknown option '$1'" >&2; usage ;;
    esac
done

case "$MATRIX_ENV" in
    "")
        [ $EXCLUDE_SET -eq 1 ] || EXCLUDE="perf|bench"
        ;;
    cross)
        EXP_REG="${EXP_REG:-34|44}"; EXP_EXE="${EXP_EXE:-34|43}"
        [ $EXCLUDE_SET -eq 1 ] || EXCLUDE="perf|bench" ;;
    native)  EXP_REG="${EXP_REG:-44|45}"; EXP_EXE="${EXP_EXE:-43|44}" ;;
    nospin)  EXP_REG="${EXP_REG:-45}";    EXP_EXE="${EXP_EXE:-44}" ;;
    asan)    EXP_REG="${EXP_REG:-43|44}"; EXP_EXE="${EXP_EXE:-40|41}" ;;
    windows) EXP_REG="${EXP_REG:-42}";    EXP_EXE="${EXP_EXE:-42}" ;;
    tsan)    EXP_REG="${EXP_REG:-1}";     EXP_EXE="${EXP_EXE:-1}" ;;
    *) echo "build-test.sh: unknown --matrix-env '$MATRIX_ENV'" >&2; exit 2 ;;
esac

BUILD_DIR="${BUILD_DIR:-$ROOT/build-ci-$ABI}"
LOG_DIR="${LOG_DIR:-$ROOT/.opencode/logs/ci}"
mkdir -p "$LOG_DIR"
JOBS="${JOBS:-$(nproc)}"

echo "== [$ABI] configure ($BUILD_TYPE) =="
[ "$CLEAN" -eq 1 ] && rm -rf "$BUILD_DIR"
CMAKE_ARGS=(-S "$ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE")
[ -n "$TOOLCHAIN" ] && CMAKE_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN")
[ -n "$EMULATOR" ]  && CMAKE_ARGS+=("-DCMAKE_CROSSCOMPILING_EMULATOR=$EMULATOR")
if ! cmake "${CMAKE_ARGS[@]}" >"$LOG_DIR/$ABI-configure.log" 2>&1; then
    echo "[$ABI] CONFIGURE FAILED"; tail -5 "$LOG_DIR/$ABI-configure.log"; exit 1
fi

echo "== [$ABI] build =="
if ! cmake --build "$BUILD_DIR" -j"$JOBS" >"$LOG_DIR/$ABI-build.log" 2>&1; then
    echo "[$ABI] BUILD FAILED"; tail -8 "$LOG_DIR/$ABI-build.log"; exit 1
fi

echo "== [$ABI] population =="
CTEST_FILTER=()
[ -n "$EXCLUDE" ] && CTEST_FILTER=(-E "$EXCLUDE")
count_tests() { ctest --test-dir "$BUILD_DIR" "$@" -N 2>/dev/null \
    | grep -oE 'Total Tests: [0-9]+' | grep -oE '[0-9]+' | head -1; }
REGISTERED="$(count_tests)"
EXECUTED="$(count_tests "${CTEST_FILTER[@]}")"
echo "[$ABI] registered=${REGISTERED:-?} executed=${EXECUTED:-?}"

echo "== [$ABI] ctest =="
ctest --test-dir "$BUILD_DIR" "${CTEST_FILTER[@]}" --output-on-failure \
    >"$LOG_DIR/$ABI-ctest.log" 2>&1
ctest_rc=$?
tail -3 "$LOG_DIR/$ABI-ctest.log"

status=0
num_ok() { echo "$1" | tr '|' '\n' | grep -qx "$2"; }
if [ "$ctest_rc" -ne 0 ]; then
    echo "[$ABI] CTEST FAILED (rc=$ctest_rc, see $LOG_DIR/$ABI-ctest.log)"; status=1
fi
if [ -n "$EXP_REG" ] && ! num_ok "$EXP_REG" "$REGISTERED"; then
    echo "[$ABI] POPULATION MISMATCH: registered=$REGISTERED, expected one of: $EXP_REG"
    status=1
fi
if [ -n "$EXP_EXE" ] && ! num_ok "$EXP_EXE" "$EXECUTED"; then
    echo "[$ABI] POPULATION MISMATCH: executed=$EXECUTED, expected one of: $EXP_EXE"
    status=1
fi

if [ "$status" -eq 0 ]; then echo "[$ABI] OK"; else echo "[$ABI] FAILED"; fi
exit "$status"
