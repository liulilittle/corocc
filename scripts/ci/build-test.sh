#!/usr/bin/env bash
# Single-site build + test driver: configure -> required-macro assertion ->
# -static build -> qemu ctest -> population assertion. CI design: the workflow
# YAML only installs dependencies and calls this script per matrix shard;
# running it locally is exactly the same code path.
#
# Usage: scripts/ci/build-test.sh <site> [options] [-- <cmake args>...]
#
#   --toolchain <file>          CMake toolchain file (cross); omit/empty = native
#   --build-dir <dir>           default: build-ci-<site>
#   --build-type <type>         default: Release
#   --emulator <qemu-...>       optional override of the toolchain emulator
#   --cmake-option <OPT>        extra -D option for configure (repeatable)
#   --select <regex>            ctest -R filter (tsan: conc_runtime_tsan);
#                               registered/executed are both the match count
#   --include <regex>           alias of --select (kept for existing callers)
#   --exclude <regex>           ctest -E site-level additions (replaces the
#                               excludes column of the site row)
#   --expect-registered <n[|n]> accepted registered count(s), '|'-separated;
#                               overrides the site-matrix floor, exact match
#   --expect-executed <n[|n]>   accepted executed count(s), '|'-separated
#   --matrix-env <name>         legacy class preset (cross|native|nospin|asan|
#                               windows|tsan); overrides the site-matrix floor
#   --site <key>                explicit ci/site-matrix.env row key
#   --no-clean                  keep the existing build directory
#
# Floors come from ci/site-matrix.env: registered is the raw `ctest -N` count
# (no -E), executed uses the same -E as the run; both are lower bounds (>=), so
# an image carrying the Boost coexistence suites reports +1 and still passes.
# The global constant ALL_EXCL='cpp_perf' is applied on every site; the row's
# excludes column adds site exemptions (ASan: the three documented
# exemptions).
#
# Required-macro assertion (G0): asan/tsan/nospin sites must show
# COROCC_ASAN / COROCC_TSAN / COROCC_PROBE_NOSPIN = ON in the build cache, so a
# dropped -D flag can never silently degrade those gates to a no-op.
#
# Environment:
#   JOBS      build parallelism (default: nproc)
#   CTEST_JOBS parallel ctest jobs (default: 2, matches the 2-vCPU runners)
#   LOG_DIR   log directory (default: ci-logs/<site>)
#   COROCC_SITE_MATRIX_ENV  override the site-matrix.env path
set -u
set -o pipefail

usage() {
    cat >&2 <<'EOF'
usage: scripts/ci/build-test.sh <site> [options] [-- <cmake args>...]
  --toolchain <file>  --build-dir <dir>  --build-type <type>  --emulator <qemu>
  --cmake-option <D>  --select <regex>  --include <regex>  --exclude <regex>
  --expect-registered <n[|n]>  --expect-executed <n[|n]>  --site <key>
  --matrix-env <cross|native|nospin|asan|windows|tsan>  --no-clean
EOF
    exit 2
}

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SITE="${1:-}"; [ -n "$SITE" ] || usage; shift || true

TOOLCHAIN=""; BUILD_DIR=""; BUILD_TYPE="Release"; EMULATOR=""
EXCLUDE=""; EXCLUDE_SET=0; INCLUDE=""; INCLUDE_SET=0
EXP_REG=""; EXP_EXE=""; MATRIX_ENV=""; SITE_KEY=""; CLEAN=1
CM_OPTS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --toolchain)          TOOLCHAIN="${2:-}"; shift 2 ;;
        --build-dir)          BUILD_DIR="${2:-}"; shift 2 ;;
        --build-type)         BUILD_TYPE="${2:-}"; shift 2 ;;
        --emulator)           EMULATOR="${2:-}"; shift 2 ;;
        --cmake-option)       CM_OPTS+=("-D${2:-}"); shift 2 ;;
        --select|--include)   INCLUDE="${2:-}"; INCLUDE_SET=1; shift 2 ;;
        --exclude)            EXCLUDE="${2:-}"; EXCLUDE_SET=1; shift 2 ;;
        --expect-registered)  EXP_REG="${2:-}"; shift 2 ;;
        --expect-executed)    EXP_EXE="${2:-}"; shift 2 ;;
        --matrix-env)         MATRIX_ENV="${2:-}"; shift 2 ;;
        --site)               SITE_KEY="${2:-}"; shift 2 ;;
        --no-clean)           CLEAN=0; shift ;;
        --)                   shift
                              while [ $# -gt 0 ]; do CM_OPTS+=("$1"); shift; done ;;
        *) echo "build-test.sh: unknown option '$1'" >&2; usage ;;
    esac
