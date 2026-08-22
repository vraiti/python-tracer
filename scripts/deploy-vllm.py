#!/usr/bin/env python3
"""Start a vLLM-Omni server, exercise it once, and shut it down.

    scripts/deploy-vllm.py [--port PORT] [--log vllm.log] [--out out.png]

Runs `vllm serve --omni --enforce-eager tiny-random/Qwen-Image` with its
stdout/stderr redirected to the log file, mirrors the log to this script's
stdout through `tail -f`, and polls the server once per second:

  * if the server process exits before /health answers, exit 1;
  * once GET /health returns 200, send the text-to-image request from
    scripts/query-qwen-image.sh, then SIGTERM the server.

Exit status is 0 only if the request succeeded and the server terminated.
"""

from __future__ import annotations

import argparse
import http.client
import json
import os
import signal
import subprocess
import sys
import time

MODEL = "tiny-random/Qwen-Image"
REQUEST = {
    "prompt": "a cup of coffee on the table",
    "size": "256x256",
    "num_inference_steps": 4,
    "seed": 42,
    "response_format": "file",
}


def health(host: str, port: int) -> int | None:
    """HTTP status of GET /health, or None if the connection failed."""
    try:
        conn = http.client.HTTPConnection(host, port, timeout=2)
        conn.request("GET", "/health")
        status = conn.getresponse().status
        conn.close()
        return status
    except OSError:
        return None


def generate_image(host: str, port: int, out: str) -> None:
    conn = http.client.HTTPConnection(host, port, timeout=600)
    conn.request(
        "POST",
        "/v1/images/generations",
        body=json.dumps(REQUEST),
        headers={"Content-Type": "application/json"},
    )
    resp = conn.getresponse()
    body = resp.read()
    conn.close()
    if resp.status != 200:
        raise RuntimeError(
            f"image request failed: HTTP {resp.status}: {body[:500]!r}"
        )
    with open(out, "wb") as f:
        f.write(body)
    print(f"saved: {out} ({len(body)} bytes)", flush=True)


def stop(proc: subprocess.Popen, sig: int, timeout: float) -> int | None:
    if proc.poll() is None:
        proc.send_signal(sig)
        try:
            proc.wait(timeout)
        except subprocess.TimeoutExpired:
            pass
    return proc.poll()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--log", default="vllm.log")
    ap.add_argument("--out", default="out.png")
    ap.add_argument("--timeout", type=float, default=1800,
                    help="seconds to wait for /health before giving up")
    args = ap.parse_args()

    cmd = ["vllm", "serve", "--omni", "--enforce-eager", MODEL,
           "--host", args.host, "--port", str(args.port)]

    log = open(args.log, "wb")
    # Own process group so SIGTERM reaches vLLM's worker children as well.
    t_start = time.monotonic()
    server = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT,
                              start_new_session=True)
    log.close()
    print(f"started pid {server.pid}: {' '.join(cmd)}", flush=True)

    tail = subprocess.Popen(["tail", "-n", "+1", "-f", args.log],
                            stdout=sys.stdout, stderr=sys.stderr)

    status = 1
    t_healthy = t_request = None
    try:
        deadline = time.monotonic() + args.timeout
        while True:
            rc = server.poll()
            if rc is not None:
                print(f"vllm exited with status {rc} before /health responded",
                      file=sys.stderr, flush=True)
                return 1
            if health(args.host, args.port) == 200:
                t_healthy = time.monotonic()
                break
            if time.monotonic() > deadline:
                print("timed out waiting for /health", file=sys.stderr, flush=True)
                return 1
            time.sleep(1)

        print("server healthy; sending image request", flush=True)
        try:
            generate_image(args.host, args.port, args.out)
            t_request = time.monotonic()
            status = 0
        except Exception as e:
            print(str(e), file=sys.stderr, flush=True)
            status = 1
    finally:
        print(f"sending SIGTERM to pid {server.pid}", flush=True)
        if stop(server, signal.SIGTERM, 60) is None:
            print("server ignored SIGTERM; killing", file=sys.stderr, flush=True)
            os.killpg(server.pid, signal.SIGKILL)
            server.wait()
        t_end = time.monotonic()
        print(f"server exited with status {server.returncode}", flush=True)
        tail.terminate()
        tail.wait()
        # Phase times from vLLM process start, for comparing tracer overhead.
        print(f"timing: startup->healthy {t_healthy - t_start:.1f}s"
              if t_healthy else "timing: startup->healthy n/a", flush=True)
        if t_request:
            print(f"timing: request {t_request - t_healthy:.1f}s", flush=True)
        print(f"timing: e2e start->exit {t_end - t_start:.1f}s", flush=True)
    return status


if __name__ == "__main__":
    sys.exit(main())
