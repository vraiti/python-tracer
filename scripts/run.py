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
    "tiny-random/Qwen-Image": {
        "args": [],
        "query": {
            "url": "http://localhost:8000/v1/images/generations",
            "body": {"prompt": "a red panda", "size": "256x256"},
        },
    },
}

_configs_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "configs")
CONFIGS = {
    dirent.name.rsplit(".", 1)[0]: dirent.path
    for dirent in os.scandir(_configs_dir)
    if dirent.is_file() and dirent.name.endswith(".yaml")
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
        "--config",
        default="default",
        choices=list(CONFIGS),
        help="Config file for code to trace",
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
    os.makedirs(os.path.dirname(output), exist_ok=True)

    config_path = CONFIGS[args.config]

    cmd = [
        "trace-python", "-m", "tracer",
        "--config", config_path,
        "--output", output,
        "--",
        "vllm-omni", "serve", args.model, "--omni",
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