done

# ---- floor resolution -----------------------------------------------------
# Order: explicit --expect-* (exact alternates) > --matrix-env preset (exact
# alternates, legacy callers) > ci/site-matrix.env row (>= lower bounds).
SITE_ENV_FILE="${COROCC_SITE_MATRIX_ENV:-$ROOT/ci/site-matrix.env}"
ALL_EXCL="cpp_perf"

row_get() { # <key> -> "excludes|registered|executed"; empty when absent
    [ -f "$SITE_ENV_FILE" ] || return 0
    awk -F'|' -v key="$1" '
        /^[[:space:]]*#/ { next }
        $1 == key {
            for (i = 5; i <= 7; i++) {
                gsub(/^[[:space:]]+/, "", $i); gsub(/[[:space:]]+$/, "", $i)
            }
            print $5 "|" $6 "|" $7
            exit
        }' "$SITE_ENV_FILE"
}

ROW=""; KEY=""
if [ -z "$MATRIX_ENV" ]; then
    KEY="$SITE_KEY"; [ -n "$KEY" ] || KEY="$SITE"
    ROW="$(row_get "$KEY")"
    if [ -z "$ROW" ] && [ -n "$TOOLCHAIN" ]; then
        KEY="cross-$SITE"
        ROW="$(row_get "$KEY")"
    fi
fi
ROW_EXCL=""; ROW_REG=""; ROW_EXE=""
if [ -n "$ROW" ]; then
    IFS='|' read -r ROW_EXCL ROW_REG ROW_EXE <<<"$ROW"
fi
case "$ROW_EXCL" in "("*) ROW_EXCL="" ;; esac   # annotation, not a filter

case "$MATRIX_ENV" in
    "") ;;
    cross)   [ -n "$EXP_REG" ] || EXP_REG="37|47"; [ -n "$EXP_EXE" ] || EXP_EXE="37|46" ;;
    native)  [ -n "$EXP_REG" ] || EXP_REG="47|48"; [ -n "$EXP_EXE" ] || EXP_EXE="46|47" ;;
    nospin)  [ -n "$EXP_REG" ] || EXP_REG="48|49"; [ -n "$EXP_EXE" ] || EXP_EXE="47|48" ;;
    asan)    [ -n "$EXP_REG" ] || EXP_REG="46|47"; [ -n "$EXP_EXE" ] || EXP_EXE="43|44" ;;
    windows) [ -n "$EXP_REG" ] || EXP_REG="45";    [ -n "$EXP_EXE" ] || EXP_EXE="45" ;;
    tsan)    [ -n "$EXP_REG" ] || EXP_REG="47|48"; [ -n "$EXP_EXE" ] || EXP_EXE="46|47" ;;
    *) echo "build-test.sh: unknown --matrix-env '$MATRIX_ENV'" >&2; exit 2 ;;
esac

# Site-level exclude additions: explicit --exclude > asan preset > table row.
if [ "$EXCLUDE_SET" -eq 1 ]; then
    SITE_EXCL="$EXCLUDE"
