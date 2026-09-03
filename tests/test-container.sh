#!/usr/bin/env bash
set -euo pipefail

# Runs `make test`'s build+test steps inside a container (Containerfile.test)
# rather than directly on the host. cpython/ is overlaid with its own
# upperdir/workdir -- separate from rebuild-d3g.sh's overlay against
# Containerfile -- so this build never shares, or clobbers, the UBI10 build's
# cached ./configure state: the two Containerfiles' toolchains disagree on
# things like readline/gdbm support, so a shared cpython/Makefile would
# silently pick up whichever container configured it last (see
# rebuild-d3g.sh's own comment for the concrete failure mode this caused).
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_TAG="python-tracer-tester"

podman build -t "$IMAGE_TAG" -f "$ROOT/Containerfile.test" "$ROOT"

mkdir -p "$ROOT/.cargo-cache" \
    "$ROOT/.container-overlay/cpython-test-upper" \
    "$ROOT/.container-overlay/cpython-test-work"
podman run --rm \
    -v "$ROOT:/src:Z" \
    -v "$ROOT/cpython:/src/cpython:O,upperdir=$ROOT/.container-overlay/cpython-test-upper,workdir=$ROOT/.container-overlay/cpython-test-work" \
    -v "$ROOT/.cargo-cache:/root/.cargo/registry:Z" \
    -w /src \
    "$IMAGE_TAG"
