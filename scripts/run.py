#!/usr/bin/env python3
import argparse
import json
import os
import signal
import subprocess
import sys
import time
import urllib.request

MODELS = {
    "Tongyi-MAI/Z-Image-Turbo": {
        "args": [],
        "query": {
            "url": "http://localhost:8000/v1/images/generations",
            "body": {"prompt": "a red panda sitting on a park bench", "size": "512x512"},
        },
    },
    "Qwen/Qwen3-Omni-30B-A3B-Instruct": {
        "args": ["--quantization", "fp8"],
    },
}

HEALTH_URL = "http://localhost:8000/health"
POLL_INTERVAL = 10


def poll_health(server):
    start = time.monotonic()
    while True:
        rc = server.poll()
        if rc is not None:
            return False

        try:
            resp = urllib.request.urlopen(HEALTH_URL, timeout=3)
            if resp.status == 200:
                return True
        except Exception:
            pass

        time.sleep(POLL_INTERVAL)


def main():
    parser = argparse.ArgumentParser(description="Run omni_tracer with vLLM-Omni")
    parser.add_argument(
        "model",
        choices=list(MODELS),
        help="Model to serve",
    )
    parser.add_argument(
        "--taint-notrace",
        action="append",
        default=["_dummy_run"],
        help="Suppress tracing inside functions matching this qualname substring (repeatable)",
    )
    parser.add_argument(
        "--prefix",
        action="append",
        default=None,
        help="Scope prefix for tracing (repeatable; omit to auto-detect vllm_omni, vllm, janus)",
    )
    parser.add_argument(
        "--no-query",
        action="store_true",
        help="Skip sending a default request before terminating",
    )
    args = parser.parse_args()

    model = MODELS[args.model]
    base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    output = os.path.join(base, "traces", "trace.db")
    track_file = os.path.join(base, "tracked.txt")
    os.makedirs(os.path.dirname(output), exist_ok=True)

    taint_args = []
    if args.taint_notrace:
        for pat in args.taint_notrace:
            taint_args.extend(["--taint-notrace", pat])

    prefix_args = []
    prefixes = args.prefix
    for p in prefixes:
        prefix_args.extend(["--prefix", p])

    cmd = [
        "trace-python", "-m", "tracer",
        "--output", output,
        *(["--tracked", track_file] if os.path.exists(track_file) else []),
        *taint_args,
        *prefix_args,
        "--",
        "serve", args.model, "--omni",
        *model["args"],
    ]

    server = subprocess.Popen(
        cmd, start_new_session=True, cwd="/",
        stdin=sys.stdin, stdout=sys.stdout, stderr=sys.stderr,
    )

    try:
        if not poll_health(server):
            server.wait()
            sys.exit(server.returncode or 1)

        if not args.no_query:
            query = model.get("query")
            if query:
                data = json.dumps(query["body"]).encode()
                req = urllib.request.Request(
                    query["url"],
                    data=data,
                    headers={"Content-Type": "application/json"},
                )
                try:
                    urllib.request.urlopen(req, timeout=120)
                except Exception:
                    pass

        os.killpg(server.pid, signal.SIGTERM)
        server.wait()
    except KeyboardInterrupt:
        os.killpg(server.pid, signal.SIGTERM)
        server.wait()
        sys.exit(1)

    sys.exit(server.returncode or 0)


if __name__ == "__main__":
    main()
