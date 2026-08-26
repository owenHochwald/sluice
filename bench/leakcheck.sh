#!/usr/bin/env bash
# fd-stability under sustained connection churn (TST-05, NFR-03): push at least
# 100,000 connections through sluiced and assert its open file-descriptor count
# does not climb - i.e. every connection's two fds are closed exactly once.
set -euo pipefail
cd "$(dirname "$0")/.."
REPO_ROOT="$(pwd)"
source bench/lib.sh

RESULTS="$REPO_ROOT/bench/results"
mkdir -p "$RESULTS"
LOG="$RESULTS/leakcheck.txt"
: > "$LOG"

RATE="${RATE:-3000}"
PAYLOAD="${PAYLOAD:-256}"
BACKENDS="${BACKENDS:-3}"
WORKERS="${WORKERS:-$(nproc)}"
TARGET_CONNS="${TARGET_CONNS:-100000}"
BASE_PORT=7300
PROXY_PORT=8080

fdcount() { ls "/proc/$SLUICED_PID/fd" 2>/dev/null | wc -l | tr -d ' '; }

log() { echo "$*" | tee -a "$LOG"; }

log "== leakcheck: >= $TARGET_CONNS conns, rate=$RATE workers=$WORKERS =="
start_backends "$BACKENDS" "$BASE_PORT"
start_sluiced "$PROXY_PORT" "$WORKERS"

# Warm up so pools/threads have settled, then take the baseline fd count.
run_surge "127.0.0.1:$PROXY_PORT" "$RATE" 3s "$PAYLOAD" "$RESULTS/leak_warmup" leakwarm >/dev/null
sleep 2
fd_base=$(fdcount)
log "baseline fds after warmup: $fd_base"

total=0
pass=0
while [ "$total" -lt "$TARGET_CONNS" ]; do
  pass=$(( pass + 1 ))
  run_surge "127.0.0.1:$PROXY_PORT" "$RATE" 20s "$PAYLOAD" "$RESULTS/leak_pass" "leak$pass" >/dev/null
  issued=$(csv_field "$RESULTS/leak_pass" total_issued)
  total=$(( total + issued ))
  sleep 2
  fd_now=$(fdcount)
  log "pass $pass: issued=$issued cumulative=$total fds=$fd_now"
done

sleep 3
fd_final=$(fdcount)
log "final fds after drain: $fd_final (baseline $fd_base)"
log "total connections churned: $total"

# Allow a small constant slack (a few transient fds), but no growth trend.
slack=16
if [ "$fd_final" -gt $(( fd_base + slack )) ]; then
  log "LEAKCHECK FAILED: fd count grew from $fd_base to $fd_final over $total connections"
  exit 1
fi
log "LEAKCHECK PASS: fds stable ($fd_base -> $fd_final) across $total connections"
