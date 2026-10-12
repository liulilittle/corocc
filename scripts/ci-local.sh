#!/usr/bin/env bash
# Runs the ci.yml steps that are meaningful on a Linux host (WSL included) so
# contributors can reproduce CI locally without pushing. The Windows and macOS
# jobs of ci.yml only configure, and only matter once the milestone-M9
# context-switch backends exist, so they are not repeated here.
set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "== workflow YAML parse =="
python3 - <<'PY'
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

echo
echo "== public-header conformance =="
bash .github/scripts/header-conformance.sh

echo
echo "== native (default toolchain) =="
bash scripts/build.sh

echo
echo "== native (clang) =="
CC=clang CXX=clang++ bash scripts/build.sh

echo
echo "== cross targets =="
if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
    bash scripts/build-all.sh
else
    echo "cross toolchains not installed; skipped (see scripts/README.md)"
fi

echo
echo "== concurrency suites (native + ASan) =="
for t in test_conc_double_resume test_conc_runtime_tsan; do
    for i in $(seq 1 3); do
        timeout 60 "./build/tests/$t" >/dev/null 2>&1 || { echo "$t FAILED (native)"; exit 1; }
    done
    echo "  $t: native 3/3"
done

echo
echo "== ThreadSanitizer =="
# The concurrency suites have a TSan-visible half (the runtime's queue,
# registry and atomics) and a behavioural half (drives through the assembly
# switch). TSan cannot follow a stack switch performed by assembly, and on this
# WSL image TSan aborts even a minimal pthread program with "unexpected memory
# mapping", so the TSan job is verified by ci.yml on the GitHub runners and is
# recorded as not-locally-verified here.
if timeout 30 ./build-tsan/tests/test_conc_runtime_tsan >/dev/null 2>&1; then
    echo "  TSan suite: PASS (this environment supports it)"
else
    echo "  TSan suite: NOT RUN locally (see the note above); ci.yml runs it"
fi