elif [ "$MATRIX_ENV" = "asan" ]; then
    # The three misuse-detection suites abort differently under ASan (the
    # sanitizer intercepts the detection path); documented exemption set (the
    # ASan site row in ci/site-matrix.env).
    SITE_EXCL="coro_exactlyonce coro_crossresume conc_double_resume"
else
    SITE_EXCL="$ROW_EXCL"
fi

EXCLUDE_RE=""
append_excl() { [ -n "$1" ] || return 0; EXCLUDE_RE="${EXCLUDE_RE:+$EXCLUDE_RE|}${1}"; }
append_excl "$ALL_EXCL"
read -r -a SITE_EXCL_TOKENS <<<"$SITE_EXCL"
for tok in "${SITE_EXCL_TOKENS[@]}"; do append_excl "$tok"; done

BUILD_DIR="${BUILD_DIR:-$ROOT/build-ci-$SITE}"
LOG_DIR="${LOG_DIR:-$ROOT/ci-logs/$SITE}"
mkdir -p "$LOG_DIR"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
CTEST_JOBS="${CTEST_JOBS:-2}"

echo "== [$SITE] configure ($BUILD_TYPE) =="
[ "$CLEAN" -eq 1 ] && rm -rf "$BUILD_DIR"
CMAKE_ARGS=(-S "$ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE")
# L2 guard: an empty/omitted --toolchain is ignored (the release native row
# relies on this); a non-empty value is injected verbatim.
[ -n "$TOOLCHAIN" ] && CMAKE_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN")
[ -n "$EMULATOR" ]  && CMAKE_ARGS+=("-DCMAKE_CROSSCOMPILING_EMULATOR=$EMULATOR")
if command -v ccache >/dev/null 2>&1; then
    CMAKE_ARGS+=("-DCMAKE_C_COMPILER_LAUNCHER=ccache"
                 "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache")
fi
[ ${#CM_OPTS[@]} -gt 0 ] && CMAKE_ARGS+=("${CM_OPTS[@]}")
if ! cmake "${CMAKE_ARGS[@]}" 2>&1 | tee "$LOG_DIR/$SITE-configure.log"; then
    echo "[$SITE] CONFIGURE FAILED (see $LOG_DIR/$SITE-configure.log)"; exit 1
fi

# G0: the sanitizer/nospin sites must carry their compile switch in the cache.
case "$SITE" in
    asan)   MACRO_KEY=COROCC_ASAN ;;
    tsan)   MACRO_KEY=COROCC_TSAN ;;
    nospin) MACRO_KEY=COROCC_PROBE_NOSPIN ;;
    *)      MACRO_KEY="" ;;
esac
if [ -n "$MACRO_KEY" ]; then
    if ! grep -q "^${MACRO_KEY}:BOOL=ON$" "$BUILD_DIR/CMakeCache.txt" 2>/dev/null; then
        echo "[$SITE] REQUIRED MACRO MISSING: ${MACRO_KEY}:BOOL=ON not in CMakeCache.txt (G0)" >&2
        exit 1
    fi
    echo "[$SITE] required macro ok: ${MACRO_KEY}:BOOL=ON"
fi

echo "== [$SITE] build =="
if ! cmake --build "$BUILD_DIR" -j"$JOBS" 2>&1 | tee "$LOG_DIR/$SITE-build.log"; then
    echo "[$SITE] BUILD FAILED (see $LOG_DIR/$SITE-build.log)"; exit 1
fi

echo "== [$SITE] population =="
CTEST_FILTER=()
[ -n "$EXCLUDE_RE" ] && CTEST_FILTER=(-E "$EXCLUDE_RE")
[ -n "$INCLUDE" ] && CTEST_FILTER+=(-R "$INCLUDE")
count_tests() { ctest --test-dir "$BUILD_DIR" "$@" -N 2>/dev/null \
    | grep -oE 'Total Tests: [0-9]+' | grep -oE '[0-9]+' | head -1; }
if [ -n "$INCLUDE" ]; then
    REGISTERED="$(count_tests -R "$INCLUDE")"
