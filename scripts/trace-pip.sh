#!/bin/bash
set -euo pipefail

if [ $# -gt 1 ]; then
	echo "usage: $0 [package]"
	exit 0
fi

cd "$(dirname "$0")"

V_FLAG=""
V=${PTRACE_PIP_V:-0}
if [ "$V" -gt 0 ]; then
	V_FLAG="-$(printf 'v%.0s' $(seq 1 "$V"))"
fi

export PIP_CACHE_DIR="$HOME/.cache/trace-python/pip-sdists"
export PATH=$PWD:/opt/trace-python/bin:$PATH

NPROC=${NPROC:-$(nproc)}

export MAX_JOBS=$NPROC
export CMAKE_BUILD_PARALLEL_LEVEL=$NPROC
export NINJA_JOBS=$NPROC

python3 install-build-deps.py $1
python3 install-special-packages.py $1

/opt/trace-python/bin/pip3 install --no-binary :all: --no-build-isolation $V_FLAG $1
