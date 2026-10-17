#!/usr/bin/env bash
# Static assertions over the workflows and the artefacts they reference,
# including the naming chain between the scripts and the upload paths.
#
# Extraction contracts (the files are ours and format-controlled; a parse that
# stops finding rows is itself a failure -- each extractor asserts a non-empty
# result where one is required):
#   * job ids:   two-space-indented `name:` keys inside the `jobs:` block
#   * matrix:    single-line flow mappings `- { abi: X, toolchain: Y, ... }`
#                (commented rows are ignored everywhere)
#   * needs:     ci-summary's single-line `needs: [...]` list
#   * summary:   `R_<UPPER>: ${{ needs.<job>.result }}` mapping entries plus
#                `report <job> "$R_..."` calls on their own lines
#   * table:     ci/site-matrix.env rows are `site|class|toolchain|apt|excludes|reg|exec`
#   * paths:     `scripts/...` / `.github/scripts/...` tokens in the workflows
#   * naming:    normalized `dist-out/corocc-<tag>-<triple>.{tar.gz,zip}`
#                upload paths vs the `corocc-$TAG-...` constants in
#                scripts/ci/stage-dist*.sh (normalization: the literal
#                `${{ inputs.tag || github.ref_name }}` -> `<tag>`,
#                `${{ matrix.triple }}` -> `<triple>`)
#   * report:    the `  perf-report:` block, sliced from its job key to the
#                next job-boundary line; must be non-gated (no per-event
#                `if:`), non-fatal, and upload an `if: always()` artifact
#
# Exit: 0 all assertions pass; 1 otherwise.
set -u
set -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 2

CI=.github/workflows/ci.yml
REL=.github/workflows/release.yml
SM=ci/site-matrix.env
REPORT_ONLY="${REPORT_ONLY:-perf-report}"
FAIL=0
err() { echo "check-workflows: FAIL: $*" >&2; FAIL=1; }

for f in "$CI" "$REL" "$SM" scripts/ci/build-test.sh scripts/ci/fetch-toolchain-assets.sh \
         scripts/ci/stage-dist.sh scripts/ci/stage-dist-win.sh; do
    [ -f "$f" ] || err "missing required file: $f"
done
[ "$FAIL" -eq 0 ] || { echo "check-workflows: FAILED (missing files)"; exit 1; }

# ---- extractors -----------------------------------------------------------
extract_jobs() { # <file>
    awk '/^jobs:/{f=1;next} f && /^[^ ]/{f=0}
         f && /^  [A-Za-z0-9_-]+:[[:space:]]*$/ {sub(/^  /,"");sub(/:.*/,"");print}' "$1"
}
extract_rows() { # <file> <job> -> the `- { ... }` include lines of that job
    awk -v job="$2" '
        $0 ~ "^  " job ":[[:space:]]*$" {f=1; next}
        f && /^  [A-Za-z0-9_-]+:[[:space:]]*$/ {exit}
        f && /^[[:space:]]*-[[:space:]]*\{/ {print}' "$1"
}
row_field() { # <line> <field>
    printf '%s' "$1" | sed -nE "s/.*$2:[[:space:]]*([^,}]+).*/\1/p" | tr -d ' "' | xargs
}
extract_abis() { # <file> <job>
    extract_rows "$1" "$2" | while IFS= read -r l; do row_field "$l" abi; done | sort -u
}
sm_field() { # <site-key> <column 1..7>
    awk -F'|' -v k="$1" -v c="$2" '
        /^[[:space:]]*#/ { next }
        $1 == k { v=$c; gsub(/^[[:space:]]+/, "", v); gsub(/[[:space:]]+$/, "", v); print v; exit }' "$SM"
}

# ---- (1) ci-summary coverage ----------------------------------------------
mapfile -t JOBS < <(extract_jobs "$CI")
[ "${#JOBS[@]}" -ge 11 ] || err "ci.yml: parsed only ${#JOBS[@]} jobs (contract drift?)"
printf '%s\n' "${JOBS[@]}" | grep -qx ci-summary || err "ci.yml has no ci-summary job"

NEEDS_LINE="$(grep -E '^    needs: \[.*\]$' "$CI" | head -1)"
[ -n "$NEEDS_LINE" ] || err "ci-summary needs list not found / not a single-line list"
NEEDS="$(printf '%s' "$NEEDS_LINE" | sed -E 's/^    needs: \[(.*)\]$/\1/' | tr -d ' ')"
case ",$NEEDS," in *",$REPORT_ONLY,"*) err "$REPORT_ONLY must NOT be in ci-summary needs (report-only)";; esac

