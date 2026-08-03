#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")/.."

git -C cpython add -A
if [ -z "${1:-}" ]; then
    git -C cpython commit --amend --no-edit
else
    git -C cpython commit -s -m "$1"
fi
git -C cpython push -f
