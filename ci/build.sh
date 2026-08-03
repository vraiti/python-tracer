#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")/.."

IMAGE=trace-python-build
BUILD_DIR="$(pwd)/build"

mkdir -p "$BUILD_DIR/opt" "$BUILD_DIR/root"

podman build -t "$IMAGE" -f ci/Containerfile .

podman run --rm \
    -v "$(pwd):/src:Z" \
    -v "$BUILD_DIR/opt:/opt:Z" \
    -v "$BUILD_DIR/root:/root:Z" \
    -e "PTRACE_PIP_V=${PTRACE_PIP_V:-0}" \
    -e "NPROC=${NPROC:-$(nproc)}" \
    "$IMAGE" \
    bash -c '
        set -euo pipefail
        cd /src

        if [ ! -f /opt/trace-python/bin/python3 ]; then
            git submodule update --init --recursive
            mkdir -p /opt/trace-python
            cd cpython
            ./configure --prefix=/opt/trace-python
            make -j$(nproc)
            make install
            cd ..
        fi

        bash scripts/trace-pip.sh vllm
    '
