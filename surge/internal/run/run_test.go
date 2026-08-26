package run

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/owenhochwald/sluice/surge/internal/report"
	"github.com/owenhochwald/sluice/surge/internal/testutil"
)

func TestExecute_AgainstFakeEcho(t *testing.T) {
	fake, err := testutil.StartFakeEcho("backend-1")
	if err != nil {
		t.Fatalf("start fake echo: %v", err)
	}
	defer fake.Close()

	opts := Options{
		Target:       fake.Addr(),
		Rate:         200,
		Duration:     100 * time.Millisecond,
		PayloadSize:  16,
		MaxInFlight:  50,
		DialTimeout:  time.Second,
		IOTimeout:    time.Second,
		LagThreshold: 50 * time.Millisecond,
		ReplicaID:    "test-0",
	}

	result := Execute(context.Background(), opts)

	if result.TotalIssued == 0 {
		t.Fatal("expected TotalIssued > 0")
	}
	if result.TotalErrors != 0 {
		t.Fatalf("unexpected errors: %d", result.TotalErrors)
	}
	if result.Cancelled {
		t.Fatal("expected Cancelled = false for a run that completed its schedule")
	}
	if len(result.InstanceDistribution) != 1 || result.InstanceDistribution["backend-1"] == 0 {
		t.Fatalf("InstanceDistribution = %v, want backend-1 attributed", result.InstanceDistribution)
	}
	if result.HistogramB64 == "" {
		t.Fatal("expected a non-empty histogram encoding")
	}
	if result.Percentiles.Max <= 0 {
		t.Fatalf("Percentiles.Max = %v, want > 0", result.Percentiles.Max)
	}
}

func TestExecute_WritesArtifacts(t *testing.T) {
	fake, err := testutil.StartFakeEcho("backend-1")
	if err != nil {
		t.Fatalf("start fake echo: %v", err)
	}
	defer fake.Close()

	dir := t.TempDir()
	opts := Options{
		Target:       fake.Addr(),
		Rate:         100,
		Duration:     50 * time.Millisecond,
		PayloadSize:  8,
		MaxInFlight:  20,
		DialTimeout:  time.Second,
		IOTimeout:    time.Second,
		LagThreshold: 50 * time.Millisecond,
		ReplicaID:    "test-1",
		ResultsDir:   dir,
	}

	Execute(context.Background(), opts)

	for _, name := range []string{"summary.csv", "latency_percentiles.csv", "result.json", "histogram.b64"} {
		if _, err := os.Stat(filepath.Join(dir, name)); err != nil {
			t.Fatalf("expected artifact %s: %v", name, err)
		}
	}

	data, err := os.ReadFile(filepath.Join(dir, "result.json"))
	if err != nil {
		t.Fatalf("read result.json: %v", err)
	}
	var got report.RunResult
	if err := json.Unmarshal(data, &got); err != nil {
		t.Fatalf("unmarshal result.json: %v", err)
	}
	if got.ReplicaID != "test-1" {
		t.Fatalf("ReplicaID = %q, want test-1", got.ReplicaID)
	}
}

func TestExecute_CancelledContextReturnsPartialResult(t *testing.T) {
	fake, err := testutil.StartFakeEcho("backend-1")
	if err != nil {
		t.Fatalf("start fake echo: %v", err)
	}
	defer fake.Close()

	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	opts := Options{
		Target:       fake.Addr(),
		Rate:         100,
		Duration:     time.Second,
		PayloadSize:  8,
		MaxInFlight:  20,
		DialTimeout:  time.Second,
		IOTimeout:    time.Second,
		LagThreshold: 50 * time.Millisecond,
		ReplicaID:    "test-2",
	}

	result := Execute(ctx, opts)
	if !result.Cancelled {
		t.Fatal("expected Cancelled = true")
	}
}
