#!/usr/bin/env bash
# Local mirror of the ci.yml steps that are meaningful on a Linux host
# (WSL included): YAML parse -> actionlint (when installed) -> shellcheck
# (when installed) -> ci/check-workflows.sh -> the same per-site driver the
# workflows call (scripts/ci/build-test.sh, crowdsourced floors from
# ci/site-matrix.env) -> optional file:// asset rehearsal -> release publish
# smoke. Windows/macOS-only steps are not repeated here.
#
# Environment:
#   COROCC_ASSET_REHEARSAL_DIR  when set, run fetch-toolchain-assets.sh with
#                               COROCC_ASSET_BASE_URL=file://<dir>; the dir
#                               must hold the asset tarballs and a
#                               toolchain-assets.sha256 ledger
#   COROCC_REHEARSAL_TRIPLES    triples for that rehearsal (default: aarch64_be)
#   SKIP_NATIVE / SKIP_CROSS    1 to skip the respective site groups
#   JOBS / CTEST_JOBS           forwarded to build-test.sh
set -u
set -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 2
rc=0

step() { echo; echo "== $* =="; }

step "workflow YAML parse"
python3 - <<'PY' || rc=1
import pathlib, sys, yaml
ok = True
for path in sorted(pathlib.Path(".github/workflows").glob("*.yml")):
    try:
        data = yaml.safe_load(path.read_text(encoding="utf-8"))
        jobs = data.get("jobs", {})
        print(f"  {path.name}: OK ({len(jobs)} jobs: {', '.join(jobs)})")
    except Exception as exc:  # noqa: BLE001
        print(f"  {path.name}: PARSE ERROR: {exc}")
        ok = False
sys.exit(0 if ok else 1)
PY

step "actionlint (workflow expression/lint)"
if command -v actionlint >/dev/null 2>&1; then
    actionlint -color || rc=1
else
    echo "SKIP actionlint (not installed)"
fi

