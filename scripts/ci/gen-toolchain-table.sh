#!/usr/bin/env bash
# Generate the per-site toolchain-source Markdown tables (docs/toolchains.md,
# docs/toolchains.zh-CN.md and the release-body draft) from the repository's
# authoritative files, and self-check the result.
#
# Authoritative inputs (the single source of truth):
#   ci/site-matrix.env              site rows (keys, toolchain-file column)
#   .github/workflows/ci.yml        cross matrix (abi/toolchain/apt/assets),
#                                   job ids and ndk ABIs
#   .github/workflows/release.yml   cross matrix triples
#   ci/toolchain-assets.sha256      prebuilt-asset sha256 ledger
#   ci/toolchain-sources.env        curated per-site source kind + assets
#
# Self-check: the site set declared in ci/toolchain-sources.env must equal the
# union of the ci.yml sites (leaf jobs, cross-<abi>, ndk-<abi>) and the
# ci/site-matrix.env keys -- otherwise exit 1 with a diff. Cross rows are also
# checked against release.yml (triple), the ci.yml assets: field and the
# sha256 ledger.
#
# This script is deliberately NOT referenced by any workflow: it is a
# documentation tool run on demand, so ci/check-workflows.sh is unaffected.
#
# Usage:
#   scripts/ci/gen-toolchain-table.sh [--lang en|zh] [--scope all|cross]
#                                     [--check] [--out FILE]
#
#   --scope all    docs table (header + all sites)   [default]
#   --scope cross  release-body fragment (cross sites + the asset ledger)
#   --check        run the validations only, print a summary, emit no table
#
# The backticks inside the single-quoted printf format strings below are
# literal Markdown, not command substitution.
# shellcheck disable=SC2016
set -u
set -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2

CI=.github/workflows/ci.yml
REL=.github/workflows/release.yml
SM=ci/site-matrix.env
LEDGER=ci/toolchain-assets.sha256
ENV=ci/toolchain-sources.env

LANG_SEL=en
SCOPE=all
CHECK=0
OUT=""

while [ $# -gt 0 ]; do
    case "$1" in
        --lang)  LANG_SEL="${2:?}"; shift 2 ;;
        --scope) SCOPE="${2:?}"; shift 2 ;;
        --check) CHECK=1; shift ;;
        --out)   OUT="${2:?}"; shift 2 ;;
        -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
        *) echo "gen-toolchain-table: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
case "$LANG_SEL" in en|zh) ;; *) echo "gen-toolchain-table: --lang must be en|zh" >&2; exit 2 ;; esac
case "$SCOPE" in all|cross) ;; *) echo "gen-toolchain-table: --scope must be all|cross" >&2; exit 2 ;; esac

for f in "$CI" "$REL" "$SM" "$LEDGER" "$ENV"; do
    [ -f "$f" ] || { echo "gen-toolchain-table: missing required file: $f" >&2; exit 2; }
done

FAIL=0
err() { echo "gen-toolchain-table: FAIL: $*" >&2; FAIL=1; }

