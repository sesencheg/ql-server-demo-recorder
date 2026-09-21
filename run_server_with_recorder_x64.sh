#!/bin/bash
cd "$(dirname "$0")"

export QL_DEMO_RECORDER_ENABLED=1
export QL_DEMO_RECORDER_PATH="/qlds/demos"
export QL_DEMO_RECORDER_STOP_DELAY_MS=4050

RECORDER_SO="./ql-demo-recorder.so"

if [ -f "./minqlx.x64.so" ]; then
    export LD_PRELOAD="$RECORDER_SO:${LD_PRELOAD:-}:./minqlx.x64.so"
else
    export LD_PRELOAD="$RECORDER_SO:${LD_PRELOAD:-}"
fi

LD_LIBRARY_PATH="./linux64:${LD_LIBRARY_PATH:-}" \
    exec ./qzeroded.x64 +set zmq_stats_enable 1 "$@"