declare -A JOB_ENV=()
mapfile -t PAIRS < <(grep -oE 'R_[A-Z0-9_]+: \$\{\{ needs\.[a-z0-9-]+\.result \}\}' "$CI" \
    | sed -E 's/^([A-Z0-9_]+): .*needs\.([a-z0-9-]+)\.result.*$/\1 \2/')
[ "${#PAIRS[@]}" -ge 10 ] || err "ci-summary env mapping: parsed ${#PAIRS[@]} entries (<10, contract drift?)"
for p in "${PAIRS[@]}"; do
    read -r envname j <<<"$p"
    if [ -z "$envname" ] || [ -z "$j" ]; then
        err "ci-summary env mapping: unparsable entry '$p'"
    fi
    JOB_ENV["$j"]="$envname"
done
for j in "${JOBS[@]}"; do
    [ "$j" = ci-summary ] && continue
    [ "$j" = "$REPORT_ONLY" ] && continue
    case ",$NEEDS," in *",$j,"*) ;; *) err "job '$j' missing from ci-summary needs";; esac
    envname="${JOB_ENV[$j]:-}"
    [ -n "$envname" ] || err "ci-summary has no R_* mapping for job '$j' (needs.$j.result)"
    call_line="$(grep -E "^[[:space:]]+report[[:space:]]+$j([[:space:]]|$)" "$CI" | head -1)"
    [ -n "$call_line" ] || err "ci-summary report call for '$j' missing"
    if [ -n "$call_line" ] && [ -n "$envname" ]; then
        case "$call_line" in
            *"\$$envname"*) ;;
            *) err "ci-summary report call for '$j' does not use \$$envname" ;;
        esac
    fi
done
# reverse: no stale needs / env-mapping / report entries
for n in $(printf '%s' "$NEEDS" | tr ',' ' '); do
    printf '%s\n' "${JOBS[@]}" | grep -qx "$n" || err "ci-summary needs references unknown job '$n'"
done
for j in "${!JOB_ENV[@]}"; do
    printf '%s\n' "${JOBS[@]}" | grep -qx "$j" || err "env mapping references unknown job '$j'"
    [ "$j" = "$REPORT_ONLY" ] && err "env mapping must not cover the report-only job '$j'"
done
while IFS= read -r r; do
    printf '%s\n' "${JOBS[@]}" | grep -qx "$r" || err "report call references unknown job '$r'"
done < <(awk '/^[[:space:]]+report[[:space:]]/{print $2}' "$CI" | sort -u)
grep -Fq 'ALLOW_SKIPPED' "$CI" || err "ci-summary skipped-whitelist (ALLOW_SKIPPED) missing"
grep -Fq 'skipped)' "$CI" || err "ci-summary skipped handling missing"

# report-only job shape: always runs, non-gating, with an always() upload.
# The job block runs from its `  <job>:` line to the next job-boundary line.
job_block() { # <file> <job>
    awk -v job="$2" '
        $0 ~ "^  " job ":[[:space:]]*$" {f=1; print; next}
        f && $0 ~ "^  [A-Za-z0-9_-]+:[[:space:]]*$" {exit}
        f {print}' "$1"
}
PERF_BLOCK="$(job_block "$CI" "$REPORT_ONLY")"
[ -n "$PERF_BLOCK" ] || err "$REPORT_ONLY job block not found (contract drift?)"
printf '%s\n' "$PERF_BLOCK" | grep -q 'continue-on-error: true' \
    || err "$REPORT_ONLY must carry continue-on-error: true"
# must NOT be gated per-event (runs on every push/PR/dispatch/schedule)
printf '%s\n' "$PERF_BLOCK" | grep -qE '^[[:space:]]+if:[[:space:]]*github\.event_name' \
    && err "$REPORT_ONLY must NOT be gated on github.event_name (must run on every event)"
printf '%s\n' "$PERF_BLOCK" | grep -q 'uses: actions/upload-artifact' \
    || err "$REPORT_ONLY must upload its logs as an artifact"
printf '%s\n' "$PERF_BLOCK" | grep -q 'if: always()' \
    || err "$REPORT_ONLY artifact upload must carry 'if: always()'"

