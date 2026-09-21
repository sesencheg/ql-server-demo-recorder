#!/bin/bash
set -e
cd "$(dirname "$0")"

export QL_DEMO_RECORDER_ENABLED="${QL_DEMO_RECORDER_ENABLED:-1}"
export QL_DEMO_RECORDER_PATH="${QL_DEMO_RECORDER_PATH:-/media/backup/mv/demos/demos}"
export QL_DEMO_RECORDER_STOP_DELAY_MS="${QL_DEMO_RECORDER_STOP_DELAY_MS:-4050}"

RECORDER_SO="./ql-server-demo-recorder.so"

if [ ! -f "$RECORDER_SO" ]; then
    echo "Missing $RECORDER_SO; run: make" >&2
    exit 1
fi

PRELOAD="$RECORDER_SO"
if [ -n "${LD_PRELOAD:-}" ]; then
    PRELOAD="$PRELOAD:$LD_PRELOAD"
fi
if [ -f "./minqlx.x64.so" ]; then
    PRELOAD="$PRELOAD:./minqlx.x64.so"
fi
export LD_PRELOAD="$PRELOAD"

LD_LIBRARY_PATH="./linux64:${LD_LIBRARY_PATH:-}" \
    exec ./qzeroded.x64 +set zmq_stats_enable 1 "$@"
