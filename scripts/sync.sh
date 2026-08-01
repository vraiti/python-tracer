#!/bin/bash
set -euo pipefail

git add -A
if [ -z "${1:-}" ]; then
    git commit --amend --no-edit
else
    git commit -m "$1"
fi
git push -f

ssh wsl 'cd /home/vraiti/python-tracer && git fetch origin && git reset --hard origin/main'
