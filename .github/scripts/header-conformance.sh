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

# ---- the C++11 wrapper header (coro.hpp) ----
# Strict C++11 for the wrapper itself, the forward standards (14/17/20 must
# compile clean: new-standard deprecation warnings are failures), the
# degraded modes (-fno-exceptions/-fno-rtti) and both include orders (the C
# headers first and the wrapper first) -- the header must stay
# include-order-idempotent.
cat >"$TMP/probe_cpp_hpp.cpp" <<'CPPEOF'
#include <corocc/coro.hpp>

int main() {
    corocc::runtime rt;      /* an empty, invalid handle is fine */
    corocc::coroutine co;
    corocc::scope* p = 0;
    corocc::cell<int> c;
    corocc::task<int> t;
    corocc::sched_task<int> st;
    corocc::channel<int> ch(rt);
    corocc::mutex m(rt);
    (void)rt; (void)co; (void)p; (void)c; (void)t; (void)st; (void)ch; (void)m;
    return corocc::status(CORO_OK).ok() ? 0 : 1;
}
CPPEOF

cat >"$TMP/probe_cpp_after.cpp" <<'CPPEOF'
#include <corocc/coro.h>
#include <corocc/coro_rt.h>
#include <corocc/coro_sched.h>
#include <corocc/coro_scope.h>
#include <corocc/coro.hpp>
int main() {
    corocc::runtime rt;
    (void)rt;
    return 0;
}
CPPEOF
cp "$TMP/probe_cpp_hpp.cpp" "$TMP/probe_cpp_before.cpp"

run2() { # label, compiler, std, extra flags, source
    local label="$1" cc="$2" std="$3" extra="$4" src="$5"
    # shellcheck disable=SC2086
    if "$cc" "$std" -pedantic-errors -Wall -Wextra -Werror $extra \
            -I"$ROOT/include" -c "$src" -o "$TMP/probe.o" 2>"$TMP/err.log"; then
        echo "ok   $label"
    else
        echo "FAIL $label"
        sed 's/^/     /' "$TMP/err.log"
        fail=1
    fi
}

run2 "g++     c++11 hpp strict"   g++     -std=c++11 ""              "$TMP/probe_cpp_hpp.cpp"
run2 "g++     c++14 hpp forward"  g++     -std=c++14 ""              "$TMP/probe_cpp_hpp.cpp"
run2 "g++     c++17 hpp forward"  g++     -std=c++17 ""              "$TMP/probe_cpp_hpp.cpp"
run2 "g++     c++20 hpp forward"  g++     -std=c++20 ""              "$TMP/probe_cpp_hpp.cpp"
run2 "clang++ c++11 hpp strict"   clang++ -std=c++11 ""              "$TMP/probe_cpp_hpp.cpp"
run2 "clang++ c++17 hpp forward"  clang++ -std=c++17 ""              "$TMP/probe_cpp_hpp.cpp"
run2 "clang++ c++20 hpp forward"  clang++ -std=c++20 ""              "$TMP/probe_cpp_hpp.cpp"
run2 "g++     no-exceptions"      g++     -std=c++11 "-fno-exceptions" "$TMP/probe_cpp_hpp.cpp"
run2 "g++     no-rtti"            g++     -std=c++11 "-fno-rtti"       "$TMP/probe_cpp_hpp.cpp"
run2 "g++     no-eh-no-rtti"      g++     -std=c++11 "-fno-exceptions -fno-rtti" "$TMP/probe_cpp_hpp.cpp"
run2 "clang++ no-exceptions"      clang++ -std=c++11 "-fno-exceptions" "$TMP/probe_cpp_hpp.cpp"
run2 "g++     include order C-first"  g++ -std=c++11 ""              "$TMP/probe_cpp_after.cpp"
run2 "g++     include order hpp-first" g++ -std=c++11 ""             "$TMP/probe_cpp_before.cpp"

# Negative compile pins: the reference-result rejection (static_assert) and
# the deleted closeout points in the no-exceptions mode MUST fail to compile.
cat >"$TMP/probe_cpp_ref.cpp" <<'CPPEOF'
#include <corocc/coro.hpp>
int main() {
    corocc::task<int&> t; /* static_assert: references are rejected */
    (void)t;
    return 0;
}
CPPEOF

cat >"$TMP/probe_cpp_ref_cell.cpp" <<'CPPEOF'
#include <corocc/coro.hpp>
int main() {
    corocc::cell<char&> c; /* static_assert: references are rejected */
    (void)c;
    return 0;
}
CPPEOF

cat >"$TMP/probe_cpp_noeh_get.cpp" <<'CPPEOF'
#define COROCC_CPP_NO_EXCEPTIONS 1
#include <corocc/coro.hpp>
int main() {
    corocc::task<int> t;
    (void)t.get(); /* deleted in the degraded mode: must not compile */
    return 0;
}
CPPEOF

run_fail() { # label, compiler, source
    if "$2" -std=c++11 -pedantic-errors -I"$ROOT/include" -c "$3" \
            -o "$TMP/probe.o" >/dev/null 2>&1; then
        echo "FAIL $1 (compiled; the rejection pin did not fire)"
        fail=1
    else
        echo "ok   $1"
    fi
}

run_fail "g++     task<R&> refused"     g++ "$TMP/probe_cpp_ref.cpp"
run_fail "g++     cell<R&> refused"     g++ "$TMP/probe_cpp_ref_cell.cpp"
run_fail "g++     get() gone under -fno-exceptions" g++ "$TMP/probe_cpp_noeh_get.cpp"

exit "$fail"
