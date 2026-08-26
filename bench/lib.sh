#!/usr/bin/env bash
# Shared setup for the sluice benchmark scripts. Sourced, not executed.
#
# Everything here runs inside one Linux container (see bench/Dockerfile via the
# Makefile), so echod, sluiced, and surge all talk over localhost. The C++
# binary is built by `make build-cpp`; the Go binaries are cross-compiled to
# Linux by the Makefile before the container starts.
set -euo pipefail

REPO_ROOT="${REPO_ROOT:-$(pwd)}"
SLUICED="${SLUICED:-$REPO_ROOT/sluiced/build/sluiced}"
ECHOD="${ECHOD:-$REPO_ROOT/bench/bin/echod}"
SURGE="${SURGE:-$REPO_ROOT/bench/bin/surge}"
ADMINGET="${ADMINGET:-$REPO_ROOT/bench/bin/adminget}"
ADMIN_SOCK="${ADMIN_SOCK:-/tmp/sluiced.sock}"

# PIDs we start, torn down by cleanup() on any exit.
_PIDS=()
BACKEND_PORTS=()
BACKEND_IDS=()

cleanup() {
  local pid
  for pid in "${_PIDS[@]:-}"; do
    kill "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
  rm -f "$ADMIN_SOCK" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# wait_port HOST PORT [TIMEOUT_S] — block until a TCP connect succeeds.
wait_port() {
  local host="$1" port="$2" timeout="${3:-10}" deadline
  deadline=$(( $(date +%s) + timeout ))
  until bash -c "exec 3<>/dev/tcp/$host/$port" 2>/dev/null; do
    if [ "$(date +%s)" -ge "$deadline" ]; then
      echo "wait_port: $host:$port never came up" >&2
      return 1
    fi
    sleep 0.1
  done
  exec 3>&- 2>/dev/null || true
}

# start_backends N BASE_PORT [BOOTSTRAP_FILE] — launch N echod instances on
# consecutive ports and write their addresses to the bootstrap file.
start_backends() {
  local n="$1" base="$2" bootstrap="${3:-$REPO_ROOT/bench/backends.conf}"
  : > "$bootstrap"
  BACKEND_PORTS=()
  BACKEND_IDS=()
  local i port admin_port id
  for (( i=0; i<n; i++ )); do
    port=$(( base + i ))
    admin_port=$(( base + 1000 + i ))
    id="b$i"
    "$ECHOD" -listen "127.0.0.1:$port" -admin-listen "127.0.0.1:$admin_port" -id "$id" \
      >"$REPO_ROOT/bench/results/echod-$id.log" 2>&1 &
    _PIDS+=("$!")
    BACKEND_PORTS+=("$port")
    BACKEND_IDS+=("$id")
    echo "127.0.0.1:$port" >> "$bootstrap"
    wait_port 127.0.0.1 "$port"
  done
  echo "started $n echod backends on ports ${BACKEND_PORTS[*]}"
}

# start_sluiced PROXY_PORT WORKERS [BOOTSTRAP_FILE] [EXTRA_ARGS...]
start_sluiced() {
  local proxy_port="$1" workers="$2" bootstrap="${3:-$REPO_ROOT/bench/backends.conf}"
  shift 2
  if [ $# -ge 1 ]; then shift; fi  # drop the optional bootstrap positional if given
  rm -f "$ADMIN_SOCK"
  "$SLUICED" --listen-port "$proxy_port" --workers "$workers" \
    --bootstrap "$bootstrap" --admin-socket "$ADMIN_SOCK" \
    --health-interval-ms 1000 "$@" \
    >"$REPO_ROOT/bench/results/sluiced.log" 2>&1 &
  _PIDS+=("$!")
  SLUICED_PID="$!"
  wait_port 127.0.0.1 "$proxy_port"
  # Give the admin thread a beat to bind its socket.
  local t=0
  until [ -S "$ADMIN_SOCK" ] || [ "$t" -ge 50 ]; do sleep 0.1; t=$(( t + 1 )); done
  echo "started sluiced on :$proxy_port with $workers workers (pid $SLUICED_PID)"
}

# run_surge TARGET RATE DURATION PAYLOAD OUTDIR [REPLICA_ID]
# Runs one surge benchmark to completion and leaves its artifacts in OUTDIR.
# surge keeps serving HTTP after the run, so we cap it with a timeout; the
# result files are written before it starts serving.
run_surge() {
  local target="$1" rate="$2" duration="$3" payload="$4" outdir="$5" id="${6:-surge}"
  mkdir -p "$outdir"
  local cap=$(( ${duration%s} + 15 ))
  timeout -s INT "${cap}s" "$SURGE" \
    -target "$target" -rate "$rate" -duration "$duration" -payload-size "$payload" \
    -results-dir "$outdir" -id "$id" -admin-listen "127.0.0.1:0" \
    >"$outdir/surge.log" 2>&1 || true
  if [ ! -f "$outdir/summary.csv" ]; then
    echo "run_surge: no summary.csv produced (see $outdir/surge.log)" >&2
    return 1
  fi
}

# csv_field OUTDIR COLUMN_NAME — pull one field from a surge summary.csv.
# Values are simple (numbers, ids, host:port) so a plain comma split is safe.
csv_field() {
  local outdir="$1" col="$2" header line i
  IFS= read -r header < "$outdir/summary.csv"
  line=$(sed -n '2p' "$outdir/summary.csv")
  local -a H V
  IFS=',' read -ra H <<< "$header"
  IFS=',' read -ra V <<< "$line"
  for i in "${!H[@]}"; do
    if [ "${H[$i]}" = "$col" ]; then echo "${V[$i]}"; return 0; fi
  done
  return 1
}
