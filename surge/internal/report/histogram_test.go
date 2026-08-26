package report

import (
	"testing"
	"time"
)

func TestRecorder_SnapshotPercentiles(t *testing.T) {
	r := NewRecorder()
	// 1..100ms, uniformly. p50 should land near 50ms, max near 100ms,
	// within HDR's documented precision tolerance for 3 significant figures.
	for i := 1; i <= 100; i++ {
		r.Record(time.Duration(i) * time.Millisecond)
	}

	snap := r.Snapshot()
	const tolerance = 2 * time.Millisecond
	if diff := absDuration(snap.P50 - 50*time.Millisecond); diff > tolerance {
		t.Fatalf("P50 = %v, want ~50ms (tolerance %v)", snap.P50, tolerance)
	}
	if diff := absDuration(snap.Max - 100*time.Millisecond); diff > tolerance {
		t.Fatalf("Max = %v, want ~100ms (tolerance %v)", snap.Max, tolerance)
	}
	if snap.P99 < snap.P95 || snap.P95 < snap.P50 {
		t.Fatalf("percentiles not monotonic: p50=%v p95=%v p99=%v", snap.P50, snap.P95, snap.P99)
	}
}

func TestRecorder_EncodeDecodeRoundTrip(t *testing.T) {
	r := NewRecorder()
	for i := 1; i <= 50; i++ {
		r.Record(time.Duration(i) * time.Millisecond)
	}
	want := r.Snapshot()

	encoded, err := r.EncodeCompressed()
	if err != nil {
		t.Fatalf("EncodeCompressed: %v", err)
	}
	if encoded == "" {
		t.Fatal("expected non-empty encoding")
	}

	decoded, err := DecodeAndMerge(encoded)
	if err != nil {
		t.Fatalf("DecodeAndMerge: %v", err)
	}

	const tolerance = int64(2000) // 2ms in microseconds
	if diff := absInt64(decoded.ValueAtPercentile(50) - want.P50.Microseconds()); diff > tolerance {
		t.Fatalf("decoded p50 = %dus, want ~%dus", decoded.ValueAtPercentile(50), want.P50.Microseconds())
	}
}

func TestDecodeAndMerge_CombinesDisjointHistograms(t *testing.T) {
	a := NewRecorder()
	for i := 1; i <= 10; i++ {
		a.Record(time.Duration(i) * time.Millisecond) // ~1-10ms
	}
	b := NewRecorder()
	for i := 91; i <= 100; i++ {
		b.Record(time.Duration(i) * time.Millisecond) // ~91-100ms
	}

	encA, err := a.EncodeCompressed()
	if err != nil {
		t.Fatalf("encode a: %v", err)
	}
	encB, err := b.EncodeCompressed()
	if err != nil {
		t.Fatalf("encode b: %v", err)
	}

	merged, err := DecodeAndMerge(encA, encB)
	if err != nil {
		t.Fatalf("DecodeAndMerge: %v", err)
	}

	if got := merged.TotalCount(); got != 20 {
		t.Fatalf("merged TotalCount = %d, want 20", got)
	}
	// The merged max should reflect b's range, not a's.
	if got := merged.Max(); got < 90_000 {
		t.Fatalf("merged Max = %dus, want >= 90000us", got)
	}
}

func TestDecodeAndMerge_NoInput(t *testing.T) {
	if _, err := DecodeAndMerge(); err == nil {
		t.Fatal("expected an error when merging zero histograms")
	}
}

func absDuration(d time.Duration) time.Duration {
	if d < 0 {
		return -d
	}
	return d
}

func absInt64(n int64) int64 {
	if n < 0 {
		return -n
	}
	return n
}
