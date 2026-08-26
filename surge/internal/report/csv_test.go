package report

import (
	"strings"
	"testing"
	"time"
)

func TestWriteSummaryCSV(t *testing.T) {
	r := RunResult{
		ReplicaID:              "surge-0",
		Target:                 "echod:7000",
		ConfiguredRate:         100,
		AchievedRate:           98.5,
		RateLimited:            false,
		Duration:               30 * time.Second,
		TotalIssued:            2955,
		TotalErrors:            3,
		TotalPayloadMismatches: 0,
		Percentiles: Percentiles{
			P50:  5 * time.Millisecond,
			P95:  12 * time.Millisecond,
			P99:  20 * time.Millisecond,
			P999: 50 * time.Millisecond,
			Max:  80 * time.Millisecond,
		},
	}

	var buf strings.Builder
	if err := WriteSummaryCSV(&buf, r); err != nil {
		t.Fatalf("WriteSummaryCSV: %v", err)
	}

	want := "replica_id,target,configured_rate,achieved_rate,rate_limited,duration_s,total_issued,total_errors,total_payload_mismatches,p50_us,p95_us,p99_us,p999_us,max_us\n" +
		"surge-0,echod:7000,100,98.5,false,30,2955,3,0,5000,12000,20000,50000,80000\n"
	if got := buf.String(); got != want {
		t.Fatalf("got:\n%s\nwant:\n%s", got, want)
	}
}

func TestWritePercentileCSV(t *testing.T) {
	rec := NewRecorder()
	for i := 1; i <= 100; i++ {
		rec.Record(time.Duration(i) * time.Millisecond)
	}

	var buf strings.Builder
	if err := WritePercentileCSV(&buf, rec); err != nil {
		t.Fatalf("WritePercentileCSV: %v", err)
	}

	lines := strings.Split(strings.TrimSpace(buf.String()), "\n")
	if lines[0] != "percentile,value_us" {
		t.Fatalf("header = %q", lines[0])
	}
	if len(lines) != len(percentilePoints)+1 {
		t.Fatalf("got %d lines, want %d", len(lines), len(percentilePoints)+1)
	}
}
