#!/usr/bin/env bash
# Wait for a server to answer on /health, or for its process to die.
#
#   poll-health-pid.sh ADDRESS PID
#
# ADDRESS is the server's base URL (e.g. http://127.0.0.1:8000) or host:port.
# Exits 0 as soon as GET ADDRESS/health returns any HTTP status code, and 1 as
# soon as PID no longer exists. Polls once per second until one of the two
# happens.
set -u

if [ $# -ne 2 ]; then
    echo "usage: $0 ADDRESS PID" >&2
    exit 2
fi

address="$1"
pid="$2"
case "$address" in
    http://*|https://*) ;;
    *) address="http://$address" ;;
esac
url="${address%/}/health"

while :; do
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "process $pid exited before $url responded" >&2
        exit 1
    fi
    # Any HTTP status counts as a response; only a failed connection does not.
    code=$(curl --silent --output /dev/null --write-out '%{http_code}' \
                --max-time 2 "$url" 2>/dev/null)
    if [ -n "$code" ] && [ "$code" != "000" ]; then
        echo "$url responded with HTTP $code"
        exit 0
    fi
    sleep 1
done