# ---- extractors (same contracts as ci/check-workflows.sh) ------------------
extract_jobs() { # <file>
    awk '/^jobs:/{f=1;next} f && /^[^ ]/{f=0}
         f && /^  [A-Za-z0-9_-]+:[[:space:]]*$/ {sub(/^  /,"");sub(/:.*/,"");print}' "$1"
}
extract_rows() { # <file> <job>
    awk -v job="$2" '
        $0 ~ "^  " job ":[[:space:]]*$" {f=1; next}
        f && /^  [A-Za-z0-9_-]+:[[:space:]]*$/ {exit}
        f && /^[[:space:]]*-[[:space:]]*\{/ {print}' "$1"
}
row_field() { # <line> <field>  (token values only; whitespace collapsed)
    printf '%s' "$1" | sed -nE "s/.*$2:[[:space:]]*([^,}]+).*/\1/p" | tr -d ' "' | xargs
}
row_val() { # <line> <field>  (keeps inner spaces; strips surrounding quotes)
    local line="$1" field="$2"
    if printf '%s' "$line" | grep -qE "[ ,{]$field:[[:space:]]*\""; then
        printf '%s' "$line" | sed -nE "s/.*[ ,{]$field:[[:space:]]*\"([^\"]*)\".*/\1/p"
    else
        printf '%s' "$line" | sed -nE "s/.*[ ,{]$field:[[:space:]]*([^,}]+).*/\1/p" \
            | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//'
    fi
}
extract_abis() { extract_rows "$1" "$2" | while IFS= read -r l; do row_field "$l" abi; done; }
grep_line() { # <file> <regex>
    grep -nE "$2" "$1" 2>/dev/null | head -1 | cut -d: -f1
}

# ---- authoritative site sets ----------------------------------------------
mapfile -t CI_JOBS < <(extract_jobs "$CI")
mapfile -t CI_CROSS_ABIS < <(extract_abis "$CI" cross | sort)
NDK_ABIS="$(grep -E '^        android_abi:' "$CI" | head -1 | sed -E 's/.*\[([^]]*)\].*/\1/' | tr ',' '\n' | sed -E 's/#.*$//' | tr -d ' ' | grep -v '^$')"
mapfile -t SM_KEYS < <(awk -F'|' '!/^[[:space:]]*#/ && $1!="" {gsub(/[[:space:]]/,"",$1); print $1}' "$SM")

declare -A EXPECT=()
for j in "${CI_JOBS[@]}"; do
    case "$j" in cross|ndk|ci-summary) ;; *) EXPECT["$j"]=1 ;; esac
done
for a in "${CI_CROSS_ABIS[@]}"; do EXPECT["cross-$a"]=1; done
while IFS= read -r a; do [ -n "$a" ] && EXPECT["ndk-$a"]=1; done <<<"$NDK_ABIS"
for k in "${SM_KEYS[@]}"; do EXPECT["$k"]=1; done

# ---- release.yml abi -> triple --------------------------------------------
declare -A REL_TRIPLE=()
while IFS= read -r l; do
    a="$(row_val "$l" abi)"; t="$(row_val "$l" triple)"
    [ -n "$a" ] && REL_TRIPLE["$a"]="$t"
done < <(extract_rows "$REL" prebuild-linux)

# ---- ci.yml abi -> apt / assets -------------------------------------------
declare -A CI_APT=() CI_ASSETS=()
while IFS= read -r l; do
    a="$(row_val "$l" abi)"
    [ -n "$a" ] || continue
    CI_APT["$a"]="$(row_val "$l" apt)"
    CI_ASSETS["$a"]="$(row_val "$l" assets)"
done < <(extract_rows "$CI" cross)

# ---- ledger sha256 ---------------------------------------------------------
declare -A ASSET_SHA=()
while read -r sha name; do [ -n "${name:-}" ] && ASSET_SHA["$name"]="$sha"; done \
    < <(awk '!/^[[:space:]]*#/ && NF==2 {print $1" "$2}' "$LEDGER")

# ---- parse ci/toolchain-sources.env ---------------------------------------
ENV_SITES=(); declare -A E_TRIPLE=() E_SRC=() E_DETAIL=() E_ASSETS=()
while IFS='|' read -r site triple src detail assets; do
    case "$site" in ''|'#'*|' '*|$'\t'*) continue ;; esac
    [ "$site" = "${site// /}" ] || { err "env: site '$site' contains whitespace"; continue; }
    ENV_SITES+=("$site")
    E_TRIPLE["$site"]="$triple"; E_SRC["$site"]="$src"
    E_DETAIL["$site"]="$detail"; E_ASSETS["$site"]="$assets"
done < "$ENV"

# ---- self-check 1: site sets ----------------------------------------------
mapfile -t ENV_SORTED < <(printf '%s\n' "${ENV_SITES[@]}" | sort)
mapfile -t EXP_SORTED < <(printf '%s\n' "${!EXPECT[@]}" | sort)
if ! diff -u <(printf '%s\n' "${EXP_SORTED[@]}") <(printf '%s\n' "${ENV_SORTED[@]}") >/tmp/gen-tc-set.diff 2>&1; then
    err "env site set != ci.yml+site-matrix site set"
    cat /tmp/gen-tc-set.diff >&2
