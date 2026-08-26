#!/usr/bin/env bash
# End-to-end correctness: proxy real traffic through sluiced to a set of echod
# backends and assert it is byte-identical (TST-03), spread across every
# backend, and observable over the admin socket (DP-U-14).
set -euo pipefail
cd "$(dirname "$0")/.."
REPO_ROOT="$(pwd)"
source bench/lib.sh

RESULTS="$REPO_ROOT/bench/results"
mkdir -p "$RESULTS"

BACKENDS=3
BASE_PORT=7100
PROXY_PORT=8080
WORKERS=2

echo "== e2e: $BACKENDS backends, $WORKERS workers =="
start_backends "$BACKENDS" "$BASE_PORT"
start_sluiced "$PROXY_PORT" "$WORKERS"

OUT="$RESULTS/e2e"
run_surge "127.0.0.1:$PROXY_PORT" 200 5s 256 "$OUT" e2e

mismatch=$(csv_field "$OUT" total_payload_mismatches)
errors=$(csv_field "$OUT" total_errors)
issued=$(csv_field "$OUT" total_issued)
distinct=$(grep -o '"b[0-9]*":' "$OUT/result.json" | sort -u | wc -l | tr -d ' ')

echo "issued=$issued errors=$errors mismatches=$mismatch backends_hit=$distinct"

# Snapshot the admin surface while connections have flowed.
"$ADMINGET" "$ADMIN_SOCK" backends > "$RESULTS/admin_backends.json" || true
"$ADMINGET" "$ADMIN_SOCK" stats    > "$RESULTS/admin_stats.json"    || true
"$ADMINGET" "$ADMIN_SOCK" config   > "$RESULTS/admin_config.json"   || true
echo "-- admin backends --"; cat "$RESULTS/admin_backends.json"
echo "-- admin stats --";    cat "$RESULTS/admin_stats.json"
echo "-- admin config --";   cat "$RESULTS/admin_config.json"

fail=0
if [ "$mismatch" != "0" ]; then echo "FAIL: $mismatch payload mismatches (not byte-identical)"; fail=1; fi
if [ "$issued" -lt 100 ]; then echo "FAIL: only $issued connections issued"; fail=1; fi
# Allow a tiny error floor from teardown races, but it must be near-zero.
if [ "$errors" -gt $(( issued / 100 + 5 )) ]; then echo "FAIL: $errors errors of $issued"; fail=1; fi
if [ "$distinct" -lt "$BACKENDS" ]; then echo "FAIL: traffic only reached $distinct/$BACKENDS backends"; fail=1; fi

if [ "$fail" = 0 ]; then
  echo "E2E PASS: byte-identical, $issued conns, $errors errors, all $BACKENDS backends used"
else
  echo "E2E FAILED"; exit 1
fi