else
    REGISTERED="$(count_tests)"
fi
EXECUTED="$(count_tests "${CTEST_FILTER[@]-}")"
echo "[$SITE] registered=${REGISTERED:-?} executed=${EXECUTED:-?} (exclude='$EXCLUDE_RE')"
[ -n "$ROW" ] && echo "[$SITE] floors from ci/site-matrix.env ($KEY): registered>=$ROW_REG executed>=$ROW_EXE"

echo "== [$SITE] ctest =="
ctest --test-dir "$BUILD_DIR" "${CTEST_FILTER[@]-}" --output-on-failure \
    -j"$CTEST_JOBS" --timeout 300 2>&1 | tee "$LOG_DIR/$SITE-ctest.log"
ctest_rc=$?
tail -3 "$LOG_DIR/$SITE-ctest.log" || true

{
    echo "site=$SITE"
    echo "git=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo n/a)"
    echo "cmake=$(cmake --version 2>/dev/null | head -1)"
    echo "build_type=$BUILD_TYPE"
    echo "toolchain=${TOOLCHAIN:-native}"
    if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
        grep -E "^CMAKE_(C|CXX)_COMPILER:(FILEPATH|STRING)=" "$BUILD_DIR/CMakeCache.txt" || true
        grep -E "^CMAKE_CROSSCOMPILING_EMULATOR:" "$BUILD_DIR/CMakeCache.txt" || true
    fi
    cc --version 2>/dev/null | head -1 || true
    if command -v ccache >/dev/null 2>&1; then ccache --version 2>/dev/null | head -1; fi
    [ -n "$EMULATOR" ] && printf 'emulator=%s\n' "$EMULATOR"
    printf 'registered=%s executed=%s exclude=%s\n' "${REGISTERED:-?}" "${EXECUTED:-?}" "$EXCLUDE_RE"
    if [ -n "${ANDROID_NDK:-}" ] && [ -f "$ANDROID_NDK/source.properties" ]; then
        grep -E "^Pkg.Revision" "$ANDROID_NDK/source.properties" || true
    fi
} >"$LOG_DIR/$SITE-versions.txt" 2>&1 || true

status=0
num_ok() { echo "$1" | tr '|' '\n' | grep -qx "$2"; }
if [ "$ctest_rc" -ne 0 ]; then
    echo "[$SITE] CTEST FAILED (rc=$ctest_rc, see $LOG_DIR/$SITE-ctest.log)"; status=1
fi
if [ -n "$EXP_REG" ]; then
    if ! num_ok "$EXP_REG" "$REGISTERED"; then
        echo "[$SITE] POPULATION MISMATCH: registered=$REGISTERED, expected one of: $EXP_REG"; status=1
    fi
elif [ -n "$ROW_REG" ] && [ "$ROW_REG" != "n/a" ]; then
    if [ -z "$REGISTERED" ] || [ "$REGISTERED" -lt "$ROW_REG" ]; then
        echo "[$SITE] POPULATION BELOW FLOOR: registered=$REGISTERED, floor >= $ROW_REG (ci/site-matrix.env: $KEY)"; status=1
    fi
fi
if [ -n "$EXP_EXE" ]; then
    if ! num_ok "$EXP_EXE" "$EXECUTED"; then
        echo "[$SITE] POPULATION MISMATCH: executed=$EXECUTED, expected one of: $EXP_EXE"; status=1
    fi
elif [ -n "$ROW_EXE" ] && [ "$ROW_EXE" != "n/a" ]; then
    if [ -z "$EXECUTED" ] || [ "$EXECUTED" -lt "$ROW_EXE" ]; then
        echo "[$SITE] POPULATION BELOW FLOOR: executed=$EXECUTED, floor >= $ROW_EXE (ci/site-matrix.env: $KEY)"; status=1
    fi
fi
if [ "$status" -eq 0 ]; then echo "[$SITE] OK"; else echo "[$SITE] FAILED"; fi
exit "$status"
