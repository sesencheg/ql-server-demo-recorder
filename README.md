# ql-server-demo-recorder standalone v26

Native-only `.so` recorder for QLDS. It can be preloaded alongside minqlx or with no minqlx.

## Environment

```bash
export QL_DEMO_RECORDER_ENABLED=1
export QL_DEMO_RECORDER_PATH="/media/backup/mv/demos/demos"
export QL_DEMO_RECORDER_STOP_DELAY_MS=4050
```

Launcher:

```bash
RECORDER_SO="./ql-demo-recorder.so"
if [ -f "./minqlx.x64.so" ]; then
    export LD_PRELOAD="$RECORDER_SO:${LD_PRELOAD:-}:./minqlx.x64.so"
else
    export LD_PRELOAD="$RECORDER_SO:${LD_PRELOAD:-}"
fi

LD_LIBRARY_PATH="./linux64:${LD_LIBRARY_PATH:-}" \
    exec ./qzeroded.x64 +set zmq_stats_enable 1 "$@"
```

