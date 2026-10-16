#!/usr/bin/env bash
# Build and test corocc for every cross target (all context-switch backends
# are vendored). The per-ABI work is delegated to scripts/ci/build-test.sh so
# that this local matrix runner and the CI use the exact same code path.
#
# Environment:
#   LOG_DIR   base log dir (default: <repo>/ci-logs; build-test.sh writes its
#             per-site logs into $LOG_DIR)
#   JOBS      parallel build jobs
#
# Usage: scripts/build-all.sh [--abi <name>] [-- keep-going build-test options]
#        scripts/build-all.sh              # all cross targets
#        scripts/build-all.sh --abi riscv32
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 2
export LOG_DIR="${LOG_DIR:-$ROOT/ci-logs}"

ONLY=""
while [ $# -gt 0 ]; do
    case "$1" in
        --abi) ONLY="$2"; shift 2 ;;
        *) echo "build-all.sh: unknown option '$1'" >&2; exit 2 ;;
    esac
done

# name | toolchain file (all backends are vendored)
TARGETS=(
  "aarch64|scripts/toolchains/aarch64.cmake"
  "aarch64_be|scripts/toolchains/aarch64_be.cmake"
  "i386|scripts/toolchains/i386.cmake"
  "arm32|scripts/toolchains/arm32.cmake"
  "armeb|scripts/toolchains/armeb.cmake"
  "mips32|scripts/toolchains/mips32.cmake"
  "mips32el|scripts/toolchains/mips32el.cmake"
  "mips64|scripts/toolchains/mips64.cmake"
  "mips64el|scripts/toolchains/mips64el.cmake"
  "mipsn32|scripts/toolchains/mipsn32.cmake"
  "mipsn32el|scripts/toolchains/mipsn32el.cmake"
  "ppc32|scripts/toolchains/ppc32.cmake"
  "ppc64|scripts/toolchains/ppc64.cmake"
  "ppc64le|scripts/toolchains/ppc64le.cmake"
  "s390x|scripts/toolchains/s390x.cmake"
  "riscv64|scripts/toolchains/riscv64.cmake"
  "riscv32|scripts/toolchains/riscv32.cmake"
  "loongarch64|scripts/toolchains/loongarch64.cmake"
  "sparc64|scripts/toolchains/sparc64.cmake"
)

pass=0; fail=0
results=()
for entry in "${TARGETS[@]}"; do
    IFS='|' read -r name toolchain <<<"$entry"
    [ -n "$ONLY" ] && [ "$name" != "$ONLY" ] && continue
    if "$ROOT/scripts/ci/build-test.sh" "$name" \
           --toolchain "$ROOT/$toolchain" --matrix-env cross; then
        results+=("$name: PASS"); pass=$((pass + 1))
    else
        results+=("$name: FAIL"); fail=$((fail + 1))
    fi
done

echo
for r in "${results[@]}"; do printf '%-24s %s\n' "${r%%:*}" "${r#*: }"; done
echo "built+tested=$pass failed=$fail"
[ "$fail" -eq 0 ] || exit 1
exit 0
