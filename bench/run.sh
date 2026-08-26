#!/usr/bin/env bash
# Headline benchmark, in two honest phases:
#
#   Phase 1 - proxy overhead (NFR-02): the SAME single echod backend in both
#   paths, so the only difference is the sluiced hop. surge -> echod is the
#   baseline; surge -> sluiced -> echod is the measured path. The delta is the
#   latency sluiced actually adds.
#
#   Phase 2 - load balancing / throughput (NFR-01, LG-U-05): surge through
#   sluiced across a pool of echod backends, reporting achieved rate, error
#   count, byte-identicalness, and how evenly the pool was used.
#
# Open-loop scheduling with latency from intended send time (no coordinated
# omission). Writes CSV/JSON artifacts and SUMMARY.md under bench/results/.
set -euo pipefail
cd "$(dirname "$0")/.."
REPO_ROOT="$(pwd)"
source bench/lib.sh

RESULTS="$REPO_ROOT/bench/results"
mkdir -p "$RESULTS"

RATE="${RATE:-2000}"
DURATION="${DURATION:-20s}"
PAYLOAD="${PAYLOAD:-512}"
POOL="${POOL:-4}"
WORKERS="${WORKERS:-$(nproc)}"
# Phase 1 runs below saturation so the single backend's own tail doesn't swamp
# the proxy hop we're trying to isolate.
OVERHEAD_RATE="${OVERHEAD_RATE:-600}"
OVERHEAD_DURATION="${OVERHEAD_DURATION:-20s}"

echo "== bench: rate=$RATE dur=$DURATION payload=$PAYLOAD pool=$POOL workers=$WORKERS =="

# --- Phase 1: proxy overhead, one backend shared by both paths ---------------
ADMIN_SOCK=/tmp/sluiced-p1.sock
start_backends 1 7200 "$REPO_ROOT/bench/backends-p1.conf"
start_sluiced 8080 "$WORKERS" "$REPO_ROOT/bench/backends-p1.conf"

# Warm both paths first so neither run pays cold-start (Go GC ramp, page
# faults) in its measured tail.
echo "-- phase 1 warmup --"
run_surge "127.0.0.1:7200" "$OVERHEAD_RATE" 3s "$PAYLOAD" "$RESULTS/warmup_d" warm >/dev/null
run_surge "127.0.0.1:8080" "$OVERHEAD_RATE" 3s "$PAYLOAD" "$RESULTS/warmup_p" warm >/dev/null

echo "-- phase 1 baseline: direct to the backend --"
run_surge "127.0.0.1:7200" "$OVERHEAD_RATE" "$OVERHEAD_DURATION" "$PAYLOAD" "$RESULTS/direct" direct
echo "-- phase 1 measured: through sluiced to the same backend --"
run_surge "127.0.0.1:8080" "$OVERHEAD_RATE" "$OVERHEAD_DURATION" "$PAYLOAD" "$RESULTS/proxied1" proxied1

# --- Phase 2: load balancing across a pool -----------------------------------
ADMIN_SOCK=/tmp/sluiced-p2.sock
start_backends "$POOL" 7210 "$REPO_ROOT/bench/backends-p2.conf"
start_sluiced 8081 "$WORKERS" "$REPO_ROOT/bench/backends-p2.conf"

echo "-- phase 2: through sluiced across $POOL backends --"
run_surge "127.0.0.1:8081" "$RATE" "$DURATION" "$PAYLOAD" "$RESULTS/proxied_pool" proxiedpool
"$ADMINGET" "$ADMIN_SOCK" stats  > "$RESULTS/admin_stats.json"  || true
"$ADMINGET" "$ADMIN_SOCK" config > "$RESULTS/admin_config.json" || true
"$ADMINGET" "$ADMIN_SOCK" backends > "$RESULTS/admin_backends.json" || true

