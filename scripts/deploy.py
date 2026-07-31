#!/usr/bin/env python3
import os
import subprocess
import sys

SCRIPTS_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPTS_DIR)

def out(cmd):
    return subprocess.check_output(
        cmd, shell=True, cwd=PROJECT_DIR, text=True,
        stdin=sys.stdin, stderr=sys.stderr,
    ).strip()

def run(cmd):
    subprocess.check_call(
        cmd, shell=True, cwd=PROJECT_DIR,
        stdin=sys.stdin, stdout=sys.stdout, stderr=sys.stderr,
    )

head_before = out("git rev-parse HEAD")
run("git pull --recurse-submodules")
head_after = out("git rev-parse HEAD")

if head_before != head_after:
    run("make install")

run_py = os.path.join(SCRIPTS_DIR, "run.py")
os.execvp("trace-python", ["trace-python", run_py, *sys.argv[1:]])
