#!/usr/bin/env bash
# Runs tests/testapp under the instrumented interpreter, postprocesses the
# merged trace, then hands the resulting trace.db to test.py for the
# structural checks.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CPYTHON="$ROOT/cpython/python"
TESTS="$ROOT/tests"
CONFIG="$TESTS/testapp-config.json"
SCRIPT="$TESTS/testapp/main.py"

OUTDIR="$(mktemp -d)"
trap 'rm -rf "$OUTDIR"' EXIT

# The application ends itself with SIGTERM under SIG_DFL; the tracer must
# preserve the default action (death by signal) after flushing, so bash
# reports 128+SIGTERM=143 for the foreground child.
set +e
PYTHONPATH="$TESTS" PYTHON_D3G_CONFIG="$CONFIG" PYTHON_D3G_OUTDIR="$OUTDIR" \
    "$CPYTHON" -m d3g -- "$SCRIPT"
status=$?
set -e
if [ "$status" -ne 143 ]; then
    echo "tracer exited with $status, expected 143 (SIGTERM)" >&2
    exit 1
fi

# The runner leaves one {pid}.db per process under a numbered run
# directory (outdir/1, outdir/2, ... -- a fresh OUTDIR always yields "1");
# postprocess takes that run directory as a subdir of PYTHON_D3G_OUTDIR and
# merges it into trace.db offline, then builds the dependency graph.
# d3g.astdump is parsed by $VIRTUAL_ENV/bin/python3, a plain venv with no
# d3g package installed; point it at cpython/Lib so it can find it.
VIRTUAL_ENV="$ROOT/venv" PYTHON_D3G_OUTDIR="$OUTDIR" PYTHONPATH="$ROOT/cpython/Lib" \
    "$CPYTHON" -m d3g.postprocess 1

DB_PATH="$OUTDIR/1/trace.db"
if [ ! -f "$DB_PATH" ]; then
    echo "no trace.db produced" >&2
    exit 1
fi

"$CPYTHON" "$TESTS/test.py" "$DB_PATH"