# --- Pull the numbers --------------------------------------------------------
d_p50=$(csv_field "$RESULTS/direct" p50_us);    x_p50=$(csv_field "$RESULTS/proxied1" p50_us)
d_p95=$(csv_field "$RESULTS/direct" p95_us);    x_p95=$(csv_field "$RESULTS/proxied1" p95_us)
d_p99=$(csv_field "$RESULTS/direct" p99_us);    x_p99=$(csv_field "$RESULTS/proxied1" p99_us)
d_rate=$(csv_field "$RESULTS/direct" achieved_rate)
x_rate=$(csv_field "$RESULTS/proxied1" achieved_rate)

pool_rate=$(csv_field "$RESULTS/proxied_pool" achieved_rate)
pool_rl=$(csv_field "$RESULTS/proxied_pool" rate_limited)
pool_iss=$(csv_field "$RESULTS/proxied_pool" total_issued)
pool_err=$(csv_field "$RESULTS/proxied_pool" total_errors)
pool_mis=$(csv_field "$RESULTS/proxied_pool" total_payload_mismatches)
pool_p50=$(csv_field "$RESULTS/proxied_pool" p50_us)
pool_p99=$(csv_field "$RESULTS/proxied_pool" p99_us)

# Per-backend distribution from the pool run's result.json (LG-U-05). surge
# pretty-prints, so allow whitespace after the colon.
dist=$(grep -oE '"b[0-9]+": *[0-9]+' "$RESULTS/proxied_pool/result.json" | tr -d ' ' | tr '\n' ' ')

# Combined CSV for plotting across sweeps.
{
  head -n1 "$RESULTS/direct/summary.csv"
  tail -n1 "$RESULTS/direct/summary.csv"
  tail -n1 "$RESULTS/proxied1/summary.csv"
  tail -n1 "$RESULTS/proxied_pool/summary.csv"
} > "$RESULTS/summary.csv"

cat > "$RESULTS/SUMMARY.md" <<EOF
# sluice benchmark results

Reproduce: \`make bench\` (env-overridable: RATE, DURATION, PAYLOAD, POOL, WORKERS).

- phase-2 pool rate: $RATE conn/s (per surge replica), duration $DURATION; phase-1 overhead rate: $OVERHEAD_RATE conn/s
- payload: $PAYLOAD bytes echoed per connection
- sluiced workers (event loops): $WORKERS
- open-loop scheduling, latency from intended send time (no coordinated omission)

## Phase 1 - proxy overhead (same single backend both paths)

Run below saturation at $OVERHEAD_RATE conn/s for $OVERHEAD_DURATION, warmed
first. The only difference between the two rows is the sluiced hop, so the
delta is the latency sluiced adds (NFR-02).

| connect latency (us) | direct | through sluiced | added |
|----------------------|-------:|----------------:|------:|
| p50 | $d_p50 | $x_p50 | $(( x_p50 - d_p50 )) |
| p95 | $d_p95 | $x_p95 | $(( x_p95 - d_p95 )) |
| p99 | $d_p99 | $x_p99 | $(( x_p99 - d_p99 )) |

Achieved rate: direct $d_rate/s, proxied $x_rate/s.

## Phase 2 - load balancing across $POOL backends

- achieved rate: $pool_rate conn/s (generator-rate-limited: $pool_rl)
- connections issued: $pool_iss, errors: $pool_err, payload mismatches: $pool_mis
- latency through the pool: p50 $pool_p50 us, p99 $pool_p99 us
- per-backend connection distribution (Maglev): $dist

Raw artifacts under \`bench/results/\`: direct/, proxied1/, proxied_pool/
(summary.csv, latency_percentiles.csv, result.json, histogram.b64), plus the
admin snapshot admin_stats.json / admin_config.json / admin_backends.json.
EOF

echo
cat "$RESULTS/SUMMARY.md"
echo
[ "$pool_mis" = "0" ] || { echo "WARNING: $pool_mis payload mismatches through the proxy" >&2; }
echo "bench complete -> $RESULTS/SUMMARY.md"
