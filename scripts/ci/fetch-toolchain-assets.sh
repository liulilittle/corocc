#!/usr/bin/env bash
# Fetch + verify + unpack the prebuilt toolchain assets (used by the asset
# rows of the CI and release workflows; see docs/platforms.md).
#
# Contract (single self-contained tree per triple):
#   asset name : corocc-toolchain-<triple>-gcc<gccver>-binutils<binutilsver>-glibc<glibcver>-ubuntu24.04.tar.gz
#   tar top    : corocc-toolchain/<triple>/{bin,lib,libexec,...,sysroot/}
#   gcc        : corocc-toolchain/<triple>/bin/<triple>-gcc
#   sysroot    : corocc-toolchain/<triple>/sysroot   (-print-sysroot, relocatable)
#
# Usage: scripts/ci/fetch-toolchain-assets.sh <triple> [<triple>...]
#
# Environment:
#   COROCC_ASSET_BASE_URL  base URL holding the asset files (default:
#                          https://github.com/$GITHUB_REPOSITORY/releases/download/toolchain-v1);
#                          file:// bases are supported for the local rehearsal.
#   COROCC_ASSET_SHA_FILE  sha256 ledger (default: <root>/ci/toolchain-assets.sha256)
#   COROCC_ASSET_OPT_DIR   unpack root (default: /opt)
#   COROCC_ASSET_SUDO      1 = always sudo, 0 = never, auto = sudo when not root
#   COROCC_ASSET_CURL      curl binary (default: curl)
#   COROCC_ASSET_WORK_DIR  download/work dir (default: mktemp -d)
#
# Per triple, in order (any failure exits 1 -- the flow is fail-closed and
# never fails open on a missing/empty/stale ledger):
#   1. the ledger must exist, be non-empty and carry >=1 line for the triple;
#      each matched line's digest must be a 64-hex sha256 (a placeholder or
#      otherwise unfilled line is rejected here);
#   2. download <base>/<name>;
#   3. `sha256sum -c --strict` against exactly the ledger lines for the triple;
#   4. tar member assertions: every member starts with corocc-toolchain/, no
#      absolute paths and no ../ ; the archive must carry
#      corocc-toolchain/<triple>/bin/<triple>-gcc and
#      corocc-toolchain/<triple>/sysroot/usr/include/stdio.h;
#   5. unpack: <sudo> tar -C "$COROCC_ASSET_OPT_DIR" -xzf <asset>;
#   6. post-unpack assertions: the gcc is executable, the sysroot stdio.h is
#      present, and the packed gcc's `-print-sysroot` equals the unpacked
#      sysroot path (strict when COROCC_ASSET_OPT_DIR=/opt, noted otherwise
#      for scratch rehearsals). Both sides are path-normalized before the
#      comparison (symlinks resolved, `x/..` and `//` collapsed), so a
#      symlinked /opt or a `<prefix>/.../bin/../sysroot` spelling still
#      compares equal; a genuinely different path is still FETCH FAIL;
#   7. symlink audit: no symlink inside corocc-toolchain/<triple>/ may resolve
#      outside that tree (a self-contained relocatable tree carries no
#      /usr/bin or other out-of-tree links).
#
# The legacy two-tree family (separate corocc-sysroot-* and corocc-toolchain-*
# tarballs) is retired by the asset-layout ruling: only the single
# self-contained tree above is handled.
set -u
set -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
[ $# -ge 1 ] || { echo "usage: $0 <triple> [<triple>...]" >&2; exit 2; }

SHA_FILE="${COROCC_ASSET_SHA_FILE:-$ROOT/ci/toolchain-assets.sha256}"
OPT_DIR="${COROCC_ASSET_OPT_DIR:-/opt}"
CURL="${COROCC_ASSET_CURL:-curl}"
WORK_DIR="${COROCC_ASSET_WORK_DIR:-}"

if [ -n "${COROCC_ASSET_BASE_URL:-}" ]; then
    BASE_URL="$COROCC_ASSET_BASE_URL"
elif [ -n "${GITHUB_REPOSITORY:-}" ]; then
    BASE_URL="https://github.com/$GITHUB_REPOSITORY/releases/download/toolchain-v1"
else
    echo "fetch-toolchain-assets: set COROCC_ASSET_BASE_URL (or run under GitHub Actions)" >&2
    exit 2
fi
BASE_URL="${BASE_URL%/}"

case "${COROCC_ASSET_SUDO:-auto}" in
    1) SUDO=(sudo) ;;
    0) SUDO=() ;;
    *) if [ "$(id -u)" -eq 0 ]; then SUDO=(); else SUDO=(sudo); fi ;;
esac

command -v "$CURL" >/dev/null 2>&1 || { echo "fetch-toolchain-assets: '$CURL' not found" >&2; exit 2; }
command -v realpath >/dev/null 2>&1 || { echo "fetch-toolchain-assets: 'realpath' not found" >&2; exit 2; }

if [ -z "$WORK_DIR" ]; then
    WORK_DIR="$(mktemp -d)"
else
    mkdir -p "$WORK_DIR"
fi
"${SUDO[@]}" mkdir -p "$OPT_DIR"

# norm_path <path>: canonicalize for comparison -- resolve symlinks and collapse
# `x/..`, `//` etc. Used on both sides of the -print-sysroot check so a
# symlinked prefix or a `<prefix>/.../bin/../sysroot` spelling compares equal.
norm_path() {
    local p="$1"
    [ -n "$p" ] || { printf '%s' ""; return 0; }
    realpath -m -- "$p" 2>/dev/null || printf '%s' "$p"
}