fi

# ---- self-check 2: cross subset -------------------------------------------
mapfile -t ENV_CROSS < <(printf '%s\n' "${ENV_SITES[@]}" | grep '^cross-' | sed 's/^cross-//' | sort)
if ! diff -u <(printf '%s\n' "${CI_CROSS_ABIS[@]}") <(printf '%s\n' "${ENV_CROSS[@]}") >/tmp/gen-tc-cross.diff 2>&1; then
    err "env cross-* set != ci.yml cross matrix abis"
    cat /tmp/gen-tc-cross.diff >&2
fi

# ---- self-check 3: per-row cross correctness ------------------------------
for site in "${ENV_SITES[@]}"; do
    triple="${E_TRIPLE[$site]}"; src="${E_SRC[$site]}"
    detail="${E_DETAIL[$site]}"; assets="${E_ASSETS[$site]}"
    [ -n "$triple" ] || err "env[$site]: empty triple"
    case "$site" in
        cross-*)
            abi="${site#cross-}"
            [ "${REL_TRIPLE[$abi]:-}" = "$triple" ] \
                || err "env[$site]: triple '$triple' != release.yml '${REL_TRIPLE[$abi]:-}'"
            ci_assets="${CI_ASSETS[$abi]:-}"
            case "$src" in
                apt|host-m32)
                    [ -z "$ci_assets" ] || err "env[$site]: source=$src but ci.yml has assets '$ci_assets'"
                    [ -z "$assets" ] || err "env[$site]: source=$src must not carry assets"
                    [ -n "${CI_APT[$abi]:-}" ] || err "env[$site]: ci.yml has no apt: field" ;;
                prebuilt-v1)
                    [ -n "$ci_assets" ] || err "env[$site]: source=prebuilt-v1 but ci.yml assets empty"
                    [ -n "$assets" ] || err "env[$site]: source=prebuilt-v1 must carry assets"
                    case "$assets" in
                        *corocc-toolchain-"$ci_assets"-*|*"corocc-toolchain-$ci_assets"*) ;;
                        *) err "env[$site]: assets do not reference ci.yml asset triple '$ci_assets'" ;;
                    esac ;;
                *) err "env[$site]: cross row source must be apt|host-m32|prebuilt-v1 (got '$src')" ;;
            esac ;;
        *)
            case "$src" in
                runner-image|xcode|vs2022|vs2026|ndk) ;;
                *) err "env[$site]: non-cross source must be runner-image|xcode|vs2022|vs2026|ndk (got '$src')" ;;
            esac
            [ -z "$assets" ] || err "env[$site]: non-cross row must not carry assets" ;;
    esac
    if [ -n "$assets" ]; then
        localIFS="$IFS"; IFS=';'
        for a in $assets; do
            [ -n "${ASSET_SHA[$a]:-}" ] || err "env[$site]: asset '$a' not found in $LEDGER"
        done
        IFS="$localIFS"
    fi
done

[ "$FAIL" -eq 0 ] || { echo "gen-toolchain-table: FAILED (self-check)" >&2; exit 1; }

if [ "$CHECK" = 1 ]; then
    echo "gen-toolchain-table: OK"
    echo "  sites (env): ${#ENV_SITES[@]}"
    echo "  sites (ci.yml+site-matrix): ${#EXPECT[@]}"
    echo "  cross sites: ${#ENV_CROSS[@]} (ci.yml cross matrix: ${#CI_CROSS_ABIS[@]})"
    echo "  ledger assets: ${#ASSET_SHA[@]}"
    echo "  site-set diff: $( [ -s /tmp/gen-tc-set.diff ] && echo NONEMPTY || echo empty )"
    echo "  cross-set diff: $( [ -s /tmp/gen-tc-cross.diff ] && echo NONEMPTY || echo empty )"
    exit 0
fi

# ---- rendering -------------------------------------------------------------
DASH="—"

