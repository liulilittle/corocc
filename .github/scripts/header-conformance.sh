#!/usr/bin/env bash
# Public-header conformance gate: every supported compiler and language
# standard must accept the public headers with -pedantic-errors (no
# extensions, no warnings), because the documented contract is "compiles as
# C99 and earlier and as C++11 and earlier".
set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat >"$TMP/probe.c" <<'EOF'
#include <corocc/coro_status.h>
#include <corocc/coro_stack.h>
#include <corocc/coro_ctx.h>

int main(void) {
    coro_stack stk; /* complete types: usable by value */
    coro_ctx   cx;
    (void)stk;
    (void)cx;
    return 0;
}
EOF
cp "$TMP/probe.c" "$TMP/probe.cpp"

fail=0
run() { # label, compiler, std, source
    local label="$1" cc="$2" std="$3" src="$4"
    if "$cc" "$std" -pedantic-errors -I"$ROOT/include" -c "$src" -o "$TMP/probe.o" 2>"$TMP/err.log"; then
        echo "ok   $label"
    else
        echo "FAIL $label"
        sed 's/^/     /' "$TMP/err.log"
        fail=1
    fi
}

run "gcc   c89" gcc   -std=gnu89 "$TMP/probe.c"
run "gcc   c99" gcc   -std=c99   "$TMP/probe.c"
run "gcc   c11" gcc   -std=c11   "$TMP/probe.c"
run "clang c99" clang -std=c99   "$TMP/probe.c"
run "clang c11" clang -std=c11   "$TMP/probe.c"
run "g++   c++98" g++ -std=c++98  "$TMP/probe.cpp"
run "g++   c++11" g++ -std=c++11  "$TMP/probe.cpp"
run "g++   c++17" g++ -std=c++17  "$TMP/probe.cpp"
run "clang++ c++11" clang++ -std=c++11 "$TMP/probe.cpp"

exit "$fail"