# ---- (2) matrix same-source ------------------------------------------------
mapfile -t CI_ABIS < <(extract_abis "$CI" cross)
mapfile -t SM_ABIS < <(awk -F'|' '!/^[[:space:]]*#/ && $1 ~ /^cross-/ {sub(/^cross-/,"",$1); print $1}' "$SM" | sort -u)
[ "${#CI_ABIS[@]}" -ge 12 ] || err "ci.yml cross matrix: parsed ${#CI_ABIS[@]} rows (<12, contract drift?)"
[ "${#SM_ABIS[@]}" -ge 12 ] || err "site-matrix: parsed ${#SM_ABIS[@]} cross rows (<12)"
if ! diff -u <(printf '%s\n' "${CI_ABIS[@]}") <(printf '%s\n' "${SM_ABIS[@]}") >/dev/null; then
    err "ci.yml cross ABIs differ from ci/site-matrix.env cross-* rows"
    diff -u <(printf '%s\n' "${CI_ABIS[@]}") <(printf '%s\n' "${SM_ABIS[@]}") >&2 || true
fi
while IFS= read -r l; do
    abi="$(row_field "$l" abi)"; tc="$(row_field "$l" toolchain)"
    smtc="$(sm_field "cross-$abi" 3)"
    [ "$smtc" = "$tc" ] || err "toolchain mismatch for cross-$abi: ci.yml='$tc' site-matrix='$smtc'"
done < <(extract_rows "$CI" cross)
while IFS= read -r l; do
    abi="$(row_field "$l" abi)"; tc="$(row_field "$l" toolchain)"
    rel="$(extract_rows "$REL" prebuild-linux | grep -E "abi:[[:space:]]*${abi}[,}]" | head -1)"
    [ -n "$rel" ] || err "release.yml prebuild-linux: missing abi '$abi'"
    [ -z "$rel" ] || { retc="$(row_field "$rel" toolchain)"
        [ "$retc" = "$tc" ] || err "toolchain mismatch for '$abi': ci.yml='$tc' release.yml='$retc'"; }
done < <(extract_rows "$CI" cross)

# ---- (2b) site-matrix table coverage by ci.yml -----------------------------
NDK_ABIS="$(grep -E '^        android_abi:' "$CI" | head -1)"
while IFS='|' read -r site _cls _tc _apt _excl _reg _exe; do
    case "$site" in ''|'#'*) continue ;; esac
    case "$site" in
        cross-*) abi="${site#cross-}"
                 printf '%s\n' "${CI_ABIS[@]}" | grep -qx "$abi" || err "site row $site not covered by ci.yml cross matrix" ;;
        ndk-*)   if [ -z "$NDK_ABIS" ] || ! grep -q "${site#ndk-}" <<<"$NDK_ABIS"; then
                     err "site row $site not covered by ci.yml ndk job"
                 fi ;;
        *)       printf '%s\n' "${JOBS[@]}" | grep -qx "$site" || err "site row $site has no ci.yml job" ;;
    esac
done < "$SM"
for j in "${JOBS[@]}"; do
    case "$j" in ci-summary|"$REPORT_ONLY"|headers) continue ;; esac
    case "$j" in
        cross|ndk) ;;
        *)  awk -F'|' -v k="$j" '!/^[[:space:]]*#/ && $1 == k {found=1} END{exit found?0:1}' "$SM" \
                || err "ci.yml job '$j' has no site-matrix.env row" ;;
    esac
done

# ---- (3) naming chain (v1.2 section 4; O-3 extraction rules) ---------------
norm_upload() { # upload path -> normalized asset name
    local p="$1"
    p="$(printf '%s' "$p" | sed -e 's/\${{ inputs\.tag || github\.ref_name }}/<tag>/' \
                                 -e 's/\${{ matrix\.triple }}/<triple>/')"
    printf '%s' "${p#dist-out/}"
}
# upload paths carry the literal `${{ ... }}` expressions being compared
UP_LINUX="$(grep -E '^[[:space:]]*path:[[:space:]]*dist-out/corocc-' "$REL" | grep -F '.tar.gz' | head -1 | sed -E 's/^[[:space:]]*path:[[:space:]]*//; s/[[:space:]]+#.*$//' | tr -d '\r')"
UP_WIN="$(grep -E '^[[:space:]]*path:[[:space:]]*dist-out/corocc-' "$REL" | grep -F '.zip' | head -1 | sed -E 's/^[[:space:]]*path:[[:space:]]*//; s/[[:space:]]+#.*$//' | tr -d '\r')"
[ -n "$UP_LINUX" ] || err "release.yml: linux upload path (dist-out/corocc-*.tar.gz) not found"
[ -n "$UP_WIN" ] || err "release.yml: windows upload path (dist-out/corocc-*.zip) not found"
[ "$(norm_upload "$UP_LINUX")" = "corocc-<tag>-<triple>.tar.gz" ] \
    || err "release.yml linux upload path name chain broken: '$UP_LINUX'"
[ "$(norm_upload "$UP_WIN")" = "corocc-<tag>-x86_64-pc-windows-msvc.zip" ] \
    || err "release.yml windows upload path name chain broken: '$UP_WIN'"