source_label() { # <kind> <detail> <apt>  -> localized label (stdout)
    local kind="$1" detail="$2" apt="$3"
    local extra=""
    [ -n "$detail" ] && extra="$detail"
    if [ "$LANG_SEL" = zh ]; then
        case "$kind" in
            apt)          printf 'apt 交叉包:%s' "$apt" ;;
            host-m32)     printf '宿主 gcc/g++ -m32(apt:%s)' "$apt" ;;
            prebuilt-v1)  printf '预编译 toolchain-v1 资产(gcc 13.3.0 + glibc 2.39)+ apt binutils:%s' "$apt" ;;
            runner-image) printf 'runner 镜像工具链(Ubuntu 24.04;GCC 13 / Clang 18)'; [ -n "$extra" ] && printf ' + apt:%s' "$extra" ;;
            xcode)        printf 'Xcode clang(macOS runner 镜像)' ;;
            vs2022)       printf 'Visual Studio 2022(MSVC x64 / Win32)' ;;
            vs2026)       printf 'Visual Studio 2026(MSVC / armasm64,ARM64)' ;;
            ndk)          printf 'Android NDK r26+ clang(预装;ANDROID_ABI 选择目标)' ;;
        esac
    else
        case "$kind" in
            apt)          printf 'apt cross packages: %s' "$apt" ;;
            host-m32)     printf 'host gcc/g++ -m32 (apt: %s)' "$apt" ;;
            prebuilt-v1)  printf 'prebuilt toolchain-v1 assets (gcc 13.3.0 + glibc 2.39) + apt binutils: %s' "$apt" ;;
            runner-image) printf 'runner image toolchain (Ubuntu 24.04; GCC 13 / Clang 18)'; [ -n "$extra" ] && printf ' + apt: %s' "$extra" ;;
            xcode)        printf 'Xcode clang (macOS runner image)' ;;
            vs2022)       printf 'Visual Studio 2022 (MSVC x64 / Win32)' ;;
            vs2026)       printf 'Visual Studio 2026 (MSVC / armasm64, ARM64)' ;;
            ndk)          printf 'Android NDK r26+ clang (preinstalled; ANDROID_ABI selects the target)' ;;
        esac
    fi
}

asset_cell() { # <assets>
    local assets="$1" out="" a
    [ -n "$assets" ] || { printf '%s' "$DASH"; return; }
    local old="$IFS"; IFS=';'
    for a in $assets; do
        [ -n "$out" ] && out+="<br>"
        out+="$a \`${ASSET_SHA[$a]}\`"
    done
    IFS="$old"
    printf '%s' "$out"
}

consumer_cell() { # <site>
    local site="$1" abi out=""
    case "$site" in
        cross-*)
            abi="${site#cross-}"
            local l1 l2 l3
            l1="$(grep_line "$CI" "abi:[[:space:]]*${abi}[,}]")"
            l2="$(grep_line "$REL" "abi:[[:space:]]*${abi}[,}]")"
            l3="$(grep_line "$SM" "^cross-${abi}[|]")"
            [ -n "$l1" ] && out+="ci.yml:$l1"
            [ -n "$l2" ] && out+="${out:+, }release.yml:$l2"
            [ -n "$l3" ] && out+="${out:+, }site-matrix.env:$l3"
            ;;
        windows-msvc)
            # the x64 Windows asset is produced by release.yml prebuild-windows
            local l1 l2 l3
            l1="$(grep_line "$CI" '^  windows-msvc:[[:space:]]*$')"
            l2="$(grep_line "$REL" '^  prebuild-windows:[[:space:]]*$')"
            l3="$(grep_line "$SM" "^${site}[|]")"
            [ -n "$l1" ] && out+="ci.yml:$l1"
            [ -n "$l2" ] && out+="${out:+, }release.yml:$l2"
            [ -n "$l3" ] && out+="${out:+, }site-matrix.env:$l3"
            ;;
        ndk-*)
            local l1 l3
            l1="$(grep_line "$CI" '^        android_abi:')"
            l3="$(grep_line "$SM" "^${site}[|]")"
            [ -n "$l1" ] && out+="ci.yml:$l1"
            [ -n "$l3" ] && out+="${out:+, }site-matrix.env:$l3"
            ;;
        *)
            local l1 l3
            l1="$(grep_line "$CI" "^  ${site}:[[:space:]]*$")"
            l3="$(grep_line "$SM" "^${site}[|]")"
            [ -n "$l1" ] && out+="ci.yml:$l1"
            [ -n "$l3" ] && out+="${out:+, }site-matrix.env:$l3"
            ;;
    esac
    printf '%s' "${out:-$DASH}"
}

