#!/usr/bin/env bash
# Fetch + verify + unpack prebuilt toolchain assets (design v1.1 section 8.2;
# v1.2 section 2 wires this into the asset matrix rows).
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
# Per asset, in order (any failure exits 1):
#   1. the ledger must exist, be non-empty and carry >=1 line for the triple
#      (an empty ledger must never fail open);
#   2. download <base>/<name>;
#   3. `sha256sum -c --strict` against exactly the ledger lines for the triple;
#   4. tar top-level assertion: every member starts with corocc-toolchain/ or
#      corocc-sysroot/, no absolute paths and no ../; sysroot assets must carry
#      corocc-sysroot/<triple>/usr/include/stdio.h; full-chain assets must
#      carry corocc-toolchain/<triple>/bin/;
#   5. unpack: <sudo> tar -C "$COROCC_ASSET_OPT_DIR" -xzf <asset>;
#   6. post-unpack assertions: sysroot stdio.h present; for full-chain assets
#      the packed gcc's `-print-sysroot` must equal the unpacked sysroot path
#      (strict when COROCC_ASSET_OPT_DIR=/opt, noted otherwise for scratch
#      rehearsals); sysroot-only assets skip the -print-sysroot probe.
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

if [ -z "$WORK_DIR" ]; then
    WORK_DIR="$(mktemp -d)"
else
    mkdir -p "$WORK_DIR"
fi
"${SUDO[@]}" mkdir -p "$OPT_DIR"

for triple in "$@"; do
    echo "== asset fetch: $triple =="
    if [ ! -s "$SHA_FILE" ]; then
        echo "FETCH FAIL: sha ledger missing or empty: $SHA_FILE" >&2
        exit 1
    fi
    mapfile -t LINES < <(awk -v t="$triple" '
        /^[[:space:]]*#/ { next }
        NF == 2 && $2 ~ /^corocc-(toolchain|sysroot)-/ && index($2, "-" t "-") { print }' "$SHA_FILE")
    if [ "${#LINES[@]}" -eq 0 ]; then
        echo "FETCH FAIL: no ledger entry for triple '$triple' in $SHA_FILE" >&2
        exit 1
    fi

    for line in "${LINES[@]}"; do
        sha="${line%% *}"
        name="${line##* }"
        file="$WORK_DIR/$name"
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
        if grep -Ev '^(corocc-toolchain|corocc-sysroot)/' "$listing" | grep -q .; then
            echo "FETCH FAIL: tar members outside corocc-toolchain/ and corocc-sysroot/: $name" >&2
            grep -Ev '^(corocc-toolchain|corocc-sysroot)/' "$listing" | head -5 >&2
            exit 1
        fi
        case "$name" in
            corocc-sysroot-*)
                if ! grep -qx "corocc-sysroot/$triple/usr/include/stdio.h" "$listing"; then
                    echo "FETCH FAIL: $name lacks corocc-sysroot/$triple/usr/include/stdio.h" >&2
                    exit 1
                fi ;;
            corocc-toolchain-*)
                if ! grep -qE "^corocc-toolchain/$triple/bin/" "$listing"; then
                    echo "FETCH FAIL: $name lacks corocc-toolchain/$triple/bin/" >&2
                    exit 1
                fi ;;
            *)
                echo "FETCH FAIL: unsupported asset name '$name' (expected corocc-toolchain-*/corocc-sysroot-*)" >&2
                exit 1 ;;
        esac

        echo "-- unpack $name into $OPT_DIR"
        if ! "${SUDO[@]}" tar -C "$OPT_DIR" -xzf "$file"; then
            echo "FETCH FAIL: unpack failed: $name" >&2
            exit 1
        fi

        if [ ! -f "$OPT_DIR/corocc-sysroot/$triple/usr/include/stdio.h" ]; then
            echo "FETCH FAIL: post-unpack sysroot header missing: $OPT_DIR/corocc-sysroot/$triple/usr/include/stdio.h" >&2
            exit 1
        fi
        case "$name" in
            corocc-toolchain-*)
                gcc="$OPT_DIR/corocc-toolchain/$triple/bin/$triple-gcc"
                if [ ! -x "$gcc" ]; then
                    echo "FETCH FAIL: post-unpack compiler missing: $gcc" >&2
                    exit 1
                fi
                actual="$("$gcc" -print-sysroot 2>/dev/null || true)"
                expected="$OPT_DIR/corocc-sysroot/$triple"
                if [ "$OPT_DIR" = "/opt" ]; then
                    if [ "$actual" != "$expected" ]; then
                        echo "FETCH FAIL: $gcc -print-sysroot = '$actual', expected '$expected'" >&2
                        exit 1
                    fi
                else
                    echo "note: scratch opt dir in use; $gcc -print-sysroot = '$actual' (CI expects '$expected')"
                fi ;;
        esac
    done
    echo "== asset fetch OK: $triple =="
done
