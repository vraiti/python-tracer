#!/usr/bin/env bash
# Single text-to-image request against a vLLM-Omni server running
# tiny-random/Qwen-Image (or any diffusion model served with `--omni`).
#
#   vllm serve tiny-random/Qwen-Image --omni --port 8000 --enforce-eager
#   scripts/query-qwen-image.sh [output.png]
#
# HOST/PORT override the server address. The image is requested small and
# with few steps: the model's weights are random, so the output is noise and
# only the request path matters.
set -euo pipefail

HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-8000}"
OUT="${1:-out.png}"

curl --fail --silent --show-error \
     -o "$OUT" \
     -X POST "http://${HOST}:${PORT}/v1/images/generations" \
     -H "Content-Type: application/json" \
     -d '{
           "prompt": "a cup of coffee on the table",
           "size": "256x256",
           "num_inference_steps": 4,
           "seed": 42,
           "response_format": "file"
         }'

echo "saved: $OUT"