SD_TAR="$(grep -oE "corocc-[$]TAG-[$]TRIPLE[.]tar[.]gz" scripts/ci/stage-dist.sh | head -1)"
SW_ZIP="$(grep -oE "corocc-[$]TAG-[$]TRIPLE[.]zip" scripts/ci/stage-dist-win.sh | head -1)"
SW_TRIPLE="$(grep -oE '^TRIPLE="[^"]+"' scripts/ci/stage-dist-win.sh | head -1 | cut -d'"' -f2)"
[ "$SD_TAR" = "corocc-\$TAG-\$TRIPLE.tar.gz" ] || err "stage-dist.sh output constant not found"
[ "$SW_ZIP" = "corocc-\$TAG-\$TRIPLE.zip" ] || err "stage-dist-win.sh output constant not found"
[ -n "$SW_TRIPLE" ] || err "stage-dist-win.sh TRIPLE constant not found"
SD_TAR_N="${SD_TAR//\$TAG/<tag>}"; SD_TAR_N="${SD_TAR_N//\$TRIPLE/<triple>}"
SW_ZIP_N="corocc-<tag>-$SW_TRIPLE.zip"
[ "$(norm_upload "$UP_LINUX")" = "$SD_TAR_N" ] || err "linux upload path != stage-dist.sh naming constant"
[ "$(norm_upload "$UP_WIN")" = "$SW_ZIP_N" ] || err "windows upload path != stage-dist-win.sh naming constant"

grep -Fq "stage-dist.sh \"\${{ matrix.abi }}\" \"\${{ matrix.triple }}\" \"\${{ inputs.tag || github.ref_name }}\"" "$REL" \
    || err "release.yml stage-dist.sh invocation must pass abi/triple/tag verbatim"
grep -Fq "stage-dist-win.sh \"\${{ inputs.tag || github.ref_name }}\"" "$REL" \
    || err "release.yml stage-dist-win.sh invocation must pass the tag expression"
[ "$(grep -cE '^[[:space:]]*if-no-files-found:[[:space:]]*error[[:space:]]*$' "$REL")" -ge 2 ] \
    || err "release.yml: upload steps must set if-no-files-found: error"
grep -Fq "sha256sum \"\$(basename" "$REL" || err "release.yml checksums: flat-basename sha256sum loop missing"
grep -Fq 'uniq -d' "$REL" || err "release.yml checksums: duplicate-name guard missing"
grep -Fq 'sort -k2 SHA256SUMS -o SHA256SUMS' "$REL" || err "release.yml checksums: sort witness missing"
grep -Fq 'set -euo pipefail' scripts/ci/stage-dist.sh || err "stage-dist.sh must run with set -euo pipefail"
grep -q 'COROCC_BUILDING=1' scripts/ci/stage-dist-win.sh || err "stage-dist-win.sh config must define COROCC_BUILDING=1"
grep -q 'corocc.lib' scripts/ci/stage-dist-win.sh || err "stage-dist-win.sh must ship corocc.lib"

# ---- (4) git ls-files: everything the workflows/scripts reference is tracked
mapfile -t REFS < <({
    grep -ohE '(\.github/scripts|scripts)/[A-Za-z0-9_./-]+' "$CI" "$REL"
    awk -F'|' '!/^[[:space:]]*#/ && $3 ~ /^scripts\// {print $3}' "$SM"
    printf '%s\n' tests/CMakeLists.txt CMakeLists.txt include/corocc/coro.hpp \
        perf/bench_cpp_switch.cpp ci/site-matrix.env ci/toolchain-assets.sha256
} | sort -u)
for p in "${REFS[@]}"; do
    git ls-files --error-unmatch "$p" >/dev/null 2>&1 || err "referenced path not tracked by git: $p"
done
n_cpp="$(git ls-files -- 'tests/cpp/*.cpp' | wc -l)"
n_asm="$(git ls-files -- 'src/arch/*.S' | wc -l)"
n_tc="$(git ls-files -- 'scripts/toolchains/*.cmake' | wc -l)"
[ "$n_cpp" -ge 10 ] || err "tracked tests/cpp/*.cpp = $n_cpp (<10)"
[ "$n_asm" -ge 8 ] || err "tracked src/arch/*.S = $n_asm (<8)"
[ "$n_tc" -ge 18 ] || err "tracked scripts/toolchains/*.cmake = $n_tc (<18)"

# ---- summary ---------------------------------------------------------------
if [ "$FAIL" -eq 0 ]; then
    echo "check-workflows: OK"
else
    echo "check-workflows: FAILED"
fi
exit "$FAIL"