emit_table() { # <scope> ; prints the Markdown table for the requested scope
    local scope="$1" site
    if [ "$LANG_SEL" = zh ]; then
        printf '| 站点 | triple | 工具链来源 | 资产名 + sha256(仅 v1) | 消费位置 |\n'
        printf '| --- | --- | --- | --- | --- |\n'
    else
        printf '| site | triple | toolchain source | asset + sha256 (v1 rows only) | consumer |\n'
        printf '| --- | --- | --- | --- | --- |\n'
    fi
    for site in "${ENV_SITES[@]}"; do
        if [ "$scope" = cross ]; then
            case "$site" in cross-*) ;; *) continue ;; esac
        fi
        local abi="" apt=""
        case "$site" in
            cross-*) abi="${site#cross-}"; apt="${CI_APT[$abi]:-}" ;;
        esac
        printf '| `%s` | `%s` | %s | %s | %s |\n' \
            "$site" "${E_TRIPLE[$site]}" \
            "$(source_label "${E_SRC[$site]}" "${E_DETAIL[$site]}" "$apt")" \
            "$(asset_cell "${E_ASSETS[$site]}")" \
            "$(consumer_cell "$site")"
    done
}

emit_ledger() { # the toolchain-v1 asset ledger (names referenced by the env)
    local names="" site a out
    for site in "${ENV_SITES[@]}"; do
        [ -n "${E_ASSETS[$site]}" ] || continue
        local old="$IFS"; IFS=';'
        for a in ${E_ASSETS[$site]}; do names+="$a"$'\n'; done
        IFS="$old"
    done
    names="$(printf '%s' "$names" | sort -u)"
    if [ "$LANG_SEL" = zh ]; then
        printf '## toolchain-v1 资产台账(tag: `toolchain-v1`)\n\n'
    else
        printf '## toolchain-v1 asset ledger (tag: `toolchain-v1`)\n\n'
    fi
    while IFS= read -r a; do
        [ -n "$a" ] || continue
        printf -- '- `%s`<br>`%s`\n' "$a" "${ASSET_SHA[$a]}"
    done <<<"$names"
}

out_dst() { if [ -n "$OUT" ]; then cat >"$OUT"; else cat; fi; }