# audit_symlinks <tree-root>: reject any symlink that resolves outside the tree.
audit_symlinks() { # fail-closed: returns 1 if a symlink escapes the tree
    local root="$1" abs link tgt res bad=0
    [ -d "$root" ] || return 0
    abs="$(cd "$root" && pwd -P)"
    while IFS= read -r -d '' link; do
        tgt="$(readlink "$link")"
        case "$tgt" in
            /*) res="$(realpath -m "$tgt")" ;;
            *)  res="$(realpath -m "$(dirname "$link")/$tgt")" ;;
        esac
        case "$res" in
            "$abs"|"$abs"/*) ;;
            *) echo "FETCH FAIL: symlink escapes the asset tree: $link -> $tgt" >&2
               bad=1 ;;
        esac
    done < <(find "$abs" -type l -print0)
    [ "$bad" -eq 0 ]
}

for triple in "$@"; do
    echo "== asset fetch: $triple =="
    if [ ! -s "$SHA_FILE" ]; then
        echo "FETCH FAIL: sha ledger missing or empty: $SHA_FILE" >&2
        exit 1
    fi
    mapfile -t LINES < <(awk -v t="$triple" '
        /^[[:space:]]*#/ { next }
        NF == 2 && $2 ~ /^corocc-toolchain-/ && index($2, "-" t "-") { print }' "$SHA_FILE")
    if [ "${#LINES[@]}" -eq 0 ]; then
        echo "FETCH FAIL: no ledger entry for triple '$triple' in $SHA_FILE" >&2
        exit 1
    fi

    for line in "${LINES[@]}"; do
        sha="${line%% *}"
        name="${line##* }"
        file="$WORK_DIR/$name"
        if ! printf '%s' "$sha" | grep -Eq '^[0-9a-fA-F]{64}$'; then
            echo "FETCH FAIL: ledger sha for $name is not a 64-hex digest (placeholder? fill it at packaging)" >&2
            exit 1
        fi
        echo "-- download $name"
        if ! "$CURL" -fL --retry 2 -o "$file" "$BASE_URL/$name"; then
            echo "FETCH FAIL: download failed: $BASE_URL/$name" >&2
            exit 1
        fi
        if ! ( cd "$WORK_DIR" && printf '%s  %s\n' "$sha" "$name" | sha256sum -c --strict ); then
            echo "FETCH FAIL: sha256 mismatch for $name" >&2
            exit 1
        fi

        listing="$WORK_DIR/$name.list"
        if ! tar -tzf "$file" >"$listing"; then
            echo "FETCH FAIL: not a gzip tar: $name" >&2
            exit 1
        fi
        if grep -Eq '(^/)|(^|/)\.\.(/|$)' "$listing"; then
            echo "FETCH FAIL: unsafe tar member path in $name:" >&2
            grep -En '(^/)|(^|/)\.\.(/|$)' "$listing" | head -5 >&2
            exit 1
        fi
        if grep -Ev '^corocc-toolchain/' "$listing" | grep -q .; then
            echo "FETCH FAIL: tar members outside corocc-toolchain/: $name" >&2
            grep -Ev '^corocc-toolchain/' "$listing" | head -5 >&2
            exit 1
        fi
        if ! grep -qx "corocc-toolchain/$triple/bin/$triple-gcc" "$listing"; then
            echo "FETCH FAIL: $name lacks corocc-toolchain/$triple/bin/$triple-gcc" >&2
            exit 1
        fi
        if ! grep -qx "corocc-toolchain/$triple/sysroot/usr/include/stdio.h" "$listing"; then
            echo "FETCH FAIL: $name lacks corocc-toolchain/$triple/sysroot/usr/include/stdio.h" >&2
            exit 1
        fi

        echo "-- unpack $name into $OPT_DIR"
        if ! "${SUDO[@]}" tar -C "$OPT_DIR" -xzf "$file"; then
            echo "FETCH FAIL: unpack failed: $name" >&2
            exit 1
        fi

        tree="$OPT_DIR/corocc-toolchain/$triple"
        gcc="$tree/bin/$triple-gcc"
        sysroot="$tree/sysroot"
        if [ ! -x "$gcc" ]; then
            echo "FETCH FAIL: post-unpack compiler missing or not executable: $gcc" >&2
            exit 1
        fi
        if [ ! -f "$sysroot/usr/include/stdio.h" ]; then
            echo "FETCH FAIL: post-unpack sysroot header missing: $sysroot/usr/include/stdio.h" >&2
            exit 1
        fi
        actual_raw="$("$gcc" -print-sysroot 2>/dev/null || true)"
        actual="$(norm_path "$actual_raw")"
        expected="$(norm_path "$sysroot")"
        if [ "$OPT_DIR" = "/opt" ]; then
            if [ "$actual" != "$expected" ]; then
                echo "FETCH FAIL: $gcc -print-sysroot = '$actual_raw' (normalized '$actual'), expected '$sysroot' (normalized '$expected')" >&2
                exit 1
            fi
        else
            echo "note: scratch opt dir in use; $gcc -print-sysroot = '$actual_raw' (normalized '$actual'; CI expects '$sysroot' -> '$expected')"
        fi
        if ! audit_symlinks "$tree"; then
            exit 1
        fi
    done
    echo "== asset fetch OK: $triple =="
done