step "shellcheck (scripts/ci/*.sh scripts/ci-local.sh ci/check-workflows.sh)"
if command -v shellcheck >/dev/null 2>&1; then
    shellcheck scripts/ci/*.sh scripts/ci-local.sh ci/check-workflows.sh || rc=1
else
    echo "SKIP shellcheck (not installed)"
fi

step "ci/check-workflows.sh (summary coverage / matrix same-source / naming chain / git ls-files)"
bash ci/check-workflows.sh || rc=1

step "public-header conformance"
bash .github/scripts/header-conformance.sh || rc=1

if [ "${SKIP_NATIVE:-0}" != "1" ]; then
    step "native sites (same driver as ci.yml)"
    bash scripts/ci/build-test.sh native-gcc || rc=1
    if command -v clang >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1; then
        CC=clang CXX=clang++ bash scripts/ci/build-test.sh native-clang || rc=1
    else
        echo "SKIP native-clang (clang/clang++ not installed)"
    fi
fi

if [ "${SKIP_CROSS:-0}" != "1" ]; then
    step "cross sites (same driver as ci.yml)"
    # name|toolchain|probe (absolute path or command name; @gcc-m32 compiles a
    # 32-bit probe); sites absent from the active ci/site-matrix.env rows are
    # driven with --matrix-env cross (floors 34/34 post-guard).
    CROSS_TARGETS=(
        "aarch64|scripts/toolchains/aarch64.cmake|aarch64-linux-gnu-gcc"
        "armhf|scripts/toolchains/arm32.cmake|arm-linux-gnueabihf-gcc"
        "i386|scripts/toolchains/i386.cmake|@gcc-m32"
        "mips_o32|scripts/toolchains/mips32.cmake|mips-linux-gnu-gcc"
        "mipsel_o32|scripts/toolchains/mips32el.cmake|mipsel-linux-gnu-gcc"
        "mips64_n64|scripts/toolchains/mips64.cmake|mips64-linux-gnuabi64-gcc-13"
        "mips64el_n64|scripts/toolchains/mips64el.cmake|mips64el-linux-gnuabi64-gcc-13"
        "mipsn32|scripts/toolchains/mipsn32.cmake|mips64-linux-gnuabi64-gcc-13"
        "mipsn32el|scripts/toolchains/mipsn32el.cmake|mips64el-linux-gnuabi64-gcc-13"
        "ppc32|scripts/toolchains/ppc32.cmake|powerpc-linux-gnu-gcc"
        "ppc64|scripts/toolchains/ppc64.cmake|powerpc64-linux-gnu-gcc"
        "ppc64le|scripts/toolchains/ppc64le.cmake|powerpc64le-linux-gnu-gcc"
        "s390x|scripts/toolchains/s390x.cmake|s390x-linux-gnu-gcc"
        "riscv64|scripts/toolchains/riscv64.cmake|riscv64-linux-gnu-gcc"
        "loongarch64|scripts/toolchains/loongarch64.cmake|loongarch64-linux-gnu-gcc-13"
        "sparc64|scripts/toolchains/sparc64.cmake|sparc64-linux-gnu-gcc-13"
        "aarch64_be|scripts/toolchains/aarch64_be.cmake|/opt/corocc-toolchain/aarch64_be-linux-gnu/bin/aarch64_be-linux-gnu-gcc"
        "armeb|scripts/toolchains/armeb.cmake|/opt/corocc-toolchain/armeb-linux-gnueabi/bin/armeb-linux-gnueabi-gcc"
        "riscv32|scripts/toolchains/riscv32.cmake|/opt/corocc-toolchain/riscv32-linux-gnu/bin/riscv32-linux-gnu-gcc"
    )
    probe_ok() { # <probe>
        case "$1" in
            @gcc-m32) echo 'int main(void){return 0;}' | gcc -m32 -x c - -o /dev/null 2>/dev/null ;;
            /*)       [ -x "$1" ] ;;
            *)        command -v "$1" >/dev/null 2>&1 ;;
        esac
    }
    for entry in "${CROSS_TARGETS[@]}"; do
        IFS='|' read -r name tc probe <<<"$entry"
        if ! probe_ok "$probe"; then
            echo "SKIP $name ($probe not available)"
            continue
        fi
        extra=()
        if ! awk -F'|' -v k="cross-$name" '!/^[[:space:]]*#/ && $1 == k {found=1} END{exit found?0:1}' ci/site-matrix.env; then
            extra=(--matrix-env cross)
        fi
        if bash scripts/ci/build-test.sh "$name" --toolchain "$tc" "${extra[@]}"; then
            :
        else
            rc=1
        fi
    done
fi

if [ -n "${COROCC_ASSET_REHEARSAL_DIR:-}" ]; then
    step "file:// asset rehearsal ($COROCC_ASSET_REHEARSAL_DIR)"
    export COROCC_ASSET_BASE_URL="file://$COROCC_ASSET_REHEARSAL_DIR"
    export COROCC_ASSET_SHA_FILE="${COROCC_ASSET_SHA_FILE:-$COROCC_ASSET_REHEARSAL_DIR/toolchain-assets.sha256}"
    triples="${COROCC_REHEARSAL_TRIPLES:-aarch64_be}"
    # shellcheck disable=SC2086
    bash scripts/ci/fetch-toolchain-assets.sh $triples || rc=1
    for t in $triples; do
        tc="scripts/toolchains/$t.cmake"
        if [ -f "$tc" ]; then
            bash scripts/ci/build-test.sh "$t" --toolchain "$tc" --matrix-env cross || rc=1
        else
            echo "note: $t has no toolchain file; skipped build/test"
        fi
    done
fi

step "release publish smoke (flat SHA256SUMS, sha256sum -c)"
if compgen -G "dist-out/corocc-*.tar.gz" >/dev/null 2>&1; then
    flat="$(mktemp -d)"
    cp dist-out/corocc-*.tar.gz "$flat"/
    for z in dist-out/corocc-*.zip; do [ -f "$z" ] && cp "$z" "$flat"/; done
    if ( cd "$flat" && shopt -s nullglob && set -- corocc-* && (($# > 0)) \
         && sha256sum "$@" > SHA256SUMS && sha256sum -c SHA256SUMS ); then
        echo "release smoke: OK (see the flat list above)"
    else
        rc=1
    fi
    rm -rf "$flat"
else
    echo "SKIP release smoke (no dist-out/corocc-*.tar.gz; run stage-dist.sh first)"
fi

echo
if [ "$rc" -eq 0 ]; then echo "ci-local: OK"; else echo "ci-local: FAILED"; fi
exit "$rc"