if [ "$SCOPE" = all ]; then
    if [ "$LANG_SEL" = zh ]; then
        {
            printf '# toolchains — 全站工具链来源\n\n'
            printf '> [English](toolchains.md) | 中文\n\n'
            printf '本表记录 corocc 每个 CI/release 站点的编译器与 sysroot 来源:runner 镜像、\n'
            printf 'apt 交叉包、经 sha256 校验的预编译 `toolchain-v1` 资产、Xcode / Visual Studio /\n'
            printf 'Android NDK。每行的 `消费位置` 指向确切的 workflow / 矩阵行(行号在生成时计算)。\n'
            printf '表中也列出两个没有 `ci/site-matrix.env` 行的 ci.yml 任务(`headers`、`perf-report`),\n'
            printf '其工具链同为 runner 镜像。\n\n'
            printf '本表由 `scripts/ci/gen-toolchain-table.sh` 从仓库权威文件生成(改 `ci/toolchain-sources.env`\n'
            printf '与相关 workflow 后重跑生成器;**不要手改本文件**)。生成器同时校验本表站点集合等于\n'
            printf '`ci.yml`(job、cross、ndk 矩阵)与 `ci/site-matrix.env` 的并集,不一致即 exit 1;\n'
            printf '该脚本**不接入 CI**。\n\n'
            printf '## 全站工具链来源表\n\n'
            emit_table all
            printf '\n'
        }
    else
        {
            printf '# toolchains — per-site toolchain sources\n\n'
            printf '> [中文](toolchains.zh-CN.md) | English\n\n'
            printf 'This table records, for every corocc CI/release site, where its compiler and\n'
            printf 'sysroot come from: the runner image, apt cross packages, the sha256-verified\n'
            printf 'prebuilt `toolchain-v1` assets, Xcode / Visual Studio / the Android NDK. The\n'
            printf '`consumer` column cites the exact workflow / matrix lines (line numbers are\n'
            printf 'computed at generation time). It also carries the two ci.yml jobs that have no\n'
            printf '`ci/site-matrix.env` row (`headers`, `perf-report`), whose toolchain is the\n'
            printf 'runner image like the native sites.\n\n'
            printf 'It is generated from the repository'"'"'s authoritative files by\n'
            printf '`scripts/ci/gen-toolchain-table.sh` (edit `ci/toolchain-sources.env` and the\n'
            printf 'referenced workflows, then re-run the generator; do not hand-edit this file).\n'
            printf 'The generator verifies that the site set here equals the union of the `ci.yml`\n'
            printf 'jobs / `cross` / `ndk` matrices and the `ci/site-matrix.env` rows; a mismatch\n'
            printf 'makes it exit 1. It is not wired into CI.\n\n'
            printf '## Per-site toolchain sources\n\n'
            emit_table all
            printf '\n'
        }
    fi | out_dst
else
    if [ "$LANG_SEL" = zh ]; then
        {
            printf '## 工具链来源 —— 交叉站(全 20 站)与 sha256 校验的预编译资产\n\n'
            printf '每个交叉站点的编译器/sysroot 来自 apt 交叉包,或本 release 内经 sha256 校验的\n'
            printf '预编译 `toolchain-v1` 资产(解包到 `/opt`,由 `scripts/ci/fetch-toolchain-assets.sh`\n'
            printf '消费,台账 `ci/toolchain-assets.sha256`)。`toolchain-v1` 资产不携带自足 binutils:\n'
            printf '多数 `<host-prefix>-{ld,ar,nm,...}` 为指向 `/usr/bin` 的绝对软链,`armeb` 的 `as` 为\n'
            printf '`exec /usr/bin/arm-linux-gnueabi-as -EB` 的薄包装,故这些行仍保留 apt binutils 包。\n'
            printf '下载基址可用环境变量 `COROCC_ASSET_BASE_URL` 覆盖(默认本 tag 的\n'
            printf '`.../releases/download/toolchain-v1`)。完整、随时更新的表见 `docs/toolchains.md`。\n\n'
            emit_table cross
            printf '\n'
            emit_ledger
        }
    else
        {
            printf '## Toolchain sources — every cross site (20) and the sha256-verified prebuilt assets\n\n'
            printf 'Each cross site gets its compiler/sysroot from apt cross packages, or from the\n'
            printf 'sha256-verified prebuilt `toolchain-v1` assets carried by this release (unpacked under\n'
            printf '`/opt`, consumed by `scripts/ci/fetch-toolchain-assets.sh`, ledger\n'
            printf '`ci/toolchain-assets.sha256`). The `toolchain-v1` assets do not carry\n'
            printf 'self-contained binutils: most `<host-prefix>-{ld,ar,nm,...}` entries are absolute\n'
            printf 'symlinks into `/usr/bin`, and `armeb`'"'"'s `as` is a thin\n'
            printf '`exec /usr/bin/arm-linux-gnueabi-as -EB` wrapper, so those rows keep the apt\n'
            printf 'binutils packages. The download base can be overridden with `COROCC_ASSET_BASE_URL`\n'
            printf '(default: this tag, `.../releases/download/toolchain-v1`). The canonical,\n'
            printf 'always-up-to-date table lives in `docs/toolchains.md`.\n\n'
            emit_table cross
            printf '\n'
            emit_ledger
        }
    fi | out_dst
fi
